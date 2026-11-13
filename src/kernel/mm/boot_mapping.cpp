// Identity-mapping policy for boot-critical physical ranges.
#include "mm/boot_mapping.hpp"

#include "handoff/memory_layout.h"
#if !defined(OS1_HOST_TEST)
#include "arch/x86_64/cpu/x86.hpp"
#endif
#include "mm/virtual_memory.hpp"
#include "util/align.hpp"

namespace
{
constexpr uint64_t kLargePageSize2MiB = 0x200000ull;

bool page_is_already_mapped(VirtualMemory& vm, uint64_t virtual_address, uint64_t physical_address)
{
    uint64_t translated_physical = 0;
    uint64_t translated_flags = 0;
    return vm.translate(virtual_address, translated_physical, translated_flags) &&
           (translated_physical == physical_address);
}

bool large_page_is_already_mapped(VirtualMemory& vm,
                                  uint64_t virtual_address,
                                  uint64_t physical_address)
{
    uint64_t translated_physical = 0;
    uint64_t flags = 0;
    return vm.translate(virtual_address, translated_physical, flags) &&
           translated_physical == physical_address && (flags & VirtualMemory::PAGE_LARGE);
}
}  // namespace

bool map_bootstrap_identity_range(VirtualMemory& vm, uint64_t physical_start, uint64_t length)
{
    if(0 == length)
    {
        return true;
    }

    const uint64_t start = align_down(physical_start, kPageSize);
    const uint64_t end = align_up(physical_start + length, kPageSize);
    return vm.map_physical(
        start, start, (end - start) / kPageSize, PageFlags::Present | PageFlags::Write);
}

namespace
{
bool map_direct(VirtualMemory& vm, uint64_t physical_start, uint64_t length, PageFlags extra);
}

bool map_direct_range(VirtualMemory& vm, uint64_t physical_start, uint64_t length)
{
    return map_direct(vm, physical_start, length, PageFlags::None);
}

bool map_framebuffer_range(VirtualMemory& vm, uint64_t physical_start, uint64_t length)
{
    return map_direct(vm, physical_start, length, PageFlags::WriteCombine);
}

namespace
{
bool map_direct(VirtualMemory& vm, uint64_t physical_start, uint64_t length, PageFlags extra)
{
    if(0 == length)
    {
        return true;
    }

    if((physical_start >= kDirectMapPhysicalLimit) ||
       (length > kDirectMapPhysicalLimit - physical_start))
    {
        return false;
    }

    uint64_t current = align_down(physical_start, kPageSize);
    const uint64_t end = align_up(physical_start + length, kPageSize);
    while(current < end)
    {
        const uint64_t virtual_address = phys_to_virt(current);
        const bool use_large_page = (0 == (current % kLargePageSize2MiB)) &&
                                    (0 == (virtual_address % kLargePageSize2MiB)) &&
                                    ((end - current) >= kLargePageSize2MiB);
        // The boot direct map can hold this range with the default attributes. Add the flags and
        // remove the old entry from the TLB of this CPU. No CPU used the range before this call.
        if(extra != PageFlags::None && page_is_already_mapped(vm, virtual_address, current))
        {
            // A large page that is only partly in the range would change memory outside the range.
            const bool large = large_page_is_already_mapped(vm, virtual_address, current);
            if((large && !use_large_page) || !vm.add_leaf_flags(virtual_address, extra))
            {
                return false;
            }
#if !defined(OS1_HOST_TEST)
            invlpg(reinterpret_cast<void*>(virtual_address));
#endif
            current += large ? kLargePageSize2MiB : kPageSize;
            continue;
        }
        if(use_large_page && (large_page_is_already_mapped(vm, virtual_address, current) ||
                              vm.map_physical_2m(virtual_address,
                                                 current,
                                                 1,
                                                 PageFlags::Present | PageFlags::Write |
                                                     PageFlags::NoExecute | extra)))
        {
            current += kLargePageSize2MiB;
            continue;
        }

        if(!page_is_already_mapped(vm, virtual_address, current) &&
           !vm.map_physical(virtual_address,
                            current,
                            1,
                            PageFlags::Present | PageFlags::Write | PageFlags::NoExecute | extra))
        {
            return false;
        }
        current += kPageSize;
    }

    return true;
}
}  // namespace

bool map_mmio_range(VirtualMemory& vm, uint64_t physical_start, uint64_t length)
{
    return map_direct_range(vm, physical_start, length);
}
