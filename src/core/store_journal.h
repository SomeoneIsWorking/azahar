// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>
#include "common/common_types.h"
#include "core/memory.h"

namespace Kernel {
class Process;
}

namespace Memory {

// Copy-on-first-store record of a process's writable memory. While armed, the page table holds no
// pointer for the writable pages it has not saved yet, so every store reaches the memory system's
// slow path, which saves the page once and gives it its pointer back. Rollback puts every saved
// page back as it was before its first store. Stores through pointers taken before arming (shared
// memory blocks the HLE services write) are not seen.
class StoreJournal {
public:
    struct Page {
        u8* host;
        VAddr vaddr;
        bool cached;
        std::unique_ptr<std::array<u8, CITRA_PAGE_SIZE>> bytes;
    };

    explicit StoreJournal(MemorySystem& memory);

    void Arm(const Kernel::Process& process);
    void Disarm();
    [[nodiscard]] bool Armed() const {
        return table != nullptr;
    }
    // Whether the journal wants page_index's pointer kept null.
    [[nodiscard]] bool Holds(const PageTable& page_table, std::size_t page_index) const;

    // Saves the page behind page_index before its first store, and restores its pointer.
    void Touch(PageTable& page_table, std::size_t page_index, u8* host_page);

    [[nodiscard]] const std::vector<Page>& Pages() const {
        return pages;
    }
    // Loads that took the slow path because their page was held: the journal's cost.
    void CountRead() {
        ++held_reads;
    }
    [[nodiscard]] u64 HeldReads() const {
        return held_reads;
    }
    // Puts every saved page back and forgets them; disarmed first.
    void Rollback();
    // Swaps every saved page with memory: twice is no change; disarmed first.
    void Exchange();
    // The saved bytes of the page behind host_page, or null when it was not stored to.
    [[nodiscard]] const u8* SavedBytes(const u8* host_page) const;
    // Forgets every saved page, keeping memory as it is.
    void Clear();

private:
    MemorySystem& memory;
    PageTable* table = nullptr;
    std::vector<std::size_t> armed;
    std::vector<bool> held;
    std::unordered_map<u8*, std::size_t> saved;
    std::vector<Page> pages;
    u64 held_reads = 0;
};

} // namespace Memory
