// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>
#include <string_view>
#include <fmt/format.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/client_session.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/thread.h"
#include "core/memory.h"
#include "core/section_replay.h"
#include "core/store_journal.h"
#include "video_core/gpu.h"

namespace Core {
namespace {

constexpr u32 SleepThread = 0x0A;
constexpr u32 ArbitrateAddress = 0x22;
constexpr u32 SendSyncRequest = 0x32;
// The IPC command buffer in a thread's TLS.
constexpr VAddr CommandBuffer = 0x80;
// gsp::Gpu FlushDataCache's request header and its reply's.
constexpr std::string_view GspSession = "gsp::Gpu_Server_Client";
constexpr u32 FlushDataCacheRequest = 0x00080082;
constexpr u32 FlushDataCacheReply = 0x00080040;

// ARM B, BL or BLX register, always executed.
struct Branch {
    VAddr target;
    bool link;
    // BLX: the register holding the target.
    std::optional<u32> reg;
};

std::optional<Branch> DecodeBranch(VAddr address, u32 instruction) {
    if ((instruction & 0xFFFFFFF0) == 0xE12FFF30) {
        return Branch{0, true, instruction & 0xF};
    }
    if ((instruction & 0xFE000000) != 0xEA000000) {
        return std::nullopt;
    }
    const s32 offset = static_cast<s32>(instruction << 8) >> 6;
    return Branch{static_cast<VAddr>(address + 8 + offset), (instruction & 0x01000000) != 0, {}};
}

} // namespace

SectionReplay::SectionReplay(System& system_) : system(system_) {}

SectionReplay::~SectionReplay() {
    if (section) {
        SetJournal(nullptr);
    }
}

void SectionReplay::Reset() {
    // The memory system these journals saved into is gone; nothing of it is touched.
    section = std::make_unique<Memory::StoreJournal>(system.Memory());
    replay = std::make_unique<Memory::StoreJournal>(system.Memory());
    window_open = false;
    pending = false;
    window_cpu = nullptr;
    replaying = false;
}

bool SectionReplay::Install(VAddr open_site_, VAddr close_site_, Handler handler_) {
    for (const VAddr site : {open_site_, close_site_}) {
        if (!DecodeBranch(site, system.Memory().Read32(site))) {
            LOG_ERROR(Core, "section site {:08X} is not an ARM B, BL or BLX", site);
            return false;
        }
    }
    Uninstall();
    open_site = open_site_;
    close_site = close_site_;
    handler = std::move(handler_);
    hooks[open_site] = Hook::Open;
    hooks[close_site] = Hook::Close;
    for (u32 core = 0; core < system.GetNumCores(); ++core) {
        system.GetCore(core).InvalidateCacheRange(open_site, 4);
        system.GetCore(core).InvalidateCacheRange(close_site, 4);
    }
    return true;
}

bool SectionReplay::Observe(VAddr site) {
    if (open_site == 0 || !DecodeBranch(site, system.Memory().Read32(site))) {
        LOG_ERROR(Core,
                  "observed site {:08X} is not an ARM B, BL or BLX, or no section is installed",
                  site);
        return false;
    }
    hooks[site] = Hook::Observe;
    observed_sites.push_back(site);
    for (u32 core = 0; core < system.GetNumCores(); ++core) {
        system.GetCore(core).InvalidateCacheRange(site, 4);
    }
    return true;
}

void SectionReplay::Uninstall() {
    if (open_site == 0) {
        return;
    }
    observed_sites.push_back(open_site);
    observed_sites.push_back(close_site);
    for (const VAddr site : observed_sites) {
        hooks.erase(site);
        for (u32 core = 0; core < system.GetNumCores(); ++core) {
            system.GetCore(core).InvalidateCacheRange(site, 4);
        }
    }
    observed_sites.clear();
    open_site = 0;
    close_site = 0;
    handler = nullptr;
}

std::optional<u32> SectionReplay::CodeAt(VAddr vaddr) const {
    const auto it = hooks.find(vaddr);
    if (it == hooks.end()) {
        return std::nullopt;
    }
    return 0xEF000000 | (HookSvcBase + static_cast<u32>(it->second));
}

SectionReplay::Svc SectionReplay::DescribeSvc(ARM_Interface& cpu, const Kernel::Thread& thread,
                                              u32 immediate) {
    Svc svc{immediate, cpu.GetPC() - 4, cpu.GetReg(14), {}, 0, 0};
    if (immediate == ArbitrateAddress) {
        svc.address = cpu.GetReg(1);
        svc.detail = cpu.GetReg(2);
        return svc;
    }
    if (immediate != SendSyncRequest) {
        return svc;
    }
    const auto session =
        thread.owner_process.lock()->handle_table.Get<Kernel::ClientSession>(cpu.GetReg(0));
    if (session) {
        svc.service = session->GetName();
    }
    svc.detail = system.Memory().Read32(thread.GetTLSAddress() + CommandBuffer);
    return svc;
}

bool SectionReplay::OnSvc(ARM_Interface& cpu, u32 immediate) {
    if (immediate >= HookSvcBase) {
        const Hook hook = static_cast<Hook>(immediate - HookSvcBase);
        switch (hook) {
        case Hook::Open:
        case Hook::Close:
        case Hook::Observe:
            AtSite(cpu, hook);
            break;
        default:
            UNREACHABLE_MSG("hook svc {:06X}", immediate);
        }
        return true;
    }
    if (replaying) {
        // ArbitrateAddress(handle, address, Signal, count): it wakes threads the replay does not
        // run, so it does nothing.
        constexpr u32 Signal = 0;
        if (immediate == ArbitrateAddress && cpu.GetReg(2) == Signal) {
            cpu.SetReg(0, 0);
            return true;
        }
        // No other thread runs in a replay and its time is held: a sleep only returns.
        if (immediate == SleepThread) {
            return true;
        }
        if (immediate == SendSyncRequest && AnswerFlushDataCache(cpu)) {
            return true;
        }
        Stop(cpu, fmt::format("svc 0x{:02X} at {:08X}", immediate, cpu.GetPC() - 4));
        return true;
    }
    const Kernel::Thread* thread = system.Kernel().GetThreadManager(cpu.GetID()).GetCurrentThread();
    if (window_open && thread != nullptr) {
        Svc svc = DescribeSvc(cpu, *thread, immediate);
        svc.other_thread = !OnWindowThread(cpu);
        census.svcs.push_back(std::move(svc));
    }
    return false;
}

bool SectionReplay::AnswerFlushDataCache(ARM_Interface& cpu) {
    // The GSP answers FlushDataCache and changes nothing; a replay's GPU run reads memory as the
    // replay left it.
    const Kernel::Thread* thread = system.Kernel().GetThreadManager(cpu.GetID()).GetCurrentThread();
    if (thread == nullptr) {
        return false;
    }
    const auto session =
        thread->owner_process.lock()->handle_table.Get<Kernel::ClientSession>(cpu.GetReg(0));
    const VAddr buffer = thread->GetTLSAddress() + CommandBuffer;
    Memory::MemorySystem& memory = system.Memory();
    if (!session || session->GetName() != GspSession ||
        memory.Read32(buffer) != FlushDataCacheRequest) {
        return false;
    }
    memory.Write32(buffer, FlushDataCacheReply);
    memory.Write32(buffer + 4, 0);
    cpu.SetReg(0, 0);
    return true;
}

bool SectionReplay::OnWindowThread(ARM_Interface& cpu) const {
    if (&cpu != window_cpu) {
        return false;
    }
    const Kernel::Thread* thread = system.Kernel().GetThreadManager(cpu.GetID()).GetCurrentThread();
    return thread != nullptr && thread->GetThreadId() == window_thread;
}

void SectionReplay::OnException(ARM_Interface& cpu, VAddr pc) {
    if (replaying) {
        Stop(cpu, fmt::format("exception at {:08X}", pc));
    }
}

void SectionReplay::AtSite(ARM_Interface& cpu, Hook hook) {
    // The hook stands in for the branch; dynarmic has already moved the PC past it.
    const VAddr site = cpu.GetPC() - 4;
    const std::optional<Branch> branch = DecodeBranch(site, system.Memory().Read32(site));
    ASSERT(branch);
    const VAddr target = branch->reg ? cpu.GetReg(*branch->reg) : branch->target;
    ASSERT_MSG((target & 1) == 0, "section site {:08X} branches to Thumb", site);
    if (hook == Hook::Observe && window_open && !replaying && OnWindowThread(cpu)) {
        census.calls.push_back(
            {site, {cpu.GetReg(0), cpu.GetReg(1), cpu.GetReg(2), cpu.GetReg(3)}});
    }
    if (branch->link) {
        cpu.SetReg(14, site + 4);
    }
    cpu.SetPC(target);
    if (hook == Hook::Observe) {
        return;
    }
    if (hook == Hook::Close) {
        if (replaying) {
            Stop(cpu, {});
        } else if (window_open && OnWindowThread(cpu)) {
            CloseWindow(cpu);
        }
    } else if (!window_open && !replaying && OpenWindow(cpu)) {
        cpu.SaveContext(start_context);
    }
}

bool SectionReplay::OpenWindow(ARM_Interface& cpu) {
    if (handler == nullptr) {
        return false;
    }
    const auto process = system.Kernel().GetCurrentProcess();
    const Kernel::Thread* thread = system.Kernel().GetThreadManager(cpu.GetID()).GetCurrentThread();
    if (process == nullptr || thread == nullptr) {
        return false;
    }
    census = {};
    window_thread = thread->GetThreadId();
    window_cpu = &cpu;
    section->Clear();
    section->Arm(*process);
    SetJournal(section.get());
    window_open = true;
    opened = std::chrono::steady_clock::now();
    return true;
}

void SectionReplay::CloseWindow(ARM_Interface& cpu) {
    section->Disarm();
    SetJournal(nullptr);
    census.pages = section->Pages().size();
    census.held_reads = section->HeldReads();
    census.host_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - opened)
                                   .count();
    window_open = false;
    pending = true;
    cpu.PrepareReschedule();
}

