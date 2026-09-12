// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include <csignal>
#include <cstring>
#include <boost/serialization/array.hpp>
#include <boost/serialization/binary_object.hpp>
#include "audio_core/dsp_interface.h"
#include "common/archives.h"
#include "common/assert.h"
#include "common/atomic_ops.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/optional_helper.h"
#include "common/settings.h"
#include "common/swap.h"
#include "core/arm/arm_interface.h"
#include "core/arm/exception_handler.h"
#include "core/core.h"
#ifdef ENABLE_GDBSTUB
#include "core/gdbstub/gdbstub.h"
#endif
#include "core/global.h"
#include "core/hle/kernel/process.h"
#include "core/hle/service/plgldr/plgldr.h"
#include "core/memory.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

// soh3d_harness write-hook forward decl. Defined in tools/soh3d_harness/
// watchhook.cpp when the harness executable is linked. Weak linkage
// keeps this a no-op in a plain Azahar build.
extern "C" void Soh3d_OnMemoryWrite(u32 vaddr, u32 size, u64 data)
    __attribute__((weak));

// Inline, per-VA write logger for RE. Placed in every case of
// MemorySystem::Write<T> below so we catch writes to specific target
// vaddrs regardless of page type (Memory, MemoryWatchpoint,
// RasterizerCachedMemory, RasterizerCachedMemoryWatchpoint). Bypasses
// the page-granular MemoryWatchpoint mechanism — that mechanism nulls
// the fast-path pointer for the whole page, adding overhead to every
// write in the page, AND misses writes on RasterizerCachedMemory
// pages (the fast path for GPU-backed memory) unless the page has
// been explicitly RegisterWatchpoint'd. The inline logger below is
// always active on the exact byte we care about and prints one line
// per write.
//
// Env: SOH3D_MEMLOG_VAS = comma-separated hex list of vaddrs to log
//                        (e.g. "0x08721a1a" = envCtx.unk_BF).
//      SOH3D_MEMLOG_PATH = output file path (default stderr).
static constexpr int SOH3D_MEMLOG_MAX_VAS = 16;
static constexpr int SOH3D_MEMLOG_MAX_RANGES = 8;
struct Soh3dMemLogRange { u32 start; u32 end; };  // half-open [start, end)
struct Soh3dMemLogCfg {
    u32 vas[SOH3D_MEMLOG_MAX_VAS];
    int n_vas = 0;
    Soh3dMemLogRange ranges[SOH3D_MEMLOG_MAX_RANGES];
    int n_ranges = 0;
    std::FILE* fp = nullptr;
    Soh3dMemLogCfg() {
        const char* path = std::getenv("SOH3D_MEMLOG_PATH");
        const char* s_vas = std::getenv("SOH3D_MEMLOG_VAS");
        const char* s_ranges = std::getenv("SOH3D_MEMLOG_RANGES");
        if ((!s_vas || !s_vas[0]) && (!s_ranges || !s_ranges[0])) return;
        fp = (path && path[0]) ? std::fopen(path, "w") : stderr;
        // Parse discrete VAs.
        for (const char* s = s_vas ? s_vas : ""; *s && n_vas < SOH3D_MEMLOG_MAX_VAS;) {
            while (*s == ',' || *s == ' ') ++s;
            if (!*s) break;
            char* end = nullptr;
            u32 v = (u32)std::strtoul(s, &end, 0);
            if (end == s) break;
            vas[n_vas++] = v;
            s = end;
        }
        // Parse ranges "start1:end1,start2:end2,..." (both hex).
        for (const char* s = s_ranges ? s_ranges : ""; *s && n_ranges < SOH3D_MEMLOG_MAX_RANGES;) {
            while (*s == ',' || *s == ' ') ++s;
            if (!*s) break;
            char* end = nullptr;
            u32 start = (u32)std::strtoul(s, &end, 0);
            if (end == s || *end != ':') break;
            s = end + 1;
            u32 stop = (u32)std::strtoul(s, &end, 0);
            if (end == s) break;
            ranges[n_ranges++] = {start, stop};
            s = end;
        }
        if (fp) {
            std::fprintf(fp, "# soh3d memlog: %d vaddrs, %d ranges\n", n_vas, n_ranges);
            for (int i = 0; i < n_vas; ++i)
                std::fprintf(fp, "#   va 0x%08x\n", vas[i]);
            for (int i = 0; i < n_ranges; ++i)
                std::fprintf(fp, "#   range [0x%08x, 0x%08x)\n",
                             ranges[i].start, ranges[i].end);
            std::fflush(fp);
        }
    }
};
static Soh3dMemLogCfg g_soh3d_memlog;   // constructed at process start
// Kept as separate name for a cheap post-store guard on the fast path.
static int& g_soh3d_memlog_n_vas = g_soh3d_memlog.n_vas;

// Opt-in pointer-acquisition trace for guest memory paths that bypass
// MemorySystem::Write through a direct host pointer. It is deliberately a
// single half-open range so normal emulation pays only one disabled branch.
struct Soh3dPointerLogCfg {
    u32 start = 0;
    u32 end = 0;
    std::FILE* fp = nullptr;
    Soh3dPointerLogCfg() {
        const char* range = std::getenv("SOH3D_PTRLOG_RANGE");
        const char* path = std::getenv("SOH3D_PTRLOG_PATH");
        if (!range || !range[0]) return;
        char* split = nullptr;
        start = (u32)std::strtoul(range, &split, 0);
        if (split == range || *split != ':') return;
        end = (u32)std::strtoul(split + 1, &split, 0);
        if (end <= start || *split != '\0') return;
        fp = (path && path[0]) ? std::fopen(path, "w") : stderr;
        if (fp) {
            std::fprintf(fp, "# soh3d pointer log: [0x%08x, 0x%08x)\n", start, end);
            std::fflush(fp);
        }
    }
};
static Soh3dPointerLogCfg g_soh3d_pointer_log;

static inline void Soh3dPointerLog(VAddr vaddr) {
    if (!g_soh3d_pointer_log.fp || vaddr < g_soh3d_pointer_log.start ||
        vaddr >= g_soh3d_pointer_log.end) {
        return;
    }
    auto& cpu = Core::System::GetInstance().GetRunningCore();
    std::fprintf(g_soh3d_pointer_log.fp,
                 "PTR pc=0x%08x lr=0x%08x va=0x%08x r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x sp=0x%08x\n",
                 cpu.GetPC(), cpu.GetReg(14), vaddr, cpu.GetReg(0), cpu.GetReg(1), cpu.GetReg(2),
                 cpu.GetReg(3), cpu.GetReg(13));
    std::fflush(g_soh3d_pointer_log.fp);
}

static inline void Soh3dMemLogBulk(VAddr vaddr, std::size_t size) {
    if (!g_soh3d_memlog.fp || size == 0) return;
    const u64 write_end = static_cast<u64>(vaddr) + size;
    bool hit = false;
    for (int i = 0; i < g_soh3d_memlog.n_vas && !hit; ++i) {
        const u32 target = g_soh3d_memlog.vas[i];
        hit = target >= vaddr && target < write_end;
    }
    for (int i = 0; i < g_soh3d_memlog.n_ranges && !hit; ++i) {
        const auto& range = g_soh3d_memlog.ranges[i];
        hit = vaddr < range.end && write_end > range.start;
    }
    if (!hit) return;
    auto& cpu = Core::System::GetInstance().GetRunningCore();
    std::fprintf(g_soh3d_memlog.fp,
                 "MB pc=0x%08x lr=0x%08x va=0x%08x sz=%zu r0=0x%08x r1=0x%08x r2=0x%08x "
                 "r3=0x%08x sp=0x%08x\n",
                 cpu.GetPC(), cpu.GetReg(14), vaddr, size, cpu.GetReg(0), cpu.GetReg(1),
                 cpu.GetReg(2), cpu.GetReg(3), cpu.GetReg(13));
    std::fflush(g_soh3d_memlog.fp);
}

