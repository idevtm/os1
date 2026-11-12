// Long-mode page-table implementation. This module owns root allocation,
// four-level walks, permission updates, user-slot teardown, and CR3 activation.
#include "mm/virtual_memory.hpp"

#include "arch/x86_64/apic/ipi.hpp"
#include "debug/debug.hpp"
#include "handoff/memory_layout.h"
#include "util/memory.h"

namespace
{
constexpr uint64_t kPageMask = ~(kPageSize - 1);
constexpr uint64_t kLargePageSize2MiB = 0x200000ull;
constexpr uint64_t kLargePageMask2MiB = ~(kLargePageSize2MiB - 1);
constexpr uint64_t kLargePageOffsetMask2MiB = kLargePageSize2MiB - 1;
constexpr uint64_t kEntryLargePage = 1ull << 7;
// Long-mode page-table entries carry the NX bit in the high flag range, so the
// physical-address mask must exclude it explicitly instead of just clearing the
// low page-offset bits.
constexpr uint64_t kEntryAddressMask = 0x000FFFFFFFFFF000ull;
constexpr uint64_t kLargePageAddressMask2MiB = 0x000FFFFFFFE00000ull;

[[nodiscard]] inline uint64_t page_index(uint64_t virtual_address, unsigned shift)
{
    return (virtual_address >> shift) & 0x1FFull;
}

[[nodiscard]] inline bool address_is_in_user_slot(uint64_t virtual_address)
{
    return page_index(virtual_address, 39) == kUserPml4Index;
}

void maybe_shootdown_user_mappings(uint64_t root, uint64_t virtual_address)
{
    if((0 == root) || (~0ull == root) || !address_is_in_user_slot(virtual_address))
    {
        return;
    }

    (void)ipi_send_tlb_shootdown();
}

void maybe_shootdown_user_slot(uint64_t root, uint64_t slot)
{
    if((0 == root) || (~0ull == root) || (slot != kUserPml4Index))
    {
        return;
    }

    (void)ipi_send_tlb_shootdown();
}
}  // namespace

VirtualMemory::VirtualMemory(PageFrameContainer& frames, uint64_t existing_root)
    : frames_(frames), initialized_(existing_root != ~0ull), root_(existing_root)
{
}

void VirtualMemory::attach(uint64_t root)
{
    root_ = root;
    initialized_ = (root != ~0ull);
}

bool VirtualMemory::ensure_root(void)
{
    if(initialized_)
    {
        return true;
    }

    if(!frames_.allocate(root_))
    {
        return false;
    }

    memsetq(kernel_physical_pointer<void>(root_), 0, kPageSize);
    initialized_ = true;
    debug("root alloc 0x")(root_, 16)();
    return true;
}

uint64_t VirtualMemory::flags_to_entry(PageFlags flags)
{
    return static_cast<uint64_t>(flags);
}

uint64_t VirtualMemory::table_entry_flags(bool user_visible)
{
    uint64_t entry = static_cast<uint64_t>(PageFlags::Present | PageFlags::Write);
    if(user_visible)
    {
        entry |= static_cast<uint64_t>(PageFlags::User);
    }
    return entry;
}

bool VirtualMemory::ensure_table_entry(uint64_t& entry, bool user_visible)
{
    if(0 == entry)
    {
        uint64_t new_page = 0;
        if(!frames_.allocate(new_page))
        {
            return false;
        }
        memsetq(kernel_physical_pointer<void>(new_page), 0, kPageSize);
        entry = (new_page & kEntryAddressMask) | table_entry_flags(user_visible);
        return true;
    }

    if(user_visible)
    {
        entry |= static_cast<uint64_t>(PageFlags::User);
    }

    return true;
}

