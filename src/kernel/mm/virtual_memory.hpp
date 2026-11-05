// Long-mode page-table manager for the higher-half kernel and per-process user
// address spaces.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mm/page_frame.hpp"

enum class PageFlags : uint64_t
{
    None = 0,
    Present = 1ull << 0,
    Write = 1ull << 1,
    User = 1ull << 2,
    SharedBacking = 1ull << 9,
    NoExecute = 1ull << 63,
};

inline constexpr PageFlags operator|(PageFlags left, PageFlags right)
{
    return static_cast<PageFlags>(static_cast<uint64_t>(left) | static_cast<uint64_t>(right));
}

inline constexpr PageFlags operator&(PageFlags left, PageFlags right)
{
    return static_cast<PageFlags>(static_cast<uint64_t>(left) & static_cast<uint64_t>(right));
}

inline constexpr PageFlags& operator|=(PageFlags& left, PageFlags right)
{
    left = left | right;
    return left;
}

class VirtualMemory
{
public:
    static const uint64_t PAGE_PRESENT = 1ull << 0;
    static const uint64_t PAGE_WRITE = 1ull << 1;

    // Construct a manager around a new or existing PML4 root.
    VirtualMemory(PageFrameContainer& frames, uint64_t existing_root = ~0ull);

    // attach this wrapper to an externally owned page-table root.
    void attach(uint64_t root);
    // Map existing physical pages into a virtual range with explicit flags.
    bool map_physical(uint64_t virtual_address,
                      uint64_t physical_address,
                      uint64_t num_pages,
                      PageFlags flags);
    // Map existing physical memory with 2 MiB leaves for early large ranges.
    bool map_physical_2m(uint64_t virtual_address,
                         uint64_t physical_address,
                         uint64_t num_pages,
                         PageFlags flags);
    // allocate physical pages, zero them, and map them into a virtual range.
    bool allocate_and_map(uint64_t virtual_address,
                          uint64_t num_pages,
                          PageFlags flags,
                          uint64_t* first_physical = nullptr);
    // Replace permissions on already-mapped pages.
    bool protect(uint64_t virtual_address, uint64_t num_pages, PageFlags flags);
    // Clear leaves without releasing shared backing or page-table reservations.
    bool detach(uint64_t address, uint64_t pages);
    // translate one virtual address and report its physical address plus leaf flags.
    bool translate(uint64_t virtual_address, uint64_t& physical_address, uint64_t& flags) const;
    // Clone the steady-state supervisor mappings required in user CR3s.
    bool clone_kernel_mappings(uint64_t source_root);
    // Destroy an entire PML4 slot and optionally all user leaf pages beneath it.
    bool destroy_user_slot(uint64_t slot);
    // free mapped physical pages in a virtual range.
    bool free(uint64_t start_address, uint64_t num_pages);
    // Destroy this entire page table hierarchy.
    bool free();
    // Return the active root physical address for this wrapper.
    uint64_t root();
    // Load this root into CR3.
    bool activate();

private:
    PageFrameContainer& frames_;
    bool initialized_;
    uint64_t root_;

    bool ensure_root(void);
    bool ensure_table_entry(uint64_t& entry, bool user_visible);
    bool walk_to_leaf(uint64_t virtual_address,
                      bool create,
                      bool user_visible,
                      uint64_t** leaf_entry);
    void destroy_table(uint64_t& entry, int level, bool free_leaf_pages);
    static uint64_t flags_to_entry(PageFlags flags);
    static uint64_t table_entry_flags(bool user_visible);
};