template <typename T>
static inline void Soh3dMemLog(u32 vaddr, T data) {
    if (!g_soh3d_memlog.fp) return;
    // Byte-granular match: any target VA in [vaddr, vaddr + sizeof(T))
    const u32 write_end = vaddr + (u32)sizeof(T);
    bool hit = false;
    for (int i = 0; i < g_soh3d_memlog.n_vas && !hit; ++i) {
        const u32 t = g_soh3d_memlog.vas[i];
        if (t >= vaddr && t < write_end) hit = true;
    }
    for (int i = 0; i < g_soh3d_memlog.n_ranges && !hit; ++i) {
        const auto& r = g_soh3d_memlog.ranges[i];
        if (vaddr < r.end && write_end > r.start) hit = true;
    }
    if (!hit) return;
    auto& cpu = Core::System::GetInstance().GetRunningCore();
    const u32 pc = cpu.GetPC();
    const u32 lr = cpu.GetReg(14);
    const u32 r0 = cpu.GetReg(0);
    const u32 r1 = cpu.GetReg(1);
    const u32 r2 = cpu.GetReg(2);
    const u32 r3 = cpu.GetReg(3);
    const u32 r4 = cpu.GetReg(4);
    const u32 r7 = cpu.GetReg(7);
    const u32 r8 = cpu.GetReg(8);
    const u32 r9 = cpu.GetReg(9);
    const u32 r10 = cpu.GetReg(10);
    // FUN_0040cdd8 receives its renderer-state input at r10 - 0x100 at
    // the exact store of its configuration template word. Capture the
    // decomp-grounded fields synchronously with that store; a post-frame
    // read observes a transient object after it has been recycled.
    const u32 r10b = r10 - 0x100;
    const u32 sp = cpu.GetReg(13);
    auto& memory = Core::System::GetInstance().Memory();
    const auto read_word = [&memory](u32 address) {
        return memory.Read32OrNullopt(address).value_or(0);
    };
    const u32 r4p8 = read_word(r4 + 0x08);
    const u32 r4p10 = read_word(r4 + 0x10);
    const u32 r4p14 = read_word(r4 + 0x14);
    const u32 saved_r4 = read_word(sp + 0x04);
    const u32 saved_r4p0 = read_word(saved_r4);
    const u32 saved_r4p4 = read_word(saved_r4 + 0x04);
    const u32 saved_r4p5c = read_word(saved_r4 + 0x5c);
    const u32 saved_r4p6c = read_word(saved_r4 + 0x6c);
    const u32 saved_r4t14 = read_word(saved_r4p0 + 0x14);
    const u32 saved_r4t20 = read_word(saved_r4p0 + 0x20);
    const u32 saved_r4t24 = read_word(saved_r4p0 + 0x24);
    const u32 r1p10 = read_word(r1 + 0x10);
    const u32 r1p14 = read_word(r1 + 0x14);
    const u32 r1p18 = read_word(r1 + 0x18);
    const u32 r1p1c = read_word(r1 + 0x1c);
    const u32 r1p20 = read_word(r1 + 0x20);
    const u32 r1p24 = read_word(r1 + 0x24);
    const u32 r1p28 = read_word(r1 + 0x28);
    const u32 r10bp0 = read_word(r10b);
    const u32 r10bp164 = read_word(r10b + 0x164);
    const u32 r10bp168 = read_word(r10b + 0x168);
    const u32 r10bp16c = read_word(r10b + 0x16c);
    const u32 r10bp170 = read_word(r10b + 0x170);
    const u32 r10bp174 = read_word(r10b + 0x174);
    const u32 r10bp178 = read_word(r10b + 0x178);
    const u32 r10bp17c = read_word(r10b + 0x17c);
    const u32 r10bp180 = read_word(r10b + 0x180);
    const u32 r10bp184 = read_word(r10b + 0x184);
    const u32 r10bp188 = read_word(r10b + 0x188);
    const u32 r10bp18c = read_word(r10b + 0x18c);
    const u32 r10bp190 = read_word(r10b + 0x190);
    u64 wd = 0;
    std::memcpy(&wd, &data, std::min(sizeof(T), sizeof(u64)));
    std::fprintf(g_soh3d_memlog.fp,
                 "MW pc=0x%08x lr=0x%08x va=0x%08x sz=%zu data=0x%016llx "
                 "r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x r4=0x%08x "
                 "r7=0x%08x r8=0x%08x r9=0x%08x r10=0x%08x "
                 "r10b=0x%08x r10bp0=0x%08x r10bp164=0x%08x r10bp168=0x%08x r10bp16c=0x%08x "
                 "r10bp170=0x%08x r10bp174=0x%08x r10bp178=0x%08x r10bp17c=0x%08x "
                 "r10bp180=0x%08x r10bp184=0x%08x r10bp188=0x%08x r10bp18c=0x%08x r10bp190=0x%08x "
                 "r4p8=0x%08x r4p10=0x%08x r4p14=0x%08x sr4=0x%08x sr4p0=0x%08x sr4p4=0x%08x "
                 "sr4p5c=0x%08x sr4p6c=0x%08x sr4t14=0x%08x sr4t20=0x%08x sr4t24=0x%08x "
                 "r1p10=0x%08x r1p14=0x%08x r1p18=0x%08x r1p1c=0x%08x r1p20=0x%08x "
                 "r1p24=0x%08x r1p28=0x%08x sp=0x%08x\n",
                 pc, lr, vaddr, sizeof(T), (unsigned long long)wd, r0, r1, r2, r3, r4, r7, r8, r9,
                 r10, r10b, r10bp0, r10bp164, r10bp168, r10bp16c, r10bp170, r10bp174, r10bp178,
                 r10bp17c, r10bp180, r10bp184, r10bp188, r10bp18c, r10bp190, r4p8, r4p10, r4p14,
                 saved_r4, saved_r4p0, saved_r4p4, saved_r4p5c, saved_r4p6c, saved_r4t14,
                 saved_r4t20, saved_r4t24, r1p10, r1p14, r1p18, r1p1c, r1p20, r1p24, r1p28, sp);
    std::fflush(g_soh3d_memlog.fp);
}

SERIALIZE_EXPORT_IMPL(Memory::MemorySystem::BackingMemImpl<Memory::Region::FCRAM>)
SERIALIZE_EXPORT_IMPL(Memory::MemorySystem::BackingMemImpl<Memory::Region::VRAM>)
SERIALIZE_EXPORT_IMPL(Memory::MemorySystem::BackingMemImpl<Memory::Region::DSP>)
SERIALIZE_EXPORT_IMPL(Memory::MemorySystem::BackingMemImpl<Memory::Region::N3DS>)

#ifndef SIGTRAP
constexpr u32 SIGTRAP = 5;
#endif

#ifndef SIGSEGV
constexpr u32 SIGSEGV = 11;
#endif

namespace Memory {

void PageTable::Clear() {
    pointers.raw.fill(nullptr);
    pointers.refs.fill(MemoryRef());
    attributes.fill(PageType::Unmapped);
}

class RasterizerCacheMarker {
public:
    void Mark(VAddr addr, bool cached) {
        bool* p = At(addr);
        if (p)
            *p = cached;
    }

    bool IsCached(VAddr addr) {
        bool* p = At(addr);
        if (p)
            return *p;
        return false;
    }

private:
    bool* At(VAddr addr) {
        if (addr >= VRAM_VADDR && addr < VRAM_VADDR_END) {
            return &vram[(addr - VRAM_VADDR) / CITRA_PAGE_SIZE];
        }
        if (addr >= LINEAR_HEAP_VADDR && addr < LINEAR_HEAP_VADDR_END) {
            return &linear_heap[(addr - LINEAR_HEAP_VADDR) / CITRA_PAGE_SIZE];
        }
        if (addr >= NEW_LINEAR_HEAP_VADDR && addr < NEW_LINEAR_HEAP_VADDR_END) {
            return &new_linear_heap[(addr - NEW_LINEAR_HEAP_VADDR) / CITRA_PAGE_SIZE];
        }
        if (addr >= PLUGIN_3GX_FB_VADDR && addr < PLUGIN_3GX_FB_VADDR_END) {
            return &plugin_fb[(addr - PLUGIN_3GX_FB_VADDR) / CITRA_PAGE_SIZE];
        }
        return nullptr;
    }

    std::array<bool, VRAM_SIZE / CITRA_PAGE_SIZE> vram{};
    std::array<bool, LINEAR_HEAP_SIZE / CITRA_PAGE_SIZE> linear_heap{};
    std::array<bool, NEW_LINEAR_HEAP_SIZE / CITRA_PAGE_SIZE> new_linear_heap{};
    std::array<bool, PLUGIN_3GX_FB_SIZE / CITRA_PAGE_SIZE> plugin_fb{};

