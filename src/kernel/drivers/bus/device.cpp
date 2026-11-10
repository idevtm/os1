// Bound device records stored in platform state.
#include "drivers/bus/device.hpp"

#include "driver/lifecycle.hpp"
#include "freestanding/string.hpp"
#include "mm/kmem.hpp"
#include "platform/state.hpp"
#include "sync/smp.hpp"
#include "util/memory.h"

namespace
{
struct DeviceBindingNode
{
    DeviceBinding binding{};
    driver::Lifecycle lifecycle{};
    DeviceBindingNode* next = nullptr;
};

struct DeviceBindingRegistry
{
    Spinlock lock{"device-binding-registry"};
    KmemCache* cache = nullptr;
    DeviceBindingNode* head = nullptr;
    DeviceBindingNode* tail = nullptr;
    DeviceBinding* snapshot = nullptr;
    size_t snapshot_capacity = 0;
    size_t count = 0;
};

OS1_BSP_ONLY DeviceBindingRegistry g_device_binding_registry{};

using DeviceBindingGuard = IrqSpinGuard<Spinlock>;

[[nodiscard]] bool device_id_equal(DeviceId left, DeviceId right)
{
    return (left.bus == right.bus) && (left.index == right.index);
}

[[nodiscard]] bool ensure_device_binding_cache()
{
    if(nullptr != g_device_binding_registry.cache)
    {
        return true;
    }

    g_device_binding_registry.cache =
        kmem_cache_create("device_binding", sizeof(DeviceBindingNode), alignof(DeviceBindingNode));
    return nullptr != g_device_binding_registry.cache;
}

[[nodiscard]] bool ensure_snapshot_capacity(size_t required_capacity)
{
    if(required_capacity <= g_device_binding_registry.snapshot_capacity)
    {
        return true;
    }

    size_t new_capacity = (0u == g_device_binding_registry.snapshot_capacity)
                              ? 4u
                              : g_device_binding_registry.snapshot_capacity;
    while(new_capacity < required_capacity)
    {
        new_capacity *= 2u;
    }

    auto* new_snapshot = static_cast<DeviceBinding*>(kcalloc(new_capacity, sizeof(DeviceBinding)));
    if(nullptr == new_snapshot)
    {
        return false;
    }

    kfree(g_device_binding_registry.snapshot);
    g_device_binding_registry.snapshot = new_snapshot;
    g_device_binding_registry.snapshot_capacity = new_capacity;
    return true;
}

DeviceBindingNode* find_binding_node_locked(DeviceId id, DeviceBindingNode** previous = nullptr)
{
    DeviceBindingNode* prior = nullptr;
    for(DeviceBindingNode* node = g_device_binding_registry.head; nullptr != node;
        node = node->next)
    {
        if(device_id_equal(node->binding.id, id))
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

void unlink_binding_node_locked(DeviceBindingNode* node, DeviceBindingNode* previous)
{
    if(nullptr == previous)
    {
        g_device_binding_registry.head = node->next;
    }
    else
    {
        previous->next = node->next;
    }

    if(g_device_binding_registry.tail == node)
    {
        g_device_binding_registry.tail = previous;
    }

    --g_device_binding_registry.count;
}
}  // namespace

void device_binding_registry_reset()
{
    if(nullptr != g_device_binding_registry.cache)
    {
        for(DeviceBindingNode* node = g_device_binding_registry.head; nullptr != node;)
        {
            DeviceBindingNode* next = node->next;
            kmem_cache_free(g_device_binding_registry.cache, node);
            node = next;
        }
        (void)kmem_cache_destroy(g_device_binding_registry.cache);
    }

    g_device_binding_registry.cache = nullptr;
    g_device_binding_registry.head = nullptr;
    g_device_binding_registry.tail = nullptr;
    g_device_binding_registry.count = 0;
    kfree(g_device_binding_registry.snapshot);
    g_device_binding_registry.snapshot = nullptr;
    g_device_binding_registry.snapshot_capacity = 0;
}

static BindingAdmission publish_binding(
    DeviceId id, uint16_t pci_index, const char* driver_name, void* driver_state, bool probing)
{
    DeviceBindingGuard guard(g_device_binding_registry.lock);
    if(DeviceBindingNode* existing = find_binding_node_locked(id))
    {
        if(probing || !freestanding::strings_equal(existing->binding.driver_name, driver_name) ||
           (existing->lifecycle.state() != driver::State::starting &&
            existing->lifecycle.state() != driver::State::ready))
            return BindingAdmission::Existing;
        existing->binding.active = true;
        existing->binding.pci_index = pci_index;
        existing->binding.driver_name = driver_name;
        existing->binding.driver_state = driver_state;
        existing->binding.state = DeviceState::Bound;
        return BindingAdmission::Published;
    }

    if(!ensure_snapshot_capacity(g_device_binding_registry.count + 1u) ||
       !ensure_device_binding_cache())
    {
        return BindingAdmission::NoMemory;
    }

    auto* node = static_cast<DeviceBindingNode*>(
        kmem_cache_alloc(g_device_binding_registry.cache, KmallocFlags::Zero));
    if(nullptr == node)
    {
        return BindingAdmission::NoMemory;
    }

    node->lifecycle = driver::Lifecycle{};
    (void)node->lifecycle.start();
    if(!probing)
        (void)node->lifecycle.started(true);
    node->binding.active = true;
    node->binding.id = id;
    node->binding.state = probing ? DeviceState::Probing : DeviceState::Bound;
    node->binding.pci_index = pci_index;
    node->binding.driver_name = driver_name;
    node->binding.driver_state = driver_state;

    if(nullptr == g_device_binding_registry.tail)
    {
        g_device_binding_registry.head = node;
    }
    else
    {
        g_device_binding_registry.tail->next = node;
    }
    g_device_binding_registry.tail = node;
    ++g_device_binding_registry.count;
    return BindingAdmission::Published;
}

bool device_binding_publish(DeviceId id, uint16_t pci_index, const char* name, void* state)
{
    return publish_binding(id, pci_index, name, state, false) == BindingAdmission::Published;
}
BindingAdmission device_binding_begin_probe(DeviceId id, uint16_t pci_index, const char* name)
{
    return publish_binding(id, pci_index, name, nullptr, true);
}

bool device_binding_set_state(DeviceId id, DeviceState state)
{
    DeviceBindingGuard guard(g_device_binding_registry.lock);
    DeviceBindingNode* node = find_binding_node_locked(id);
    if(nullptr == node)
    {
        return false;
    }
    node->binding.state = state;
    return true;
}

void device_binding_remove(DeviceId id)
{
    DeviceBindingGuard guard(g_device_binding_registry.lock);
    DeviceBindingNode* previous = nullptr;
    DeviceBindingNode* node = find_binding_node_locked(id, &previous);
    if(nullptr != node)
    {
        unlink_binding_node_locked(node, previous);
        node->binding.active = false;
        node->binding.state = DeviceState::Removed;
        kmem_cache_free(g_device_binding_registry.cache, node);
    }
}

DeviceBinding* device_binding_find(DeviceId id)
{
    DeviceBindingGuard guard(g_device_binding_registry.lock);
    DeviceBindingNode* node = find_binding_node_locked(id);
    return (nullptr == node) ? nullptr : &node->binding;
}

size_t device_binding_count()
{
    DeviceBindingGuard guard(g_device_binding_registry.lock);
    return g_device_binding_registry.count;
}

const DeviceBinding* device_bindings()
{
    DeviceBindingGuard guard(g_device_binding_registry.lock);
    if(0u == g_device_binding_registry.count)
    {
        return nullptr;
    }

    size_t index = 0;
    for(DeviceBindingNode* node = g_device_binding_registry.head; nullptr != node;
        node = node->next)
    {
        g_device_binding_registry.snapshot[index++] = node->binding;
    }
    return g_device_binding_registry.snapshot;
}

bool device_binding_finish_probe(DeviceId id, bool success)
{
    DeviceBindingGuard guard(g_device_binding_registry.lock);
    auto* node = find_binding_node_locked(id);
    if(!node || !node->lifecycle.started(success))
        return false;
    node->binding.state = success ? DeviceState::Started : DeviceState::Failed;
    return true;
}
bool device_binding_begin_remove(DeviceId id)
{
    DeviceBindingGuard guard(g_device_binding_registry.lock);
    auto* node = find_binding_node_locked(id);
    if(!node || !node->lifecycle.stop())
        return false;
    node->binding.state = DeviceState::Stopping;
    return true;
}
bool device_binding_finish_remove(DeviceId id, bool retired)
{
    DeviceBindingGuard guard(g_device_binding_registry.lock);
    auto* node = find_binding_node_locked(id);
    if(!node || !node->lifecycle.stopped(retired))
        return false;
    node->binding.state = retired ? DeviceState::Removed : DeviceState::Failed;
    return true;
}