void SectionReplay::AfterRun(ARM_Interface& cpu) {
    if (!pending || replaying || &cpu != window_cpu) {
        return;
    }
    pending = false;
    if (handler) {
        handler(*this);
    }
    section->Clear();
}

void SectionReplay::Stop(ARM_Interface& cpu, std::string why) {
    stopped = true;
    refusal = std::move(why);
    cpu.PrepareReschedule();
}

void SectionReplay::CountTicks(u64 ticks) {
    replay_ticks += ticks;
    if (replay_ticks > ReplayTickLimit && !stopped && window_cpu != nullptr) {
        Stop(*window_cpu, fmt::format("tick limit at {:08X} lr {:08X}", window_cpu->GetPC(),
                                      window_cpu->GetReg(14)));
    }
}

void SectionReplay::SetJournal(Memory::StoreJournal* journal) {
    system.Memory().SetStoreJournal(journal);
}

SectionReplay::Result SectionReplay::Replay(std::span<const Input> inputs,
                                            const std::function<void()>& capture) {
    Result result;
    const auto process = system.Kernel().GetCurrentProcess();
    if (window_cpu == nullptr || replaying || window_open || process == nullptr) {
        result.refusal = "no section to replay";
        return result;
    }
    ARM_Interface& cpu = *window_cpu;
    const auto started = std::chrono::steady_clock::now();
    ARM_Interface::ThreadContext end_context{};
    cpu.SaveContext(end_context);

    // The cache follows memory through the exchanges and the rollback; the overlay undoes it all.
    system.GPU().OpenOverlay();
    section->Exchange();
    replay->Clear();
    replay->Arm(*process);
    SetJournal(replay.get());
    for (const Input& input : inputs) {
        system.Memory().WriteBlock(*process, input.address, input.bytes.data(), input.bytes.size());
    }

    cpu.LoadContext(start_context);
    cpu.ClearExclusiveState();
    replaying = true;
    stopped = false;
    refusal.clear();
    replay_ticks = 0;
    while (!stopped) {
        cpu.Run();
    }
    replaying = false;

    result.instructions = replay_ticks;
    result.refusal = refusal;
    result.done = refusal.empty();
    // The capture's own stores are journalled with the replay's.
    if (result.done && capture) {
        capture();
    }
    replay->Disarm();
    SetJournal(nullptr);
    result.pages = replay->Pages().size();
    result.held_reads = replay->HeldReads();
    replay->Rollback();
    section->Exchange();
    system.GPU().CloseOverlay();
    cpu.LoadContext(end_context);
    cpu.ClearExclusiveState();
    result.host_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();
    return result;
}