    static_assert(sizeof(bool) == 1);
    friend class boost::serialization::access;
    template <typename Archive>
    void serialize(Archive& ar, const unsigned int file_version) {
        ar & vram;
        ar & linear_heap;
        ar & new_linear_heap;
        ar & plugin_fb;
    }
};

class MemorySystem::Impl {
public:
    // Visual Studio would try to allocate these on compile time
    // if they are std::array which would exceed the memory limit.
    std::unique_ptr<u8[]> fcram = std::make_unique<u8[]>(Memory::FCRAM_N3DS_SIZE);
    std::unique_ptr<u8[]> vram = std::make_unique<u8[]>(Memory::VRAM_SIZE);
    std::unique_ptr<u8[]> n3ds_extra_ram = std::make_unique<u8[]>(Memory::N3DS_EXTRA_RAM_SIZE);
    std::unique_ptr<u8[]> dsp_ram = std::make_unique<u8[]>(Memory::DSP_RAM_SIZE);

    Core::System& system;
    std::shared_ptr<PageTable> current_page_table = nullptr;
    RasterizerCacheMarker cache_marker;
    std::vector<std::shared_ptr<PageTable>> page_table_list;

    std::shared_ptr<BackingMem> fcram_mem;
    std::shared_ptr<BackingMem> vram_mem;
    std::shared_ptr<BackingMem> n3ds_extra_ram_mem;
    std::shared_ptr<BackingMem> dsp_mem;

    PAddr plugin_fb_address{};

    Impl(Core::System& system_);

    const u8* GetPtr(Region r) const {
        switch (r) {
        case Region::VRAM:
            return vram.get();
        case Region::DSP:
            return dsp_ram.get();
        case Region::FCRAM:
            return fcram.get();
        case Region::N3DS:
            return n3ds_extra_ram.get();
        default:
            UNREACHABLE();
        }
    }

    u8* GetPtr(Region r) {
        switch (r) {
        case Region::VRAM:
            return vram.get();
        case Region::DSP:
            return dsp_ram.get();
        case Region::FCRAM:
            return fcram.get();
        case Region::N3DS:
            return n3ds_extra_ram.get();
        default:
            UNREACHABLE();
        }
    }

    u32 GetSize(Region r) const {
        switch (r) {
        case Region::VRAM:
            return VRAM_SIZE;
        case Region::DSP:
            return DSP_RAM_SIZE;
        case Region::FCRAM:
            return FCRAM_N3DS_SIZE;
        case Region::N3DS:
            return N3DS_EXTRA_RAM_SIZE;
        default:
            UNREACHABLE();
        }
    }

    u32 GetPC() const noexcept {
        return system.GetRunningCore().GetPC();
    }

    template <bool UNSAFE>
    void ReadBlockImpl(const Kernel::Process& process, const VAddr src_addr, void* dest_buffer,
                       const std::size_t size) {
        auto& page_table = *process.vm_manager.page_table;

        std::size_t remaining_size = size;
        std::size_t page_index = src_addr >> CITRA_PAGE_BITS;
        std::size_t page_offset = src_addr & CITRA_PAGE_MASK;

        while (remaining_size > 0) {
            const std::size_t copy_amount = std::min(CITRA_PAGE_SIZE - page_offset, remaining_size);
            const VAddr current_vaddr =
                static_cast<VAddr>((page_index << CITRA_PAGE_BITS) + page_offset);
            switch (page_table.attributes[page_index]) {
            case PageType::Unmapped: {
                LOG_ERROR(
                    HW_Memory,
                    "unmapped ReadBlock @ 0x{:08X} (start address = 0x{:08X}, size = {}) at PC "
                    "0x{:08X}",
                    current_vaddr, src_addr, size, GetPC());
                std::memset(dest_buffer, 0, copy_amount);
                break;
            }
            case PageType::Memory: {
                DEBUG_ASSERT(page_table.pointers[page_index]);

                const u8* src_ptr = page_table.pointers[page_index] + page_offset;
                std::memcpy(dest_buffer, src_ptr, copy_amount);
                break;
            }
            case PageType::MemoryWatchpoint: {
                auto it = page_table.watchpoint_pages_map.find(page_index);
                ASSERT_MSG(it != page_table.watchpoint_pages_map.end(),
                           "Missing memory for watchpoint page");

                const u8* src_ptr = it->second.memory.GetPtr() + page_offset;
                std::memcpy(dest_buffer, src_ptr, copy_amount);
                break;
            }
            case PageType::RasterizerCachedMemory:
            case PageType::RasterizerCachedMemoryWatchpoint: {
                if constexpr (!UNSAFE) {
                    RasterizerFlushVirtualRegion(current_vaddr, static_cast<u32>(copy_amount),
                                                 FlushMode::Flush);
                }
                std::memcpy(dest_buffer, GetPointerForRasterizerCache(current_vaddr), copy_amount);
                break;
            }
            default:
                UNREACHABLE();
            }

            page_index++;
            page_offset = 0;
            dest_buffer = static_cast<u8*>(dest_buffer) + copy_amount;
            remaining_size -= copy_amount;
        }
    }

    template <bool UNSAFE>
    void WriteBlockImpl(const Kernel::Process& process, const VAddr dest_addr,
                        const void* src_buffer, const std::size_t size) {
        auto& page_table = *process.vm_manager.page_table;
        std::size_t remaining_size = size;
        std::size_t page_index = dest_addr >> CITRA_PAGE_BITS;
        std::size_t page_offset = dest_addr & CITRA_PAGE_MASK;

        while (remaining_size > 0) {
            const std::size_t copy_amount = std::min(CITRA_PAGE_SIZE - page_offset, remaining_size);
            const VAddr current_vaddr =
                static_cast<VAddr>((page_index << CITRA_PAGE_BITS) + page_offset);
            Soh3dMemLogBulk(current_vaddr, copy_amount);

            switch (page_table.attributes[page_index]) {
            case PageType::Unmapped: {
                LOG_ERROR(
                    HW_Memory,
                    "unmapped WriteBlock @ 0x{:08X} (start address = 0x{:08X}, size = {}) at PC "
                    "0x{:08X}",
                    current_vaddr, dest_addr, size, GetPC());
                break;
            }
            case PageType::Memory: {
                DEBUG_ASSERT(page_table.pointers[page_index]);

                u8* dest_ptr = page_table.pointers[page_index] + page_offset;
                std::memcpy(dest_ptr, src_buffer, copy_amount);
                break;
            }
            case PageType::MemoryWatchpoint: {
                auto it = page_table.watchpoint_pages_map.find(page_index);
                ASSERT_MSG(it != page_table.watchpoint_pages_map.end(),
                           "Missing memory for watchpoint page");

                u8* dest_ptr = it->second.memory.GetPtr() + page_offset;
                std::memcpy(dest_ptr, src_buffer, copy_amount);
                break;
            }
            case PageType::RasterizerCachedMemory:
            case PageType::RasterizerCachedMemoryWatchpoint: {
                if constexpr (!UNSAFE) {
                    RasterizerFlushVirtualRegion(current_vaddr, static_cast<u32>(copy_amount),
                                                 FlushMode::Invalidate);
                }
                std::memcpy(GetPointerForRasterizerCache(current_vaddr), src_buffer, copy_amount);
                break;
            }
            default:
                UNREACHABLE();
            }

            page_index++;
            page_offset = 0;
            src_buffer = static_cast<const u8*>(src_buffer) + copy_amount;
            remaining_size -= copy_amount;
        }
    }

    MemoryRef GetPointerForRasterizerCache(VAddr addr) const {
        if (addr >= LINEAR_HEAP_VADDR && addr < LINEAR_HEAP_VADDR_END) {
            return {fcram_mem, addr - LINEAR_HEAP_VADDR};
        }
        if (addr >= NEW_LINEAR_HEAP_VADDR && addr < NEW_LINEAR_HEAP_VADDR_END) {
            return {fcram_mem, addr - NEW_LINEAR_HEAP_VADDR};
        }
        if (addr >= VRAM_VADDR && addr < VRAM_VADDR_END) {
            return {vram_mem, addr - VRAM_VADDR};
        }
        if (addr >= PLUGIN_3GX_FB_VADDR && addr < PLUGIN_3GX_FB_VADDR_END && plugin_fb_address) {
            return {fcram_mem, addr - PLUGIN_3GX_FB_VADDR + plugin_fb_address - FCRAM_PADDR};
        }

        UNREACHABLE();
        return MemoryRef{};
    }

