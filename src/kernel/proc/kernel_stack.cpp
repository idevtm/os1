#include "proc/kernel_stack.hpp"

#include "arch/x86_64/apic/ipi.hpp"
#include "handoff/memory_layout.h"
#include "mm/virtual_memory.hpp"
#include "sync/smp.hpp"
#include "util/memory.h"

namespace
{
constexpr uint64_t kStackBytes = kKernelThreadStackPages * kPageSize;
constexpr uint64_t kSlotBytes = kStackBytes + kPageSize;
constexpr uint64_t kMaxTrackedBytes = kPageFrameBitmapSizeBytes * 8 * kPageSize;
static_assert(kKernelStackVirtualBase + kMaxTrackedBytes / kStackBytes * kSlotBytes <
              kKernelVirtualBase);

Spinlock g_kernel_stack_lock{"kernel-stacks"};
OS1_BSP_ONLY uint64_t g_kernel_stack_root = 0;

uint64_t stack_bottom(uint64_t physical)
{
    // Two live runs cannot start in the same bucket because each owns kStackBytes.
    // The first virtual page in every slot is permanently unmapped.
    return kKernelStackVirtualBase + physical / kStackBytes * kSlotBytes + kPageSize;
}

void free_stack_frames(PageFrameContainer& frames, uint64_t physical)
{
    for(uint64_t offset = 0; offset < kStackBytes; offset += kPageSize)
    {
        frames.free(physical + offset);
    }
}
}  // namespace

bool initialize_kernel_stacks(PageFrameContainer& frames, uint64_t kernel_root)
{
#if defined(OS1_HOST_TEST)
    // Most host fixtures have no boot page tables. Give their stack mapper a root.
    if(0 == kernel_root)
    {
        if(!frames.allocate(kernel_root))
        {
            return false;
        }
        memset(kernel_physical_pointer<void>(kernel_root), 0, kPageSize);
    }
#else
    (void)frames;
#endif
    g_kernel_stack_root = kernel_root;
    return 0 != kernel_root;
}

bool allocate_kernel_stack(PageFrameContainer& frames, uint64_t& physical_base, uint64_t& top)
{
    uint64_t physical = 0;
    if(!frames.allocate(physical, kKernelThreadStackPages))
    {
        return false;
    }

    memset(kernel_physical_pointer<void>(physical), 0, kStackBytes);
    const uint64_t bottom = stack_bottom(physical);
    VirtualMemory vm(frames, g_kernel_stack_root);
    uint64_t mapped = 0;
    {
        IrqSpinGuard guard(g_kernel_stack_lock);
        while(mapped < kKernelThreadStackPages &&
              vm.map_physical(bottom + mapped * kPageSize,
                              physical + mapped * kPageSize,
                              1,
                              PageFlags::Write | PageFlags::NoExecute))
        {
            ++mapped;
        }
        if(mapped != kKernelThreadStackPages && mapped && !vm.detach(bottom, mapped))
        {
            __builtin_trap();
        }
    }

    if(mapped != kKernelThreadStackPages)
    {
        // This alias never became visible to a thread. Keep the page tables for reuse.
        free_stack_frames(frames, physical);
        return false;
    }

    physical_base = physical;
    top = bottom + kStackBytes;
    return true;
}

void release_kernel_stack(PageFrameContainer& frames, uint64_t physical_base)
{
    if(0 == physical_base)
    {
        return;
    }

    VirtualMemory vm(frames, g_kernel_stack_root);
    {
        IrqSpinGuard guard(g_kernel_stack_lock);
        if(!vm.detach(stack_bottom(physical_base), kKernelThreadStackPages))
        {
            __builtin_trap();
        }
    }

    // User roots share these supervisor mappings. Retire old translations before
    // the page allocator can reuse either the backing or the virtual slot.
    ipi_retire_tlb();
    free_stack_frames(frames, physical_base);
}
