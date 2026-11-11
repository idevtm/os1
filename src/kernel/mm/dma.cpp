// DMA buffer ownership and bookkeeping.
#include "mm/dma.hpp"

#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "platform/state.hpp"
#include "sync/smp.hpp"
#include "util/memory.h"

namespace
{
struct DmaAllocationNode
{
    DmaAllocationRecord record{};
    DmaAllocationNode* next = nullptr;
};

struct DmaAllocationRegistry
{
    Spinlock lock{"dma-allocation-registry"};
    KmemCache* cache = nullptr;
    DmaAllocationNode* head = nullptr;
    DmaAllocationNode* tail = nullptr;
    size_t count = 0;
};

DmaAllocationRegistry g_dma_allocation_registry{};

using DmaAllocationGuard = IrqSpinGuard<Spinlock>;

[[nodiscard]] uint32_t bytes_to_pages(size_t size_bytes)
{
    const size_t pages = size_bytes / kPageSize + (size_bytes % kPageSize != 0);
    return pages <= UINT32_MAX ? static_cast<uint32_t>(pages) : 0;
}

[[nodiscard]] bool ensure_dma_allocation_cache()
{
    if(nullptr != g_dma_allocation_registry.cache)
    {
        return true;
    }

    g_dma_allocation_registry.cache =
        kmem_cache_create("dma_allocation", sizeof(DmaAllocationNode), alignof(DmaAllocationNode));
    return nullptr != g_dma_allocation_registry.cache;
}

DmaAllocationNode* find_allocation_locked(uint64_t physical_base,
                                          DmaAllocationNode** previous = nullptr)
{
    DmaAllocationNode* prior = nullptr;
    for(DmaAllocationNode* node = g_dma_allocation_registry.head; nullptr != node;
        node = node->next)
    {
        if(node->record.physical_base == physical_base)
        {
            if(nullptr != previous)
            {
                *previous = prior;
            }
            return node;
        }
        prior = node;
    }

    if(nullptr != previous)
    {
        *previous = nullptr;
    }
    return nullptr;
}

void unlink_allocation_locked(DmaAllocationNode* node, DmaAllocationNode* previous)
{
    if(nullptr == previous)
    {
        g_dma_allocation_registry.head = node->next;
    }
    else
    {
        previous->next = node->next;
    }

    if(g_dma_allocation_registry.tail == node)
    {
        g_dma_allocation_registry.tail = previous;
    }

    --g_dma_allocation_registry.count;
}

void free_buffer_pages(PageFrameContainer& frames, uint64_t physical_address, uint32_t page_count)
{
    for(uint32_t page = 0; page < page_count; ++page)
    {
        (void)frames.free(physical_address + static_cast<uint64_t>(page) * kPageSize);
    }
}
}  // namespace

void dma_registry_reset()
{
    if(nullptr != g_dma_allocation_registry.cache)
    {
        for(DmaAllocationNode* node = g_dma_allocation_registry.head; nullptr != node;)
        {
            DmaAllocationNode* next = node->next;
            kmem_cache_free(g_dma_allocation_registry.cache, node);
            node = next;
        }
        (void)kmem_cache_destroy(g_dma_allocation_registry.cache);
    }

    g_dma_allocation_registry.cache = nullptr;
    g_dma_allocation_registry.head = nullptr;
    g_dma_allocation_registry.tail = nullptr;
    g_dma_allocation_registry.count = 0;
}

bool dma_allocate_buffer(PageFrameContainer& frames,
                         DeviceId owner,
                         size_t size_bytes,
                         DmaDirection direction,
                         DmaBuffer& buffer)
{
    buffer = {};
    if(0 == size_bytes)
    {
        return false;
    }

    {
        DmaAllocationGuard guard(g_dma_allocation_registry.lock);
        if(!ensure_dma_allocation_cache())
        {
            return false;
        }
    }

    const uint32_t page_count = bytes_to_pages(size_bytes);
    uint64_t physical = 0;
    if((0 == page_count) || !frames.allocate(physical, page_count))
    {
        return false;
    }

    void* virtual_address = kernel_physical_pointer<void>(physical);
    memset(virtual_address, 0, static_cast<size_t>(page_count) * kPageSize);

    {
        DmaAllocationGuard guard(g_dma_allocation_registry.lock);
        auto* node = g_dma_allocation_registry.cache
                         ? static_cast<DmaAllocationNode*>(kmem_cache_alloc(
                               g_dma_allocation_registry.cache, KmallocFlags::Zero))
                         : nullptr;
        if(node)
        {
            node->record.owner = owner;
            node->record.direction = direction;
            node->record.physical_base = physical;
            node->record.size_bytes = size_bytes;
            node->record.page_count = page_count;
            node->record.coherent = true;
            node->record.active = true;
            if(auto* tail = g_dma_allocation_registry.tail)
                tail->next = node;
            else
                g_dma_allocation_registry.head = node;
            g_dma_allocation_registry.tail = node;
            ++g_dma_allocation_registry.count;
            buffer.owner = owner;
            buffer.virtual_address = virtual_address;
            buffer.physical_address = physical;
            buffer.size_bytes = size_bytes;
            buffer.page_count = page_count;
            buffer.direction = direction;
            buffer.coherent = true;
            buffer.active = true;
            return true;
        }
    }

    free_buffer_pages(frames, physical, page_count);
    return false;
}

void dma_release_buffer(PageFrameContainer& frames, DmaBuffer& buffer)
{
    if(!buffer.active)
    {
        return;
    }

    {
        DmaAllocationGuard guard(g_dma_allocation_registry.lock);
        DmaAllocationNode* previous = nullptr;
        DmaAllocationNode* node = find_allocation_locked(buffer.physical_address, &previous);
        if(nullptr != node)
        {
            unlink_allocation_locked(node, previous);
            node->record.active = false;
            kmem_cache_free(g_dma_allocation_registry.cache, node);
        }
    }

    free_buffer_pages(frames, buffer.physical_address, buffer.page_count);
    buffer = {};
}

void dma_sync_for_device(const DmaBuffer& buffer)
{
    (void)buffer;
    asm volatile("" : : : "memory");
}

void dma_sync_for_cpu(const DmaBuffer& buffer)
{
    (void)buffer;
    asm volatile("" : : : "memory");
}

size_t dma_allocation_count()
{
    DmaAllocationGuard guard(g_dma_allocation_registry.lock);
    return g_dma_allocation_registry.count;
}

bool visit_dma_allocations(bool (*visitor)(void*, const DmaAllocationRecord&), void* context)
{
    DmaAllocationGuard guard(g_dma_allocation_registry.lock);
    for(auto* node = g_dma_allocation_registry.head; node; node = node->next)
    {
        if(!visitor(context, node->record))
            return false;
    }
    return true;
}