    void RasterizerFlushVirtualRegion(VAddr start, u32 size, FlushMode mode) {
        const VAddr end = start + size;

        auto CheckRegion = [&](VAddr region_start, VAddr region_end, PAddr paddr_region_start) {
            if (start >= region_end || end <= region_start) {
                // No overlap with region
                return;
            }

            auto& renderer = system.GPU().Renderer();
            VAddr overlap_start = std::max(start, region_start);
            VAddr overlap_end = std::min(end, region_end);
            PAddr physical_start = paddr_region_start + (overlap_start - region_start);
            u32 overlap_size = overlap_end - overlap_start;

            auto* rasterizer = renderer.Rasterizer();
            switch (mode) {
            case FlushMode::Flush:
                rasterizer->FlushRegion(physical_start, overlap_size);
                break;
            case FlushMode::Invalidate:
                rasterizer->InvalidateRegion(physical_start, overlap_size);
                break;
            case FlushMode::FlushAndInvalidate:
                rasterizer->FlushAndInvalidateRegion(physical_start, overlap_size);
                break;
            }
        };

        CheckRegion(LINEAR_HEAP_VADDR, LINEAR_HEAP_VADDR_END, FCRAM_PADDR);
        CheckRegion(NEW_LINEAR_HEAP_VADDR, NEW_LINEAR_HEAP_VADDR_END, FCRAM_PADDR);
        CheckRegion(VRAM_VADDR, VRAM_VADDR_END, VRAM_PADDR);
        if (plugin_fb_address) {
            CheckRegion(PLUGIN_3GX_FB_VADDR, PLUGIN_3GX_FB_VADDR_END, plugin_fb_address);
        }
    }

private:
    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const unsigned int file_version) {
        bool save_n3ds_ram = Settings::values.is_new_3ds.GetValue();
        ar & save_n3ds_ram;
        ar& boost::serialization::make_binary_object(vram.get(), Memory::VRAM_SIZE);
        ar& boost::serialization::make_binary_object(
            fcram.get(), save_n3ds_ram ? Memory::FCRAM_N3DS_SIZE : Memory::FCRAM_SIZE);
        ar& boost::serialization::make_binary_object(
            n3ds_extra_ram.get(), save_n3ds_ram ? Memory::N3DS_EXTRA_RAM_SIZE : 0);
        ar& boost::serialization::make_binary_object(dsp_ram.get(), Memory::DSP_RAM_SIZE);
        ar & cache_marker;
        ar & page_table_list;
        // dsp is set from Core::System at startup
        ar & current_page_table;
        ar & fcram_mem;
        ar & vram_mem;
        ar & n3ds_extra_ram_mem;
        ar & dsp_mem;
        ar & plugin_fb_address;
    }
};

// We use this rather than BufferMem because we don't want new objects to be allocated when
// deserializing. This avoids unnecessary memory thrashing.
template <Region R>
class MemorySystem::BackingMemImpl : public BackingMem {
public:
    BackingMemImpl() : impl(*Core::Global<Core::System>().Memory().impl) {}
    explicit BackingMemImpl(MemorySystem::Impl& impl_) : impl(impl_) {}
    u8* GetPtr() override {
        return impl.GetPtr(R);
    }
    const u8* GetPtr() const override {
        return impl.GetPtr(R);
    }
    std::size_t GetSize() const override {
        return impl.GetSize(R);
    }

private:
    MemorySystem::Impl& impl;

    template <class Archive>
    void serialize(Archive& ar, const unsigned int) {
        ar& boost::serialization::base_object<BackingMem>(*this);
    }
    friend class boost::serialization::access;
};

MemorySystem::Impl::Impl(Core::System& system_)
    : system{system_}, fcram_mem(std::make_shared<BackingMemImpl<Region::FCRAM>>(*this)),
      vram_mem(std::make_shared<BackingMemImpl<Region::VRAM>>(*this)),
      n3ds_extra_ram_mem(std::make_shared<BackingMemImpl<Region::N3DS>>(*this)),
      dsp_mem(std::make_shared<BackingMemImpl<Region::DSP>>(*this)) {}

MemorySystem::MemorySystem(Core::System& system) : impl(std::make_unique<Impl>(system)) {}
MemorySystem::~MemorySystem() = default;

template <class Archive>
void MemorySystem::serialize(Archive& ar, const unsigned int file_version) {
    ar&* impl.get();
}

SERIALIZE_IMPL(MemorySystem)

void MemorySystem::SetCurrentPageTable(std::shared_ptr<PageTable> page_table) {
    impl->current_page_table = page_table;
}

std::shared_ptr<PageTable> MemorySystem::GetCurrentPageTable() const {
    return impl->current_page_table;
}

void MemorySystem::RasterizerFlushVirtualRegion(VAddr start, u32 size, FlushMode mode) {
    impl->RasterizerFlushVirtualRegion(start, size, mode);
}

PAddr& Memory::MemorySystem::Plugin3GXFramebufferAddress() {
    return impl->plugin_fb_address;
}

void MemorySystem::RegisterWatchpoint(const Kernel::Process& process, VAddr addr, u32 size) {
    auto& page_table = *process.vm_manager.page_table;

    VAddr current = addr;
    VAddr end = addr + size;

    while (current < end) {
        const VAddr page_base = (current & ~CITRA_PAGE_MASK);
        const VAddr page_index = page_base >> CITRA_PAGE_BITS;

        auto it = page_table.watchpoint_pages_map.find(page_index);
        if (it != page_table.watchpoint_pages_map.end()) {
            // Nothing to do, only increment count.
            it->second.watchpoint_count++;
        } else {
            MemoryRef mem;
            PageType& type = page_table.attributes[page_index];

            switch (type) {
            case PageType::Memory:
                mem = page_table.pointers.Ref(page_index);
                type = PageType::MemoryWatchpoint;
                page_table.pointers[page_index] = nullptr;
                break;
            case PageType::RasterizerCachedMemory:
                mem = GetPointerForRasterizerCache(page_base);
                type = PageType::RasterizerCachedMemoryWatchpoint;
                break;
            default:
                LOG_ERROR(HW_Memory, "Cannot get pointer to register watchpoint for page 0x{:08X}",
                          page_base);
                continue;
            }

            page_table.watchpoint_pages_map.insert(
                {page_index,
                 PageTable::WatchpointPageInfo{.watchpoint_count = 1, .memory = std::move(mem)}});
        }

        current = page_base + CITRA_PAGE_SIZE;
    }
}

void MemorySystem::UnregisterWatchpoint(const Kernel::Process& process, VAddr addr, u32 size) {
    auto& page_table = *process.vm_manager.page_table;

    VAddr current = addr;
    VAddr end = addr + size;

    while (current < end) {
        const VAddr page_base = (current & ~CITRA_PAGE_MASK);
        const VAddr page_index = page_base >> CITRA_PAGE_BITS;

        auto it = page_table.watchpoint_pages_map.find(page_index);
        if (it != page_table.watchpoint_pages_map.end()) {
            if (--it->second.watchpoint_count == 0) {

                PageType& type = page_table.attributes[page_index];

                switch (type) {
                case PageType::MemoryWatchpoint:
                    type = PageType::Memory;
                    page_table.pointers[page_index] = it->second.memory;
                    break;
                case PageType::RasterizerCachedMemoryWatchpoint:
                    type = PageType::RasterizerCachedMemory;
                    break;
                default:
                    LOG_ERROR(HW_Memory, "Invalid watchpoint page type for page 0x{:08X}: {}",
                              page_base, static_cast<u8>(type));
                }

                page_table.watchpoint_pages_map.erase(page_index);
            }
        } else {
            LOG_ERROR(HW_Memory, "No watchpoint found on page 0x{:08X}", page_base);
        }

        current = page_base + CITRA_PAGE_SIZE;
    }
}

void MemorySystem::MapPages(PageTable& page_table, u32 base, u32 size, MemoryRef memory,
                            PageType type) {
    LOG_DEBUG(HW_Memory, "Mapping {} onto {:08X}-{:08X}", (void*)memory.GetPtr(),
              base * CITRA_PAGE_SIZE, (base + size) * CITRA_PAGE_SIZE);

    if (impl->system.IsPoweredOn()) {
        RasterizerFlushVirtualRegion(base << CITRA_PAGE_BITS, size * CITRA_PAGE_SIZE,
                                     FlushMode::FlushAndInvalidate);
    }

    u32 end = base + size;
    while (base != end) {
        ASSERT_MSG(base < PAGE_TABLE_NUM_ENTRIES, "out of range mapping at {:08X}", base);

        page_table.attributes[base] = type;
        page_table.pointers[base] = memory;

        // If the memory to map is already rasterizer-cached, mark the page
        if (type == PageType::Memory && impl->cache_marker.IsCached(base * CITRA_PAGE_SIZE)) {
            page_table.attributes[base] = PageType::RasterizerCachedMemory;
            page_table.pointers[base] = nullptr;
        }

        base += 1;
        if (memory != nullptr && memory.GetSize() > CITRA_PAGE_SIZE)
            memory += CITRA_PAGE_SIZE;
    }
}