bool VirtualMemory::walk_to_leaf(uint64_t virtual_address,
                                 bool create,
                                 bool user_visible,
                                 uint64_t** leaf_entry)
{
    if((nullptr == leaf_entry) || !ensure_root())
    {
        return false;
    }

    uint64_t* pml4 = kernel_physical_pointer<uint64_t>(root_);
    uint64_t* pml3 = nullptr;
    uint64_t* pml2 = nullptr;
    uint64_t* pml1 = nullptr;
    const uint64_t required = user_visible ? table_entry_flags(true) : 0;
    const uint64_t table_mask = user_visible ? required | kEntryLargePage : 0;

    uint64_t& pml4e = pml4[page_index(virtual_address, 39)];
    if(create)
    {
        if(!ensure_table_entry(pml4e, user_visible))
        {
            return false;
        }
    }
    else if(pml4e == 0 || (pml4e & required) != required)
    {
        return false;
    }
    pml3 = kernel_physical_pointer<uint64_t>(pml4e & kEntryAddressMask);

    uint64_t& pml3e = pml3[page_index(virtual_address, 30)];
    if(create)
    {
        if(!ensure_table_entry(pml3e, user_visible))
        {
            return false;
        }
    }
    else if(pml3e == 0 || (pml3e & table_mask) != required)
    {
        return false;
    }
    pml2 = kernel_physical_pointer<uint64_t>(pml3e & kEntryAddressMask);

    uint64_t& pml2e = pml2[page_index(virtual_address, 21)];
    if(create)
    {
        if(!ensure_table_entry(pml2e, user_visible))
        {
            return false;
        }
    }
    else if(pml2e == 0 || (pml2e & table_mask) != required)
    {
        return false;
    }
    pml1 = kernel_physical_pointer<uint64_t>(pml2e & kEntryAddressMask);

    *leaf_entry = &pml1[page_index(virtual_address, 12)];
    return true;
}

bool VirtualMemory::map_physical(uint64_t virtual_address,
                                 uint64_t physical_address,
                                 uint64_t num_pages,
                                 PageFlags flags)
{
    if((0 == num_pages) || (virtual_address & (kPageSize - 1)) ||
       (physical_address & (kPageSize - 1)))
    {
        return false;
    }

    const bool user_visible = (PageFlags::User == (flags & PageFlags::User));
    for(uint64_t i = 0; i < num_pages; ++i)
    {
        uint64_t* leaf_entry = nullptr;
        if(!walk_to_leaf(virtual_address + i * kPageSize, true, user_visible, &leaf_entry))
        {
            return false;
        }
        *leaf_entry = ((physical_address + i * kPageSize) & kEntryAddressMask) |
                      flags_to_entry(flags | PageFlags::Present);
    }

    return true;
}

bool VirtualMemory::map_reserved_user_page(uint64_t virtual_address,
                                           uint64_t physical_address,
                                           PageFlags flags)
{
    if(!initialized_ || ((virtual_address | physical_address) & (kPageSize - 1)))
        return false;

    uint64_t* leaf = nullptr;
    if(!walk_to_leaf(virtual_address, false, true, &leaf) || *leaf)
        return false;
    *leaf = (physical_address & kEntryAddressMask) |
            flags_to_entry(flags | PageFlags::Present | PageFlags::User);
    return true;
}

bool VirtualMemory::map_physical_2m(uint64_t virtual_address,
                                    uint64_t physical_address,
                                    uint64_t num_pages,
                                    PageFlags flags)
{
    if((0 == num_pages) || (virtual_address & kLargePageOffsetMask2MiB) ||
       (physical_address & kLargePageOffsetMask2MiB))
    {
        return false;
    }

    const bool user_visible = (PageFlags::User == (flags & PageFlags::User));
    for(uint64_t i = 0; i < num_pages; ++i)
    {
        const uint64_t current_virtual = virtual_address + i * kLargePageSize2MiB;
        const uint64_t current_physical = physical_address + i * kLargePageSize2MiB;

        if(!ensure_root())
        {
            return false;
        }

        uint64_t* pml4 = kernel_physical_pointer<uint64_t>(root_);
        uint64_t& pml4e = pml4[page_index(current_virtual, 39)];
        if(!ensure_table_entry(pml4e, user_visible))
        {
            return false;
        }

        uint64_t* pml3 = kernel_physical_pointer<uint64_t>(pml4e & kEntryAddressMask);
        uint64_t& pml3e = pml3[page_index(current_virtual, 30)];
        if(!ensure_table_entry(pml3e, user_visible))
        {
            return false;
        }

        uint64_t* pml2 = kernel_physical_pointer<uint64_t>(pml3e & kEntryAddressMask);
        uint64_t& pml2e = pml2[page_index(current_virtual, 21)];
        pml2e = (current_physical & kLargePageAddressMask2MiB) |
                flags_to_entry(flags | PageFlags::Present) | kEntryLargePage;
    }

    return true;
}

