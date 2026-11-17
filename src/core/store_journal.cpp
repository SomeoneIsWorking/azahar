// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/vm_manager.h"
#include "core/store_journal.h"

namespace Memory {

StoreJournal::StoreJournal(MemorySystem& memory_) : memory(memory_) {}

void StoreJournal::Arm(const Kernel::Process& process) {
    Disarm();
    table = process.vm_manager.page_table.get();
    if (held.empty()) {
        held.assign(PAGE_TABLE_NUM_ENTRIES, false);
    }
    auto& raw = table->GetPointerArray();
    for (const auto& [base, vma] : process.vm_manager.vma_map) {
        if (vma.type != Kernel::VMAType::BackingMemory ||
            (static_cast<u32>(vma.permissions) & static_cast<u32>(Kernel::VMAPermission::Write)) ==
                0) {
            continue;
        }
        const std::size_t first = base >> CITRA_PAGE_BITS;
        const std::size_t end = first + (vma.size >> CITRA_PAGE_BITS);
        for (std::size_t index = first; index < end; ++index) {
            if (table->attributes[index] != PageType::Memory || raw[index] == nullptr ||
                saved.contains(raw[index])) {
                continue;
            }
            raw[index] = nullptr;
            held[index] = true;
            armed.push_back(index);
        }
    }
}

void StoreJournal::Disarm() {
    if (table == nullptr) {
        return;
    }
    auto& raw = table->GetPointerArray();
    for (const std::size_t index : armed) {
        if (!held[index]) {
            continue;
        }
        held[index] = false;
        if (table->attributes[index] == PageType::Memory) {
            raw[index] = table->pointers.Backing(index);
        }
    }
    armed.clear();
    table = nullptr;
}

bool StoreJournal::Holds(const PageTable& page_table, std::size_t page_index) const {
    return &page_table == table && held[page_index];
}

void StoreJournal::Touch(PageTable& page_table, std::size_t page_index, u8* host_page) {
    if (host_page != nullptr && !saved.contains(host_page)) {
        auto bytes = std::make_unique<std::array<u8, CITRA_PAGE_SIZE>>();
        std::memcpy(bytes->data(), host_page, CITRA_PAGE_SIZE);
        saved.emplace(host_page, pages.size());
        const bool cached =
            page_table.attributes[page_index] == PageType::RasterizerCachedMemory ||
            page_table.attributes[page_index] == PageType::RasterizerCachedMemoryWatchpoint;
        pages.push_back({host_page, static_cast<VAddr>(page_index << CITRA_PAGE_BITS), cached,
                         std::move(bytes)});
    }
    if (Holds(page_table, page_index)) {
        held[page_index] = false;
        if (page_table.attributes[page_index] == PageType::Memory) {
            page_table.GetPointerArray()[page_index] = page_table.pointers.Backing(page_index);
        }
    }
}

void StoreJournal::Rollback() {
    for (const Page& page : pages) {
        std::memcpy(page.host, page.bytes->data(), CITRA_PAGE_SIZE);
        if (page.cached) {
            memory.RasterizerFlushVirtualRegion(page.vaddr, CITRA_PAGE_SIZE, FlushMode::Invalidate);
        }
    }
    Clear();
}

void StoreJournal::Exchange() {
    std::array<u8, CITRA_PAGE_SIZE> held_bytes;
    for (const Page& page : pages) {
        std::memcpy(held_bytes.data(), page.host, CITRA_PAGE_SIZE);
        std::memcpy(page.host, page.bytes->data(), CITRA_PAGE_SIZE);
        *page.bytes = held_bytes;
        if (page.cached) {
            memory.RasterizerFlushVirtualRegion(page.vaddr, CITRA_PAGE_SIZE, FlushMode::Invalidate);
        }
    }
}

const u8* StoreJournal::SavedBytes(const u8* host_page) const {
    const auto it = saved.find(const_cast<u8*>(host_page));
    return it == saved.end() ? nullptr : pages[it->second].bytes->data();
}

void StoreJournal::Clear() {
    pages.clear();
    saved.clear();
    held_reads = 0;
}

} // namespace Memory
