// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "common/common_types.h"
#include "core/arm/arm_interface.h"

namespace Kernel {
class Thread;
}

namespace Memory {
class StoreJournal;
}

namespace Core {

class System;

// Runs a title's draw section again inside a sandbox. A hooked call site (an ARM B or BL to the
// section) opens a window: the section runs for real while a store journal saves every page it
// writes. When it returns, outside the JIT, the handler may replay the section any number of times
// from the state it started in, with inputs written over that state; each replay's stores are
// journalled and rolled back, guest time is held, and any SVC or CPU exception refuses the replay.
class SectionReplay {
public:
    // A hook is the instruction run as `svc` with an immediate this far above the kernel's.
    static constexpr u32 HookSvcBase = 0xFFFF00;

    struct Input {
        VAddr address;
        std::vector<u8> bytes;
    };
    struct Result {
        bool done = false;
        std::string refusal;
        u64 instructions = 0;
        std::size_t pages = 0;
        u64 held_reads = 0;
        s64 host_microseconds = 0;
    };
    // An SVC the real section made; for SendSyncRequest, the session's name and the request's
    // header.
    struct Svc {
        u32 number;
        VAddr pc;
        std::string service;
        u32 header = 0;
    };
    // What the real section reached besides memory: each SVC it made, in order.
    struct Census {
        std::vector<Svc> svcs;
        std::size_t pages = 0;
        u64 held_reads = 0;
        // Host time from the call to the return, other threads' slices included.
        s64 host_microseconds = 0;
    };
    using Handler = std::function<void(SectionReplay&)>;

    explicit SectionReplay(System& system);
    ~SectionReplay();
    SectionReplay(const SectionReplay&) = delete;
    SectionReplay& operator=(const SectionReplay&) = delete;

    // Hooks the call site; the handler runs after each real section returns.
    bool Install(VAddr call_site, Handler handler);
    void Uninstall();

    // Inside the handler. Replays the section from its starting state with `inputs` written, calls
    // `capture` while the replay's memory is in place, and puts everything back.
    Result Replay(std::span<const Input> inputs, const std::function<void()>& capture);
    [[nodiscard]] const Census& SectionCensus() const {
        return census;
    }
    // Reads memory as the section found it.
    void ReadStart(VAddr address, std::span<u8> out);
    // Inside a replay's capture: each page where memory differs from what the real section left.
    struct Difference {
        VAddr page;
        u32 first;
        u32 bytes;
        // The replay stored to the page; otherwise only the real window did (the section or
        // another writer while it ran).
        bool replayed;
    };
    [[nodiscard]] std::vector<Difference> DiffWithSection() const;
    // The pages the real section stored to (and other threads while it ran), by address.
    [[nodiscard]] std::vector<VAddr> SectionPages() const;

    // The CPU's side.
    [[nodiscard]] std::optional<u32> CodeAt(VAddr vaddr) const;
    bool OnSvc(ARM_Interface& cpu, u32 immediate);
    void OnException(ARM_Interface& cpu, VAddr pc);
    void AfterRun(ARM_Interface& cpu);
    [[nodiscard]] bool Replaying() const {
        return replaying;
    }
    // A state saved while the section runs could not return from it: saving waits for the close.
    [[nodiscard]] bool WindowOpen() const {
        return window_open;
    }
    void CountTicks(u64 ticks);

    // A new memory system (boot or load state): no window is open.
    void Reset();

private:
    enum class Hook : u32 { CallSite, Return };
    // The section and its replays return here, so the return address they push is the same.
    static constexpr VAddr ReturnAddress = 0x00000F00;
    static constexpr u64 ReplayTickLimit = 400'000'000;

    void OpenWindow(ARM_Interface& cpu);
    void CloseWindow(ARM_Interface& cpu);
    void Stop(ARM_Interface& cpu, std::string refusal);
    Svc DescribeSvc(ARM_Interface& cpu, const Kernel::Thread& thread, u32 immediate);
    void SetJournal(Memory::StoreJournal* journal);

    System& system;
    std::map<VAddr, Hook> hooks;
    VAddr call_site = 0;
    Handler handler;

    std::unique_ptr<Memory::StoreJournal> section;
    std::unique_ptr<Memory::StoreJournal> replay;
    Census census;
    bool window_open = false;
    bool pending = false;
    u32 window_thread = 0;
    ARM_Interface* window_cpu = nullptr;
    ARM_Interface::ThreadContext start_context{};
    VAddr target = 0;
    VAddr return_address = 0;
    std::chrono::steady_clock::time_point opened;

    bool replaying = false;
    bool stopped = false;
    std::string refusal;
    u64 replay_ticks = 0;
};

} // namespace Core
