// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Frontend-private libretro extensions for test harnesses. A frontend that does not know these
// commands returns false and sees no difference.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "libretro.h"

// Core to frontend: const struct azahar_debug_interface *, valid until retro_unload_game.
#define AZAHAR_ENVIRONMENT_SET_DEBUG_INTERFACE (RETRO_ENVIRONMENT_PRIVATE | 1)

struct azahar_write_hit {
    uint32_t address;
    uint32_t size;
    uint64_t value;
    // The running core's PC and LR when the write was handled. Under the JIT these are the values
    // at the start of the translated block (LR may be stale); the interpreter gives exact ones.
    uint32_t pc;
    uint32_t lr;
};

typedef void (*azahar_write_hit_t)(const struct azahar_write_hit* hit);

struct azahar_debug_interface {
    // Guest virtual memory of the running game process, as its CPU sees it now (code, data, bss,
    // heap). False if any page of the range is unmapped.
    bool (*read_memory)(uint32_t address, void* out, uint32_t size);
    bool (*write_memory)(uint32_t address, const void* data, uint32_t size);
    // Reports every guest CPU write that overlaps [address, address + size).
    bool (*watch_writes)(uint32_t address, uint32_t size);
    void (*clear_watches)(void);
    void (*set_write_hit)(azahar_write_hit_t callback);
};