void MemorySystem::MapMemoryRegion(PageTable& page_table, VAddr base, u32 size, MemoryRef target) {
    ASSERT_MSG((size & CITRA_PAGE_MASK) == 0, "non-page aligned size: {:08X}", size);
    ASSERT_MSG((base & CITRA_PAGE_MASK) == 0, "non-page aligned base: {:08X}", base);
    MapPages(page_table, base / CITRA_PAGE_SIZE, size / CITRA_PAGE_SIZE, target, PageType::Memory);
}

void MemorySystem::UnmapRegion(PageTable& page_table, VAddr base, u32 size) {
    ASSERT_MSG((size & CITRA_PAGE_MASK) == 0, "non-page aligned size: {:08X}", size);
    ASSERT_MSG((base & CITRA_PAGE_MASK) == 0, "non-page aligned base: {:08X}", base);
    MapPages(page_table, base / CITRA_PAGE_SIZE, size / CITRA_PAGE_SIZE, nullptr,
             PageType::Unmapped);
}

MemoryRef MemorySystem::GetPointerForRasterizerCache(VAddr addr) const {
    return impl->GetPointerForRasterizerCache(addr);
}

void MemorySystem::RegisterPageTable(std::shared_ptr<PageTable> page_table) {
    impl->page_table_list.push_back(page_table);
}

void MemorySystem::UnregisterPageTable(std::shared_ptr<PageTable> page_table) {
    auto it = std::find(impl->page_table_list.begin(), impl->page_table_list.end(), page_table);
    if (it != impl->page_table_list.end()) {
        impl->page_table_list.erase(it);
    }
}

template <typename T>
void MemorySystem::UnmappedAccess(const VAddr vaddr, const T value, bool read) {
    const std::string mode = (read ? "Read" : "Write");
    const std::string value_str = read ? std::string("") : fmt::format(" 0x{:08X}", value);
    const std::string message = fmt::format("unmapped {}{}{} @ 0x{:08X} at PC 0x{:08X}", mode,
                                            sizeof(T) * 8, value_str, vaddr, impl->GetPC());
#ifdef ENABLE_GDBSTUB
    if (GDBStub::IsConnected()) {
        GDBStub::Break(SIGSEGV);
    }
#endif
    LOG_ERROR(HW_Memory, "{}", message);
    if (Settings::values.enable_exception_handler) {
        Core::LogException(impl->system, read ? Core::ExceptionType::UnmappedRead
                                              : Core::ExceptionType::UnmappedWrite);
    }
}

template <typename T>
T MemorySystem::Read(const std::shared_ptr<PageTable>& page_table, const VAddr vaddr) {
    constexpr bool is_optional = is_optional_type<T>;
    using ReadType = optional_inner_or_type<T>;

    constexpr size_t read_size = sizeof(ReadType);

    const u8* page_pointer = page_table->pointers[vaddr >> CITRA_PAGE_BITS];
    if (page_pointer) {
        // NOTE: Avoid adding any extra logic to this fast-path block
        ReadType value;
        std::memcpy(&value, &page_pointer[vaddr & CITRA_PAGE_MASK], read_size);
        return value;
    }

    // Custom Luma3ds mapping (bit 31 set) Bypasses page tables for FCRAM/MMIO
    constexpr VAddr LUMA_ALIAS_BIT = 0x80000000u;

    if (vaddr & LUMA_ALIAS_BIT) [[unlikely]] {
        const PAddr paddr = vaddr & ~LUMA_ALIAS_BIT;

        // FCRAM (0x2xxxxxxx)
        if ((paddr & 0xF0000000) == Memory::FCRAM_PADDR) {
            ReadType value;
            std::memcpy(&value, GetFCRAMPointer(paddr - Memory::FCRAM_PADDR), read_size);
            return value;
        }

        // MMIO (0x1xxxxxxx, >= IO_AREA_PADDR) - Strictly 32-bit
        if ((paddr & 0xF0000000) == 0x10000000 && paddr >= Memory::IO_AREA_PADDR) [[unlikely]] {
            return static_cast<ReadType>(impl->system.GPU().ReadReg(
                static_cast<VAddr>(paddr) - Memory::IO_AREA_PADDR + 0x1EC00000));
        }
        // Fallthrough: Standard page table lookup
    }

    PageType type = page_table->attributes[vaddr >> CITRA_PAGE_BITS];
    switch (type) {
    case PageType::Unmapped: {

        UnmappedAccess<ReadType>(vaddr, 0, true);

        if constexpr (is_optional) {
            return std::nullopt;
        } else {
            return T{};
        }
    }
    case PageType::Memory:
        ASSERT_MSG(false, "Mapped memory page without a pointer @ {:08X}", vaddr);
        break;
    case PageType::MemoryWatchpoint: {
        auto it = page_table->watchpoint_pages_map.find(vaddr >> CITRA_PAGE_BITS);
        ASSERT_MSG(it != page_table->watchpoint_pages_map.end(),
                   "Missing memory for watchpoint page");

        ReadType value;
        std::memcpy(&value, it->second.memory.GetPtr() + (vaddr & CITRA_PAGE_MASK), read_size);

#ifdef ENABLE_GDBSTUB
        if (GDBStub::CheckBreakpoint(vaddr, read_size, GDBStub::BreakpointType::Read)) {
            GDBStub::Break(SIGTRAP);
        }
#endif

        return value;
    }
    [[likely]] case PageType::RasterizerCachedMemory: {
        RasterizerFlushVirtualRegion(vaddr, read_size, FlushMode::Flush);

        ReadType value;
        std::memcpy(&value, GetPointerForRasterizerCache(vaddr), read_size);
        return value;
    }
    case PageType::RasterizerCachedMemoryWatchpoint: {
        RasterizerFlushVirtualRegion(vaddr, read_size, FlushMode::Flush);

        ReadType value;
        std::memcpy(&value, GetPointerForRasterizerCache(vaddr), read_size);

#ifdef ENABLE_GDBSTUB
        if (GDBStub::CheckBreakpoint(vaddr, read_size, GDBStub::BreakpointType::Read)) {
            GDBStub::Break(SIGTRAP);
        }
#endif

        return value;
    }
    default:
        UNREACHABLE();
    }

    if constexpr (is_optional) {
        return std::nullopt;
    } else {
        return T{};
    }
}

