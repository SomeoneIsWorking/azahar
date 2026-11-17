// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>
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

namespace Core {
namespace {

// ARM B or BL, always executed.
struct Branch {
    VAddr target;
    bool link;
};

std::optional<Branch> DecodeBranch(VAddr address, u32 instruction) {
    if ((instruction & 0xFE000000) != 0xEA000000) {
        return std::nullopt;
    }
    const s32 offset = static_cast<s32>(instruction << 8) >> 6;
    return Branch{static_cast<VAddr>(address + 8 + offset), (instruction & 0x01000000) != 0};
}

} // namespace

SectionReplay::SectionReplay(System& system_) : system(system_) {
    hooks[ReturnAddress] = Hook::Return;
}

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

bool SectionReplay::Install(VAddr call_site_, Handler handler_) {
    const std::optional<Branch> branch =
        DecodeBranch(call_site_, system.Memory().Read32(call_site_));
    if (!branch) {
        LOG_ERROR(Core, "section call site {:08X} is not an ARM B or BL", call_site_);
        return false;
    }
    Uninstall();
    call_site = call_site_;
    handler = std::move(handler_);
    hooks[call_site] = Hook::CallSite;
    for (u32 core = 0; core < system.GetNumCores(); ++core) {
        system.GetCore(core).InvalidateCacheRange(call_site, 4);
    }
    return true;
}

void SectionReplay::Uninstall() {
    if (call_site == 0) {
        return;
    }
    hooks.erase(call_site);
    for (u32 core = 0; core < system.GetNumCores(); ++core) {
        system.GetCore(core).InvalidateCacheRange(call_site, 4);
    }
    call_site = 0;
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
    constexpr u32 SendSyncRequest = 0x32;
    constexpr VAddr CommandBuffer = 0x80;
    Svc svc{immediate, cpu.GetPC() - 4, {}, 0};
    if (immediate != SendSyncRequest) {
        return svc;
    }
    const auto session =
        thread.owner_process.lock()->handle_table.Get<Kernel::ClientSession>(cpu.GetReg(0));
    if (session) {
        svc.service = session->GetName();
    }
    svc.header = system.Memory().Read32(thread.GetTLSAddress() + CommandBuffer);
    return svc;
}

bool SectionReplay::OnSvc(ARM_Interface& cpu, u32 immediate) {
    if (immediate >= HookSvcBase) {
        switch (static_cast<Hook>(immediate - HookSvcBase)) {
        case Hook::CallSite:
            OpenWindow(cpu);
            break;
        case Hook::Return:
            if (replaying) {
                Stop(cpu, {});
            } else {
                CloseWindow(cpu);
            }
            break;
        default:
            UNREACHABLE_MSG("hook svc {:06X}", immediate);
        }
        return true;
    }
    if (replaying) {
        Stop(cpu, fmt::format("svc 0x{:02X} at {:08X}", immediate, cpu.GetPC() - 4));
        return true;
    }
    if (window_open && &cpu == window_cpu) {
        const Kernel::Thread* thread =
            system.Kernel().GetThreadManager(cpu.GetID()).GetCurrentThread();
        if (thread != nullptr && thread->GetThreadId() == window_thread) {
            census.svcs.push_back(DescribeSvc(cpu, *thread, immediate));
        }
    }
    return false;
}

void SectionReplay::OnException(ARM_Interface& cpu, VAddr pc) {
    if (replaying) {
        Stop(cpu, fmt::format("exception at {:08X}", pc));
    }
}

void SectionReplay::OpenWindow(ARM_Interface& cpu) {
    // The hook stands in for the branch; dynarmic has already moved the PC past it.
    const VAddr site = cpu.GetPC() - 4;
    const std::optional<Branch> branch = DecodeBranch(site, system.Memory().Read32(site));
    ASSERT(branch);
    if (branch->link) {
        cpu.SetReg(14, site + 4);
    }
    cpu.SetPC(branch->target);
    if (window_open || replaying || handler == nullptr) {
        return;
    }
    const auto process = system.Kernel().GetCurrentProcess();
    const Kernel::Thread* thread = system.Kernel().GetThreadManager(cpu.GetID()).GetCurrentThread();
    if (process == nullptr || thread == nullptr) {
        return;
    }
    cpu.SaveContext(start_context);
    target = branch->target;
    return_address = cpu.GetReg(14);
    census = {};
    window_thread = thread->GetThreadId();
    window_cpu = &cpu;
    section->Clear();
    section->Arm(*process);
    SetJournal(section.get());
    cpu.SetReg(14, ReturnAddress);
    window_open = true;
    opened = std::chrono::steady_clock::now();
}

void SectionReplay::CloseWindow(ARM_Interface& cpu) {
    ASSERT_MSG(window_open, "the section returned without an open window");
    cpu.SetPC(return_address);
    cpu.SetReg(14, return_address);
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
        Stop(*window_cpu, "tick limit");
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

    section->Exchange();
    replay->Clear();
    replay->Arm(*process);
    SetJournal(replay.get());
    for (const Input& input : inputs) {
        system.Memory().WriteBlock(*process, input.address, input.bytes.data(), input.bytes.size());
    }

    cpu.LoadContext(start_context);
    cpu.SetReg(14, ReturnAddress);
    cpu.SetPC(target);
    cpu.ClearExclusiveState();
    replaying = true;
    stopped = false;
    refusal.clear();
    replay_ticks = 0;
    while (!stopped) {
        cpu.Run();
    }
    replaying = false;

    replay->Disarm();
    SetJournal(nullptr);
    result.instructions = replay_ticks;
    result.pages = replay->Pages().size();
    result.held_reads = replay->HeldReads();
    result.refusal = refusal;
    result.done = refusal.empty();
    if (result.done && capture) {
        capture();
    }
    replay->Rollback();
    section->Exchange();
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
