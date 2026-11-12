// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <vector>

#include "citra_libretro/azahar_debug.h"
#include "citra_libretro/debug_interface.h"
#include "citra_libretro/environment.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"

namespace LibRetro::Debug {
namespace {

struct Range {
    u32 start;
    u32 size;
};

class Watches {
public:
    bool Add(u32 address, u32 size) {
        auto process = Core::System::GetInstance().Kernel().GetCurrentProcess();
        if (!process || size == 0) {
            return false;
        }
        auto& memory = Core::System::GetInstance().Memory();
        memory.RegisterWatchpoint(*process, address, size);
        ranges.push_back({address, size});
        memory.SetWriteObserver(
            [this](VAddr vaddr, u32 width, u64 value) { OnWrite(vaddr, width, value); });
        return true;
    }

    void Clear() {
        auto process = Core::System::GetInstance().Kernel().GetCurrentProcess();
        auto& memory = Core::System::GetInstance().Memory();
        if (process) {
            for (const Range& range : ranges) {
                memory.UnregisterWatchpoint(*process, range.start, range.size);
            }
        }
        ranges.clear();
        memory.SetWriteObserver({});
    }

    azahar_write_hit_t callback = nullptr;

private:
    void OnWrite(VAddr vaddr, u32 width, u64 value) const {
        if (!callback) {
            return;
        }
        for (const Range& range : ranges) {
            if (vaddr < range.start + range.size && range.start < vaddr + width) {
                auto& cpu = Core::System::GetInstance().GetRunningCore();
                const azahar_write_hit hit{vaddr, width, value, cpu.GetPC(), cpu.GetReg(14)};
                callback(&hit);
                return;
            }
        }
    }

    std::vector<Range> ranges;
};

Watches watches;

bool WatchWrites(uint32_t address, uint32_t size) {
    return watches.Add(address, size);
}

void ClearWatches() {
    watches.Clear();
}

void SetWriteHit(azahar_write_hit_t callback) {
    watches.callback = callback;
}

bool Mapped(const Kernel::Process& process, u32 address, u32 size) {
    auto& memory = Core::System::GetInstance().Memory();
    if (size == 0 || address + size < address) {
        return false;
    }
    for (u64 page = address & ~Memory::CITRA_PAGE_MASK; page < u64{address} + size;
         page += Memory::CITRA_PAGE_SIZE) {
        if (!memory.IsValidVirtualAddress(process, static_cast<VAddr>(page))) {
            return false;
        }
    }
    return true;
}

bool ReadMemory(uint32_t address, void* out, uint32_t size) {
    auto process = Core::System::GetInstance().Kernel().GetCurrentProcess();
    if (!process || !Mapped(*process, address, size)) {
        return false;
    }
    Core::System::GetInstance().Memory().ReadBlock(*process, address, out, size);
    return true;
}

bool WriteMemory(uint32_t address, const void* data, uint32_t size) {
    auto process = Core::System::GetInstance().Kernel().GetCurrentProcess();
    if (!process || !Mapped(*process, address, size)) {
        return false;
    }
    Core::System::GetInstance().Memory().WriteBlock(*process, address, data, size);
    return true;
}

const azahar_debug_interface debug_interface{ReadMemory, WriteMemory, WatchWrites, ClearWatches,
                                             SetWriteHit};

} // namespace

void PublishToFrontend() {
    LibRetro::SetPrivateEnvironment(AZAHAR_ENVIRONMENT_SET_DEBUG_INTERFACE,
                                    const_cast<azahar_debug_interface*>(&debug_interface));
}

void Reset() {
    watches.Clear();
    watches.callback = nullptr;
}

} // namespace LibRetro::Debug