template <typename T>
void MemorySystem::Write(const std::shared_ptr<PageTable>& page_table, const VAddr vaddr,
                         const T data) {
    u8* page_pointer = page_table->pointers[vaddr >> CITRA_PAGE_BITS];
    if (page_pointer) {
        // NOTE: Avoid adding any extra logic to this fast-path block
        std::memcpy(&page_pointer[vaddr & CITRA_PAGE_MASK], &data, sizeof(T));
        // Soh3d RE hook: cheap post-store log for narrow target VA set.
        // Init() is a no-op after first call. When SOH3D_MEMLOG_VAS is
        // unset g_soh3d_memlog_n_vas is 0 and Soh3dMemLog returns
        // immediately. Kept AFTER the memcpy so the store itself is
        // unaffected by any harness overhead.
        if (g_soh3d_memlog.fp) Soh3dMemLog(vaddr, data);
        return;
    }

    // Custom Luma3ds mapping (bit 31 set) Bypasses page tables for FCRAM/MMIO
    constexpr VAddr LUMA_ALIAS_BIT = 0x80000000u;

    if (vaddr & LUMA_ALIAS_BIT) [[unlikely]] {
        const PAddr paddr = vaddr & ~LUMA_ALIAS_BIT;

        // FCRAM (0x2xxxxxxx)
        if ((paddr & 0xF0000000) == Memory::FCRAM_PADDR) {
            std::memcpy(GetFCRAMPointer(paddr - Memory::FCRAM_PADDR), &data, sizeof(T));
            return;
        }

        // MMIO (0x1xxxxxxx, >= IO_AREA_PADDR) - Strictly 32-bit
        if ((paddr & 0xF0000000) == 0x10000000 && paddr >= Memory::IO_AREA_PADDR) [[unlikely]] {
            ASSERT(sizeof(data) == sizeof(u32));
            impl->system.GPU().WriteReg(static_cast<VAddr>(paddr) - Memory::IO_AREA_PADDR +
                                            0x1EC00000,
                                        static_cast<u32>(data));
            return;
        }
        // Fallthrough: Standard page table lookup
    }

    PageType type = page_table->attributes[vaddr >> CITRA_PAGE_BITS];
    switch (type) {
    case PageType::Unmapped:
        (void)UnmappedAccess<T>(vaddr, data, false);
        return;
    case PageType::Memory:
        ASSERT_MSG(false, "Mapped memory page without a pointer @ {:08X}", vaddr);
        break;
    case PageType::MemoryWatchpoint: {
        auto it = page_table->watchpoint_pages_map.find(vaddr >> CITRA_PAGE_BITS);
        ASSERT_MSG(it != page_table->watchpoint_pages_map.end(),
                   "Missing memory for watchpoint page");

        std::memcpy(it->second.memory.GetPtr() + (vaddr & CITRA_PAGE_MASK), &data, sizeof(T));
        if (g_soh3d_memlog.fp) Soh3dMemLog(vaddr, data);

#ifdef ENABLE_GDBSTUB
        if (GDBStub::CheckBreakpoint(vaddr, sizeof(T), GDBStub::BreakpointType::Write)) {
            GDBStub::Break(SIGTRAP);
        }
#endif

        // soh3d_harness write-hook: notify external hook on every write
        // that lands in a MemoryWatchpoint page. See tools/soh3d_harness/
        // watchhook.{h,cpp} for the harness-side receiver. Declared at
        // top-of-file with weak linkage so a build without the harness
        // stays a no-op.
        if (&::Soh3d_OnMemoryWrite) {
            u64 wd = 0;
            std::memcpy(&wd, &data, std::min(sizeof(T), sizeof(u64)));
            ::Soh3d_OnMemoryWrite(vaddr, sizeof(T), wd);
        }

        break;
    }
    [[likely]] case PageType::RasterizerCachedMemory: {
        RasterizerFlushVirtualRegion(vaddr, sizeof(T), FlushMode::Invalidate);
        std::memcpy(GetPointerForRasterizerCache(vaddr), &data, sizeof(T));
        if (g_soh3d_memlog.fp) Soh3dMemLog(vaddr, data);
        break;
    }
    case PageType::RasterizerCachedMemoryWatchpoint: {
        RasterizerFlushVirtualRegion(vaddr, sizeof(T), FlushMode::Invalidate);
        std::memcpy(GetPointerForRasterizerCache(vaddr), &data, sizeof(T));
        if (g_soh3d_memlog.fp) Soh3dMemLog(vaddr, data);

#ifdef ENABLE_GDBSTUB
        if (GDBStub::CheckBreakpoint(vaddr, sizeof(T), GDBStub::BreakpointType::Write)) {
            GDBStub::Break(SIGTRAP);
        }
#endif

        break;
    }
    default:
        UNREACHABLE();
    }
}

template <typename T>
bool MemorySystem::WriteExclusive(const VAddr vaddr, const T data, const T expected) {
    u8* page_pointer = impl->current_page_table->pointers[vaddr >> CITRA_PAGE_BITS];

    if (page_pointer) {
        const auto volatile_pointer =
            reinterpret_cast<volatile T*>(&page_pointer[vaddr & CITRA_PAGE_MASK]);
        return Common::AtomicCompareAndSwap(volatile_pointer, data, expected);
    }

    PageType type = impl->current_page_table->attributes[vaddr >> CITRA_PAGE_BITS];
    switch (type) {
    case PageType::Unmapped:
        (void)UnmappedAccess<T>(vaddr, data, false);
        return true;
    case PageType::Memory:
        ASSERT_MSG(false, "Mapped memory page without a pointer @ {:08X}", vaddr);
        return true;
    case PageType::MemoryWatchpoint: {
        auto it = impl->current_page_table->watchpoint_pages_map.find(vaddr >> CITRA_PAGE_BITS);
        ASSERT_MSG(it != impl->current_page_table->watchpoint_pages_map.end(),
                   "Missing memory for watchpoint page");

        const auto volatile_pointer =
            reinterpret_cast<volatile T*>(it->second.memory.GetPtr() + (vaddr & CITRA_PAGE_MASK));

        bool ret = Common::AtomicCompareAndSwap(volatile_pointer, data, expected);

#ifdef ENABLE_GDBSTUB
        if (GDBStub::CheckBreakpoint(vaddr, sizeof(T), GDBStub::BreakpointType::Write)) {
            GDBStub::Break(SIGTRAP);
        }
#endif

        return ret;
    }
    [[likely]] case PageType::RasterizerCachedMemory: {
        RasterizerFlushVirtualRegion(vaddr, sizeof(T), FlushMode::Invalidate);
        const auto volatile_pointer =
            reinterpret_cast<volatile T*>(GetPointerForRasterizerCache(vaddr).GetPtr());
        return Common::AtomicCompareAndSwap(volatile_pointer, data, expected);
    }
    case PageType::RasterizerCachedMemoryWatchpoint: {
        RasterizerFlushVirtualRegion(vaddr, sizeof(T), FlushMode::Invalidate);
        const auto volatile_pointer =
            reinterpret_cast<volatile T*>(GetPointerForRasterizerCache(vaddr).GetPtr());

#ifdef ENABLE_GDBSTUB
        if (GDBStub::CheckBreakpoint(vaddr, sizeof(T), GDBStub::BreakpointType::Write)) {
            GDBStub::Break(SIGTRAP);
        }
#endif

        return Common::AtomicCompareAndSwap(volatile_pointer, data, expected);
    }
    default:
        UNREACHABLE();
    }
    return true;
}

bool MemorySystem::IsValidVirtualAddress(const Kernel::Process& process, const VAddr vaddr) {
    auto& page_table = *process.vm_manager.page_table;

    auto page_pointer = page_table.pointers[vaddr >> CITRA_PAGE_BITS];
    if (page_pointer) {
        return true;
    }

    if (page_table.attributes[vaddr >> CITRA_PAGE_BITS] != PageType::Unmapped) {
        return true;
    }

    return false;
}

bool MemorySystem::IsValidPhysicalAddress(const PAddr paddr) {
    return GetPhysicalRef(paddr);
}

u8* MemorySystem::GetPointer(const VAddr vaddr) {
    Soh3dPointerLog(vaddr);
    u8* page_pointer = impl->current_page_table->pointers[vaddr >> CITRA_PAGE_BITS];
    if (page_pointer) {
        return page_pointer + (vaddr & CITRA_PAGE_MASK);
    }

    if (impl->current_page_table->attributes[vaddr >> CITRA_PAGE_BITS] ==
            PageType::RasterizerCachedMemory ||
        impl->current_page_table->attributes[vaddr >> CITRA_PAGE_BITS] ==
            PageType::RasterizerCachedMemoryWatchpoint) {
        return GetPointerForRasterizerCache(vaddr);
    }

    LOG_ERROR(HW_Memory, "unknown GetPointer @ 0x{:08x} at PC 0x{:08X}", vaddr, impl->GetPC());
    return nullptr;
}

const u8* MemorySystem::GetPointer(const VAddr vaddr) const {
    Soh3dPointerLog(vaddr);
    const u8* page_pointer = impl->current_page_table->pointers[vaddr >> CITRA_PAGE_BITS];
    if (page_pointer) {
        return page_pointer + (vaddr & CITRA_PAGE_MASK);
    }

    if (impl->current_page_table->attributes[vaddr >> CITRA_PAGE_BITS] ==
            PageType::RasterizerCachedMemory ||
        impl->current_page_table->attributes[vaddr >> CITRA_PAGE_BITS] ==
            PageType::RasterizerCachedMemoryWatchpoint) {
        return GetPointerForRasterizerCache(vaddr);
    }

    LOG_ERROR(HW_Memory, "unknown GetPointer @ 0x{:08x}", vaddr);
    return nullptr;
}

std::string MemorySystem::ReadCString(VAddr vaddr, std::size_t max_length) {
    std::string string;
    string.reserve(max_length);
    for (std::size_t i = 0; i < max_length; ++i) {
        char c = Read8(vaddr);
        if (c == '\0') {
            break;
        }

        string.push_back(c);
        ++vaddr;
    }

    string.shrink_to_fit();
    return string;
}