bool VirtualMemory::allocate_and_map(uint64_t virtual_address,
                                     uint64_t num_pages,
                                     PageFlags flags,
                                     uint64_t* first_physical)
{
    if((0 == num_pages) || (virtual_address & (kPageSize - 1)))
    {
        return false;
    }

    uint64_t first_page = 0;
    for(uint64_t i = 0; i < num_pages; ++i)
    {
        uint64_t physical_page = 0;
        if(!frames_.allocate(physical_page))
        {
            return false;
        }
        if(0 == i)
        {
            first_page = physical_page;
        }
        memsetq(kernel_physical_pointer<void>(physical_page), 0, kPageSize);
        if(!map_physical(
               virtual_address + i * kPageSize, physical_page, 1, flags | PageFlags::Present))
        {
            // This data frame never became owned by the page tables. Earlier
            // mappings remain owned by the address space and its teardown.
            frames_.free(physical_page);
            return false;
        }
    }

    if(first_physical)
    {
        *first_physical = first_page;
    }

    return true;
}

bool VirtualMemory::protect(uint64_t virtual_address, uint64_t num_pages, PageFlags flags)
{
    if((0 == num_pages) || (virtual_address & (kPageSize - 1)))
    {
        return false;
    }

    for(uint64_t i = 0; i < num_pages; ++i)
    {
        uint64_t* leaf_entry = nullptr;
        if(!walk_to_leaf(virtual_address + i * kPageSize, false, false, &leaf_entry) ||
           (nullptr == leaf_entry) || (0 == *leaf_entry))
        {
            return false;
        }
        const uint64_t physical_page = *leaf_entry & kEntryAddressMask;
        *leaf_entry = physical_page | flags_to_entry(flags | PageFlags::Present);
    }

    maybe_shootdown_user_mappings(root_, virtual_address);

    return true;
}

bool VirtualMemory::detach(uint64_t address, uint64_t pages)
{
    for(uint64_t i = 0; i < pages; ++i)
    {
        uint64_t* leaf = nullptr;
        if(!walk_to_leaf(address + i * kPageSize, false, false, &leaf))
            return false;
        *leaf = 0;
    }
    return true;
}

bool VirtualMemory::translate(uint64_t virtual_address,
                              uint64_t& physical_address,
                              uint64_t& flags) const
{
    if(!initialized_)
    {
        return false;
    }

    const uint64_t* pml4 = kernel_physical_pointer<const uint64_t>(root_);
    const uint64_t pml4e = pml4[page_index(virtual_address, 39)];
    if(0 == (pml4e & PAGE_PRESENT))
    {
        return false;
    }
    const uint64_t* pml3 = kernel_physical_pointer<const uint64_t>(pml4e & kEntryAddressMask);
    const uint64_t pml3e = pml3[page_index(virtual_address, 30)];
    if(0 == (pml3e & PAGE_PRESENT))
    {
        return false;
    }
    const uint64_t* pml2 = kernel_physical_pointer<const uint64_t>(pml3e & kEntryAddressMask);
    const uint64_t pml2e = pml2[page_index(virtual_address, 21)];
    if(0 == (pml2e & PAGE_PRESENT))
    {
        return false;
    }
    if(0 != (pml2e & kEntryLargePage))
    {
        flags = pml2e & ~kLargePageAddressMask2MiB;
        physical_address =
            (pml2e & kLargePageAddressMask2MiB) | (virtual_address & kLargePageOffsetMask2MiB);
        return true;
    }
    const uint64_t* pml1 = kernel_physical_pointer<const uint64_t>(pml2e & kEntryAddressMask);
    const uint64_t pml1e = pml1[page_index(virtual_address, 12)];
    if(0 == (pml1e & PAGE_PRESENT))
    {
        return false;
    }

    flags = pml1e & ~kEntryAddressMask;
    physical_address = (pml1e & kEntryAddressMask) | (virtual_address & ~kPageMask);
    return true;
}