std::vector<SectionReplay::Difference> SectionReplay::DiffWithSection() const {
    std::vector<Difference> differences;
    const auto compare = [&differences](const Memory::StoreJournal::Page& page, bool replayed) {
        Difference difference{page.vaddr, 0, 0, replayed};
        for (u32 i = 0; i < Memory::CITRA_PAGE_SIZE; ++i) {
            if (page.host[i] != (*page.bytes)[i]) {
                if (difference.bytes == 0) {
                    difference.first = i;
                }
                ++difference.bytes;
            }
        }
        if (difference.bytes != 0) {
            differences.push_back(difference);
        }
    };
    for (const auto& page : section->Pages()) {
        compare(page, replay->SavedBytes(page.host) != nullptr);
    }
    for (const auto& page : replay->Pages()) {
        if (section->SavedBytes(page.host) == nullptr) {
            compare(page, true);
        }
    }
    return differences;
}

std::vector<VAddr> SectionReplay::SectionPages() const {
    std::vector<VAddr> pages;
    for (const auto& page : section->Pages()) {
        pages.push_back(page.vaddr);
    }
    return pages;
}

void SectionReplay::ReadStart(VAddr address, std::span<u8> out) {
    const auto process = system.Kernel().GetCurrentProcess();
    if (process == nullptr) {
        std::fill(out.begin(), out.end(), u8{0});
        return;
    }
    Memory::PageTable& table = *process->vm_manager.page_table;
    std::size_t done = 0;
    while (done < out.size()) {
        const VAddr at = static_cast<VAddr>(address + done);
        const std::size_t offset = at & Memory::CITRA_PAGE_MASK;
        const std::size_t amount = std::min(Memory::CITRA_PAGE_SIZE - offset, out.size() - done);
        const u8* host = table.pointers.Backing(at >> Memory::CITRA_PAGE_BITS);
        const u8* saved = host != nullptr ? section->SavedBytes(host) : nullptr;
        if (saved != nullptr) {
            std::memcpy(out.data() + done, saved + offset, amount);
        } else {
            system.Memory().ReadBlock(*process, at, out.data() + done, amount);
        }
        done += amount;
    }
}

} // namespace Core