MemorySystem::PhysMemRegionInfo MemorySystem::GetPhysMemRegionInfo(PAddr address) {
    if (address >= phys_mem_region_info_cache.region_start &&
        address < phys_mem_region_info_cache.region_end) {
        return phys_mem_region_info_cache;
    }

    constexpr std::array memory_areas = {
        std::make_pair(VRAM_PADDR, VRAM_SIZE),
        std::make_pair(DSP_RAM_PADDR, DSP_RAM_SIZE),
        std::make_pair(FCRAM_PADDR, FCRAM_N3DS_SIZE),
        std::make_pair(N3DS_EXTRA_RAM_PADDR, N3DS_EXTRA_RAM_SIZE),
    };

    const auto area = std::find_if(memory_areas.begin(), memory_areas.end(), [&](const auto& area) {
        // Note: the region end check is inclusive because the user can pass in an address that
        // represents an open right bound
        return address >= area.first && address <= area.first + area.second;
    });

    if (area == memory_areas.end()) [[unlikely]] {
        LOG_ERROR(HW_Memory, "Unknown GetPhysMemRegionInfo @ {:#08X} at PC {:#08X}", address,
                  impl->GetPC());
        phys_mem_region_info_cache = PhysMemRegionInfo();
        return phys_mem_region_info_cache;
    }

    switch (area->first) {
    case VRAM_PADDR:
        phys_mem_region_info_cache = {&impl->vram_mem, area->first, area->second};
        break;
    case DSP_RAM_PADDR:
        phys_mem_region_info_cache = {&impl->dsp_mem, area->first, area->second};
        break;
    case FCRAM_PADDR:
        phys_mem_region_info_cache = {&impl->fcram_mem, area->first, area->second};
        break;
    case N3DS_EXTRA_RAM_PADDR:
        phys_mem_region_info_cache = {&impl->n3ds_extra_ram_mem, area->first, area->second};
        break;
    default:
        UNREACHABLE();
    }

    return phys_mem_region_info_cache;
}

u8* MemorySystem::GetPhysicalPointer(PAddr address) {
    auto target_mem = GetPhysMemRegionInfo(address);

    if (!target_mem.valid()) [[unlikely]] {
        return {nullptr};
    }

    u32 offset_into_region = address - target_mem.region_start;
    return target_mem.backing_mem->get()->GetPtr() + offset_into_region;
}

MemoryRef MemorySystem::GetPhysicalRef(PAddr address) {
    const auto& target_mem = GetPhysMemRegionInfo(address);

    if (!target_mem.valid()) [[unlikely]] {
        return {nullptr};
    }

    u32 offset_into_region = address - target_mem.region_start;
    return {*target_mem.backing_mem, offset_into_region};
}

std::vector<VAddr> MemorySystem::PhysicalToVirtualAddressForRasterizer(PAddr addr) {
    if (addr >= VRAM_PADDR && addr < VRAM_PADDR_END) {
        return {addr - VRAM_PADDR + VRAM_VADDR};
    }
    // NOTE: Order matters here.
    PAddr plg_fb_addr = Plugin3GXFramebufferAddress();
    if (plg_fb_addr && addr >= plg_fb_addr && addr < plg_fb_addr + PLUGIN_3GX_FB_SIZE) {
        return {addr - plg_fb_addr + PLUGIN_3GX_FB_VADDR};
    }
    if (addr >= FCRAM_PADDR && addr < FCRAM_PADDR_END) {
        return {addr - FCRAM_PADDR + LINEAR_HEAP_VADDR, addr - FCRAM_PADDR + NEW_LINEAR_HEAP_VADDR};
    }
    if (addr >= FCRAM_PADDR_END && addr < FCRAM_N3DS_PADDR_END) {
        return {addr - FCRAM_PADDR + NEW_LINEAR_HEAP_VADDR};
    }
    // While the physical <-> virtual mapping is 1:1 for the regions supported by the cache,
    // some games (like Pokemon Super Mystery Dungeon) will try to use textures that go beyond
    // the end address of VRAM, causing the Virtual->Physical translation to fail when flushing
    // parts of the texture.
    LOG_ERROR(HW_Memory,
              "Trying to use invalid physical address for rasterizer: {:08X} at PC 0x{:08X}", addr,
              impl->GetPC());
    return {};
}

void MemorySystem::RasterizerMarkRegionCached(PAddr start, u32 size, bool cached) {
    if (start == 0) {
        return;
    }

    u32 num_pages = ((start + size - 1) >> CITRA_PAGE_BITS) - (start >> CITRA_PAGE_BITS) + 1;
    PAddr paddr = start;

    for (unsigned i = 0; i < num_pages; ++i, paddr += CITRA_PAGE_SIZE) {
        for (VAddr vaddr : PhysicalToVirtualAddressForRasterizer(paddr)) {
            impl->cache_marker.Mark(vaddr, cached);
            for (auto& page_table : impl->page_table_list) {
                PageType& page_type = page_table->attributes[vaddr >> CITRA_PAGE_BITS];

                if (cached) {
                    // Switch page type to cached if now cached
                    switch (page_type) {
                    case PageType::Unmapped:
                        // It is not necessary for a process to have this region mapped into its
                        // address space, for example, a system module need not have a VRAM mapping.
                        break;
                    case PageType::Memory:
                    case PageType::MemoryWatchpoint:
                        page_type = (page_type == PageType::Memory)
                                        ? PageType::RasterizerCachedMemory
                                        : PageType::RasterizerCachedMemoryWatchpoint;
                        page_table->pointers[vaddr >> CITRA_PAGE_BITS] = nullptr;
                        break;
                    default:
                        UNREACHABLE();
                    }
                } else {
                    // Switch page type to uncached if now uncached
                    switch (page_type) {
                    case PageType::Unmapped:
                        // It is not necessary for a process to have this region mapped into its
                        // address space, for example, a system module need not have a VRAM mapping.
                        break;
                    case PageType::RasterizerCachedMemory:
                    case PageType::RasterizerCachedMemoryWatchpoint: {
                        page_type = (page_type == PageType::RasterizerCachedMemory)
                                        ? PageType::Memory
                                        : PageType::MemoryWatchpoint;

                        if (page_type == PageType::Memory) {
                            page_table->pointers[vaddr >> CITRA_PAGE_BITS] =
                                GetPointerForRasterizerCache(vaddr & ~CITRA_PAGE_MASK);
                        }
                        break;
                    }
                    default:
                        UNREACHABLE();
                    }
                }
            }
        }
    }
}

u8 MemorySystem::Read8(const VAddr addr) {
    return Read<u8>(impl->current_page_table, addr);
}

u8 MemorySystem::Read8(const Kernel::Process& process, VAddr addr) {
    return Read<u8>(process.vm_manager.page_table, addr);
}

u16 MemorySystem::Read16(const VAddr addr) {
    return Read<u16_le>(impl->current_page_table, addr);
}

u16 MemorySystem::Read16(const Kernel::Process& process, VAddr addr) {
    return Read<u16_le>(process.vm_manager.page_table, addr);
}

u32 MemorySystem::Read32(const VAddr addr) {
    return Read<u32_le>(impl->current_page_table, addr);
}

u32 MemorySystem::Read32(const Kernel::Process& process, VAddr addr) {
    return Read<u32_le>(process.vm_manager.page_table, addr);
}

u64 MemorySystem::Read64(const VAddr addr) {
    return Read<u64_le>(impl->current_page_table, addr);
}

u64 MemorySystem::Read64(const Kernel::Process& process, VAddr addr) {
    return Read<u64_le>(process.vm_manager.page_table, addr);
}

std::optional<u32> MemorySystem::Read32OrNullopt(VAddr addr) {
    return Read<std::optional<u32_le>>(impl->current_page_table, addr);
}

std::optional<u32> MemorySystem::Read32OrNullopt(const Kernel::Process& process, VAddr addr) {
    return Read<std::optional<u32_le>>(process.vm_manager.page_table, addr);
}

void MemorySystem::ReadBlock(const Kernel::Process& process, const VAddr src_addr,
                             void* dest_buffer, const std::size_t size) {
    return impl->ReadBlockImpl<false>(process, src_addr, dest_buffer, size);
}

void MemorySystem::ReadBlock(VAddr src_addr, void* dest_buffer, std::size_t size) {
    const auto& process = *impl->system.Kernel().GetCurrentProcess();
    return impl->ReadBlockImpl<false>(process, src_addr, dest_buffer, size);
}