bool VirtualMemory::clone_kernel_mappings(uint64_t source_root)
{
    if(!ensure_root())
    {
        return false;
    }

    uint64_t* target = kernel_physical_pointer<uint64_t>(root_);
    const uint64_t* source = kernel_physical_pointer<const uint64_t>(source_root);
    // Firmware can place PCI BARs beyond the first 512 GiB direct-map slot.
    // Share the complete supervisor half, including those device mappings.
    // Keep the user half private and deny user access at the top level.
    for(unsigned slot = 256; slot < 512; ++slot)
        target[slot] = source[slot] & ~static_cast<uint64_t>(PageFlags::User);
    return true;
}

void VirtualMemory::destroy_table(uint64_t& entry, int level, bool free_leaf_pages)
{
    if(0 == entry)
    {
        return;
    }

    uint64_t* table = kernel_physical_pointer<uint64_t>(entry & kEntryAddressMask);
    if(level > 1)
    {
        for(int i = 0; i < 512; ++i)
        {
            if(table[i])
            {
                destroy_table(table[i], level - 1, free_leaf_pages);
            }
        }
    }
    else if(free_leaf_pages)
    {
        for(int i = 0; i < 512; ++i)
        {
            if(table[i])
            {
                if(!(table[i] & static_cast<uint64_t>(PageFlags::SharedBacking)))
                    frames_.free(table[i] & kEntryAddressMask);
                table[i] = 0;
            }
        }
    }

    frames_.free(entry & kEntryAddressMask);
    entry = 0;
}

bool VirtualMemory::destroy_user_slot(uint64_t slot)
{
    if(!initialized_ || (slot >= 512))
    {
        return false;
    }

    uint64_t* pml4 = kernel_physical_pointer<uint64_t>(root_);
    // A PML4 entry points to a level-three table, not another PML4.
    destroy_table(pml4[slot], 3, true);
    maybe_shootdown_user_slot(root_, slot);
    return true;
}

bool VirtualMemory::free(uint64_t start_address, uint64_t num_pages)
{
    if((0 == num_pages) || (start_address & (kPageSize - 1)))
    {
        return false;
    }

    for(uint64_t i = 0; i < num_pages; ++i)
    {
        uint64_t* leaf_entry = nullptr;
        if(!walk_to_leaf(start_address + i * kPageSize, false, false, &leaf_entry) ||
           (nullptr == leaf_entry) || (0 == *leaf_entry))
        {
            return false;
        }
        if(!(*leaf_entry & static_cast<uint64_t>(PageFlags::SharedBacking)))
            frames_.free(*leaf_entry & kEntryAddressMask);
        *leaf_entry = 0;
    }

    maybe_shootdown_user_mappings(root_, start_address);

    return true;
}

bool VirtualMemory::free()
{
    if(initialized_)
    {
        uint64_t* pml4 = kernel_physical_pointer<uint64_t>(root_);
        for(int i = 0; i < 512; ++i)
        {
            if(pml4[i])
            {
                destroy_table(pml4[i], 3, true);
            }
        }
        frames_.free(root_);
        root_ = ~0ull;
        initialized_ = false;
    }
    return true;
}

uint64_t VirtualMemory::root()
{
    if(!initialized_)
    {
        debug("VirtualMemory::root() warning: VirtualMemory not initialized")();
    }
    return root_;
}

bool VirtualMemory::activate()
{
    if(!initialized_)
    {
        return false;
    }
#if defined(OS1_HOST_TEST)
    return true;
#else
    asm volatile("mov %0, %%cr3" : : "r"(root_));
    return true;
#endif
}
