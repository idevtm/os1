// IRQ route ownership registry. This keeps vector-to-device relationships
// explicit before the wider driver core exists.
#include "platform/irq_registry.hpp"

#include "arch/x86_64/interrupt/interrupt.hpp"
#include "arch/x86_64/interrupt/vector_allocator.hpp"
#include "debug/debug.hpp"
#include "mm/kmem.hpp"
#include "platform/state.hpp"
#include "sync/smp.hpp"

namespace
{
struct IrqRouteNode
{
    IrqRoute route{};
    IrqRouteNode* next = nullptr;
};

struct IrqRouteRegistry
{
    Spinlock lock{"irq-route-registry"};
    KmemCache* cache = nullptr;
    IrqRouteNode* head = nullptr;
    IrqRouteNode* tail = nullptr;
    size_t count = 0;
};

OS1_BSP_ONLY IrqRouteRegistry g_irq_route_registry{};

using IrqRouteGuard = IrqSpinGuard<Spinlock>;

[[nodiscard]] bool device_id_equal(DeviceId left, DeviceId right)
{
    return (left.bus == right.bus) && (left.index == right.index);
}

[[nodiscard]] bool route_kind_uses_dynamic_vector(IrqRouteKind kind)
{
    switch(kind)
    {
        case IrqRouteKind::LocalApic:
        case IrqRouteKind::Msi:
        case IrqRouteKind::Msix:
            return true;
        case IrqRouteKind::LegacyIsa:
            return false;
    }
    return false;
}

[[nodiscard]] bool ensure_irq_route_cache()
{
    if(nullptr != g_irq_route_registry.cache)
    {
        return true;
    }

    g_irq_route_registry.cache =
        kmem_cache_create("irq_route", sizeof(IrqRouteNode), alignof(IrqRouteNode));
    return nullptr != g_irq_route_registry.cache;
}

IrqRouteNode* find_route_locked(uint8_t vector, IrqRouteNode** previous = nullptr)
{
    IrqRouteNode* prior = nullptr;
    for(IrqRouteNode* node = g_irq_route_registry.head; nullptr != node; node = node->next)
    {
        if(node->route.vector == vector)
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

void unlink_route_locked(IrqRouteNode* node, IrqRouteNode* previous)
{
    if(nullptr == previous)
    {
        g_irq_route_registry.head = node->next;
    }
    else
    {
        previous->next = node->next;
    }

    if(g_irq_route_registry.tail == node)
    {
        g_irq_route_registry.tail = previous;
    }

    --g_irq_route_registry.count;
}

IrqRoute* reserve_route_slot_locked(uint8_t vector)
{
    if(nullptr != find_route_locked(vector))
    {
        return nullptr;
    }

    if(!ensure_irq_route_cache())
    {
        debug("platform: IRQ route allocation failed")();
        return nullptr;
    }

    auto* node = static_cast<IrqRouteNode*>(
        kmem_cache_alloc(g_irq_route_registry.cache, KmallocFlags::Zero));
    if(nullptr == node)
    {
        debug("platform: IRQ route allocation failed")();
        return nullptr;
    }

    node->route.active = true;
    node->route.vector = vector;
    if(nullptr == g_irq_route_registry.tail)
    {
        g_irq_route_registry.head = node;
    }
    else
    {
        g_irq_route_registry.tail->next = node;
    }
    g_irq_route_registry.tail = node;
    ++g_irq_route_registry.count;
    return &node->route;
}

bool register_route(IrqRouteKind kind,
                    DeviceId owner,
                    uint16_t source_id,
                    uint8_t source_irq,
                    uint32_t gsi,
                    uint16_t flags,
                    uint8_t vector)
{
    if(!interrupt_vector_is_external(vector))
    {
        debug("platform: rejected invalid IRQ vector 0x")(vector, 16, 2)();
        return false;
    }

    IrqRouteGuard guard(g_irq_route_registry.lock);
    IrqRoute* route = reserve_route_slot_locked(vector);
    if(nullptr == route)
    {
        return false;
    }

    route->kind = kind;
    route->owner = owner;
    route->source_id = source_id;
    route->source_irq = source_irq;
    route->gsi = gsi;
    route->flags = flags;
    return true;
}
}  // namespace

void platform_irq_registry_reset()
{
    if(nullptr != g_irq_route_registry.cache)
    {
        for(IrqRouteNode* node = g_irq_route_registry.head; nullptr != node;)
        {
            IrqRouteNode* next = node->next;
            if(route_kind_uses_dynamic_vector(node->route.kind) &&
               irq_vector_is_allocated(node->route.vector))
            {
                (void)irq_free_vector(node->route.vector);
            }
            kmem_cache_free(g_irq_route_registry.cache, node);
            node = next;
        }
        (void)kmem_cache_destroy(g_irq_route_registry.cache);
    }

    g_irq_route_registry.cache = nullptr;
    g_irq_route_registry.head = nullptr;
    g_irq_route_registry.tail = nullptr;
    g_irq_route_registry.count = 0;
}

bool platform_register_isa_irq_route(
    DeviceId owner, uint8_t source_irq, uint32_t gsi, uint16_t flags, uint8_t vector)
{
    return register_route(IrqRouteKind::LegacyIsa, owner, 0, source_irq, gsi, flags, vector);
}

bool platform_register_local_apic_irq_route(DeviceId owner, uint16_t source_id, uint8_t vector)
{
    return register_route(IrqRouteKind::LocalApic, owner, source_id, 0, 0, 0, vector);
}

bool platform_allocate_local_apic_irq_route(DeviceId owner, uint16_t source_id, uint8_t& vector)
{
    vector = 0;
    uint8_t allocated_vector = 0;
    if(!irq_allocate_vector(allocated_vector))
    {
        return false;
    }

    if(!platform_register_local_apic_irq_route(owner, source_id, allocated_vector))
    {
        (void)irq_free_vector(allocated_vector);
        return false;
    }

    vector = allocated_vector;
    return true;
}

bool platform_register_msi_irq_route(DeviceId owner, uint16_t source_id, uint8_t vector)
{
    return register_route(IrqRouteKind::Msi, owner, source_id, 0, 0, 0, vector);
}

bool platform_register_msix_irq_route(DeviceId owner, uint16_t source_id, uint8_t vector)
{
    return register_route(IrqRouteKind::Msix, owner, source_id, 0, 0, 0, vector);
}

bool platform_find_irq_route(uint8_t vector, IrqRoute& route)
{
    IrqRouteGuard guard(g_irq_route_registry.lock);
    IrqRouteNode* node = find_route_locked(vector);
    if(!node)
        return false;
    route = node->route;
    return true;
}

bool platform_release_irq_route(uint8_t vector)
{
    IrqRouteGuard guard(g_irq_route_registry.lock);
    IrqRouteNode* previous = nullptr;
    IrqRouteNode* node = find_route_locked(vector, &previous);
    if(nullptr == node)
    {
        return false;
    }

    if(route_kind_uses_dynamic_vector(node->route.kind) &&
       irq_vector_is_allocated(node->route.vector))
    {
        (void)irq_free_vector(node->route.vector);
    }
    unlink_route_locked(node, previous);
    node->route.active = false;
    kmem_cache_free(g_irq_route_registry.cache, node);
    return true;
}

void platform_release_irq_routes_for_owner(DeviceId owner)
{
    IrqRouteGuard guard(g_irq_route_registry.lock);
    IrqRouteNode* previous = nullptr;
    IrqRouteNode* node = g_irq_route_registry.head;
    while(nullptr != node)
    {
        if(device_id_equal(node->route.owner, owner))
        {
            if(route_kind_uses_dynamic_vector(node->route.kind) &&
               irq_vector_is_allocated(node->route.vector))
            {
                (void)irq_free_vector(node->route.vector);
            }
            IrqRouteNode* next = node->next;
            unlink_route_locked(node, previous);
            node->route.active = false;
            kmem_cache_free(g_irq_route_registry.cache, node);
            node = next;
            continue;
        }

        previous = node;
        node = node->next;
    }
}

size_t platform_irq_route_count()
{
    IrqRouteGuard guard(g_irq_route_registry.lock);
    return g_irq_route_registry.count;
}

bool visit_irq_routes(bool (*visitor)(void*, const IrqRoute&), void* context)
{
    IrqRouteGuard guard(g_irq_route_registry.lock);
    for(auto* node = g_irq_route_registry.head; node; node = node->next)
    {
        if(!visitor(context, node->route))
            return false;
    }
    return true;
}