void MemorySystem::Write8(const VAddr addr, const u8 data) {
    Write<u8>(impl->current_page_table, addr, data);
}

void MemorySystem::Write8(const Kernel::Process& process, const VAddr addr, const u8 data) {
    Write<u8>(process.vm_manager.page_table, addr, data);
}

void MemorySystem::Write16(const VAddr addr, const u16 data) {
    Write<u16_le>(impl->current_page_table, addr, data);
}

void MemorySystem::Write16(const Kernel::Process& process, const VAddr addr, const u16 data) {
    Write<u16_le>(process.vm_manager.page_table, addr, data);
}

void MemorySystem::Write32(const VAddr addr, const u32 data) {
    Write<u32_le>(impl->current_page_table, addr, data);
}

void MemorySystem::Write32(const Kernel::Process& process, const VAddr addr, const u32 data) {
    Write<u32_le>(process.vm_manager.page_table, addr, data);
}

void MemorySystem::Write64(const VAddr addr, const u64 data) {
    Write<u64_le>(impl->current_page_table, addr, data);
}

void MemorySystem::Write64(const Kernel::Process& process, const VAddr addr, const u64 data) {
    Write<u64_le>(process.vm_manager.page_table, addr, data);
}

bool MemorySystem::WriteExclusive8(const VAddr addr, const u8 data, const u8 expected) {
    return WriteExclusive<u8>(addr, data, expected);
}

bool MemorySystem::WriteExclusive16(const VAddr addr, const u16 data, const u16 expected) {
    return WriteExclusive<u16_le>(addr, data, expected);
}

bool MemorySystem::WriteExclusive32(const VAddr addr, const u32 data, const u32 expected) {
    return WriteExclusive<u32_le>(addr, data, expected);
}

bool MemorySystem::WriteExclusive64(const VAddr addr, const u64 data, const u64 expected) {
    return WriteExclusive<u64_le>(addr, data, expected);
}

void MemorySystem::WriteBlock(const Kernel::Process& process, const VAddr dest_addr,
                              const void* src_buffer, const std::size_t size) {
    return impl->WriteBlockImpl<false>(process, dest_addr, src_buffer, size);
}

void MemorySystem::WriteBlock(const VAddr dest_addr, const void* src_buffer,
                              const std::size_t size) {
    auto& process = *impl->system.Kernel().GetCurrentProcess();
    return impl->WriteBlockImpl<false>(process, dest_addr, src_buffer, size);
}

void MemorySystem::ZeroBlock(const Kernel::Process& process, const VAddr dest_addr,
                             const std::size_t size) {
    auto& page_table = *process.vm_manager.page_table;
    std::size_t remaining_size = size;
    std::size_t page_index = dest_addr >> CITRA_PAGE_BITS;
    std::size_t page_offset = dest_addr & CITRA_PAGE_MASK;

    while (remaining_size > 0) {
        const std::size_t copy_amount = std::min(CITRA_PAGE_SIZE - page_offset, remaining_size);
        const VAddr current_vaddr =
            static_cast<VAddr>((page_index << CITRA_PAGE_BITS) + page_offset);

        switch (page_table.attributes[page_index]) {
        case PageType::Unmapped: {
            LOG_ERROR(HW_Memory,
                      "unmapped ZeroBlock @ 0x{:08X} (start address = 0x{:08X}, size = {}) at PC "
                      "0x{:08X}",
                      current_vaddr, dest_addr, size, impl->GetPC());
            break;
        }
        case PageType::Memory: {
            DEBUG_ASSERT(page_table.pointers[page_index]);

            u8* dest_ptr = page_table.pointers[page_index] + page_offset;
            std::memset(dest_ptr, 0, copy_amount);
            break;
        }
        case PageType::MemoryWatchpoint: {
            auto it = page_table.watchpoint_pages_map.find(page_index);
            ASSERT_MSG(it != page_table.watchpoint_pages_map.end(),
                       "Missing memory for watchpoint page");

            u8* dest_ptr = it->second.memory.GetPtr() + page_offset;
            std::memset(dest_ptr, 0, copy_amount);
            break;
        }
        case PageType::RasterizerCachedMemory:
        case PageType::RasterizerCachedMemoryWatchpoint: {
            RasterizerFlushVirtualRegion(current_vaddr, static_cast<u32>(copy_amount),
                                         FlushMode::Invalidate);
            std::memset(GetPointerForRasterizerCache(current_vaddr), 0, copy_amount);
            break;
        }
        default:
            UNREACHABLE();
        }

        page_index++;
        page_offset = 0;
        remaining_size -= copy_amount;
    }
}

void MemorySystem::CopyBlock(const Kernel::Process& process, VAddr dest_addr, VAddr src_addr,
                             const std::size_t size) {
    CopyBlock(process, process, dest_addr, src_addr, size);
}

void MemorySystem::CopyBlock(const Kernel::Process& dest_process,
                             const Kernel::Process& src_process, VAddr dest_addr, VAddr src_addr,
                             std::size_t size) {
    auto& page_table = *src_process.vm_manager.page_table;
    std::size_t remaining_size = size;
    std::size_t page_index = src_addr >> CITRA_PAGE_BITS;
    std::size_t page_offset = src_addr & CITRA_PAGE_MASK;

    while (remaining_size > 0) {
        const std::size_t copy_amount = std::min(CITRA_PAGE_SIZE - page_offset, remaining_size);
        const VAddr current_vaddr =
            static_cast<VAddr>((page_index << CITRA_PAGE_BITS) + page_offset);

        switch (page_table.attributes[page_index]) {
        case PageType::Unmapped: {
            LOG_ERROR(HW_Memory,
                      "unmapped CopyBlock @ 0x{:08X} (start address = 0x{:08X}, size = {}) at PC "
                      "0x{:08X}",
                      current_vaddr, src_addr, size, impl->GetPC());
            ZeroBlock(dest_process, dest_addr, copy_amount);
            break;
        }
        case PageType::Memory: {
            DEBUG_ASSERT(page_table.pointers[page_index]);
            const u8* src_ptr = page_table.pointers[page_index] + page_offset;
            WriteBlock(dest_process, dest_addr, src_ptr, copy_amount);
            break;
        }
        case PageType::MemoryWatchpoint: {
            auto it = page_table.watchpoint_pages_map.find(page_index);
            ASSERT_MSG(it != page_table.watchpoint_pages_map.end(),
                       "Missing memory for watchpoint page");

            const u8* src_ptr = it->second.memory.GetPtr() + page_offset;
            WriteBlock(dest_process, dest_addr, src_ptr, copy_amount);
            break;
        }
        case PageType::RasterizerCachedMemory:
        case PageType::RasterizerCachedMemoryWatchpoint: {
            RasterizerFlushVirtualRegion(current_vaddr, static_cast<u32>(copy_amount),
                                         FlushMode::Flush);
            WriteBlock(dest_process, dest_addr, GetPointerForRasterizerCache(current_vaddr),
                       copy_amount);
            break;
        }
        default:
            UNREACHABLE();
        }

        page_index++;
        page_offset = 0;
        dest_addr += static_cast<VAddr>(copy_amount);
        src_addr += static_cast<VAddr>(copy_amount);
        remaining_size -= copy_amount;
    }
}

u32 MemorySystem::GetFCRAMOffset(const u8* pointer) const {
    ASSERT(pointer >= impl->fcram.get() && pointer <= impl->fcram.get() + Memory::FCRAM_N3DS_SIZE);
    return static_cast<u32>(pointer - impl->fcram.get());
}

u8* MemorySystem::GetFCRAMPointer(std::size_t offset) {
    ASSERT(offset <= Memory::FCRAM_N3DS_SIZE);
    return impl->fcram.get() + offset;
}

const u8* MemorySystem::GetFCRAMPointer(std::size_t offset) const {
    ASSERT(offset <= Memory::FCRAM_N3DS_SIZE);
    return impl->fcram.get() + offset;
}

MemoryRef MemorySystem::GetFCRAMRef(std::size_t offset) const {
    ASSERT(offset <= Memory::FCRAM_N3DS_SIZE);
    return MemoryRef(impl->fcram_mem, offset);
}

u8* MemorySystem::GetDspMemory(std::size_t offset) const {
    ASSERT(offset <= Memory::DSP_RAM_SIZE);
    return impl->dsp_ram.get() + offset;
}

} // namespace Memory
