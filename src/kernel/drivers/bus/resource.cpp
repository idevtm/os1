// PCI BAR ownership records shared by bus and driver code.
#include "drivers/bus/resource.hpp"

#include "debug/debug.hpp"
#include "mm/kmem.hpp"
#include "platform/state.hpp"
#include "sync/smp.hpp"

namespace
{
struct PciBarClaimNode
{
    PciBarClaim claim{};
    PciBarClaimNode* next = nullptr;
};

struct PciBarClaimRegistry
{
    Spinlock lock{"pci-bar-claim-registry"};
    KmemCache* cache = nullptr;
    PciBarClaimNode* head = nullptr;
    PciBarClaimNode* tail = nullptr;
    size_t count = 0;
};

OS1_BSP_ONLY PciBarClaimRegistry g_pci_bar_claim_registry{};

using PciBarClaimGuard = IrqSpinGuard<Spinlock>;

[[nodiscard]] bool device_id_equal(DeviceId left, DeviceId right)
{
    return (left.bus == right.bus) && (left.index == right.index);
}

[[nodiscard]] bool ensure_pci_bar_claim_cache()
{
    if(nullptr != g_pci_bar_claim_registry.cache)
    {
        return true;
    }

    g_pci_bar_claim_registry.cache =
        kmem_cache_create("pci_bar_claim", sizeof(PciBarClaimNode), alignof(PciBarClaimNode));
    return nullptr != g_pci_bar_claim_registry.cache;
}

PciBarClaimNode* find_claim_locked(uint16_t pci_index,
                                   uint8_t bar_index,
                                   PciBarClaimNode** previous = nullptr)
{
    PciBarClaimNode* prior = nullptr;
    for(PciBarClaimNode* node = g_pci_bar_claim_registry.head; nullptr != node; node = node->next)
    {
        if((node->claim.pci_index == pci_index) && (node->claim.bar_index == bar_index))
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

void unlink_claim_node_locked(PciBarClaimNode* node, PciBarClaimNode* previous)
{
    if(nullptr == previous)
    {
        g_pci_bar_claim_registry.head = node->next;
    }
    else
    {
        previous->next = node->next;
    }

    if(g_pci_bar_claim_registry.tail == node)
    {
        g_pci_bar_claim_registry.tail = previous;
    }

    --g_pci_bar_claim_registry.count;
}
}  // namespace

void pci_bar_claim_registry_reset()
{
    if(nullptr != g_pci_bar_claim_registry.cache)
    {
        for(PciBarClaimNode* node = g_pci_bar_claim_registry.head; nullptr != node;)
        {
            PciBarClaimNode* next = node->next;
            kmem_cache_free(g_pci_bar_claim_registry.cache, node);
            node = next;
        }
        (void)kmem_cache_destroy(g_pci_bar_claim_registry.cache);
    }

    g_pci_bar_claim_registry.cache = nullptr;
    g_pci_bar_claim_registry.head = nullptr;
    g_pci_bar_claim_registry.tail = nullptr;
    g_pci_bar_claim_registry.count = 0;
}

bool claim_pci_bar(DeviceId owner, uint16_t pci_index, uint8_t bar_index, PciBarResource& resource)
{
    resource = {};
    if((pci_index >= g_platform.device_count) || (bar_index >= 6u))
    {
        return false;
    }

    const PciDevice& device = g_platform.devices[pci_index];
    const PciBarInfo& bar = device.bars[bar_index];
    if((0 == bar.base) || (0 == bar.size) || (PciBarType::Unused == bar.type))
    {
        return false;
    }

    PciBarClaimGuard guard(g_pci_bar_claim_registry.lock);
    if(PciBarClaimNode* existing = find_claim_locked(pci_index, bar_index))
    {
        if(!device_id_equal(existing->claim.owner, owner))
        {
            debug("pci: BAR already claimed pci=")(pci_index)(" bar=")(bar_index)();
            return false;
        }

        resource.pci_index = pci_index;
        resource.bar_index = bar_index;
        resource.type = existing->claim.type;
        resource.base = existing->claim.base;
        resource.size = existing->claim.size;
        return true;
    }

    if(!ensure_pci_bar_claim_cache())
    {
        debug("pci: BAR claim allocation failed")();
        return false;
    }

    auto* node = static_cast<PciBarClaimNode*>(
        kmem_cache_alloc(g_pci_bar_claim_registry.cache, KmallocFlags::Zero));
    if(nullptr == node)
    {
        debug("pci: BAR claim allocation failed")();
        return false;
    }

    node->claim.active = true;
    node->claim.owner = owner;
    node->claim.pci_index = pci_index;
    node->claim.bar_index = bar_index;
    node->claim.type = bar.type;
    node->claim.base = bar.base;
    node->claim.size = bar.size;

    if(nullptr == g_pci_bar_claim_registry.tail)
    {
        g_pci_bar_claim_registry.head = node;
    }
    else
    {
        g_pci_bar_claim_registry.tail->next = node;
    }
    g_pci_bar_claim_registry.tail = node;
    ++g_pci_bar_claim_registry.count;

    resource.pci_index = pci_index;
    resource.bar_index = bar_index;
    resource.type = bar.type;
    resource.base = bar.base;
    resource.size = bar.size;
    return true;
}

void release_pci_bar(DeviceId owner, uint16_t pci_index, uint8_t bar_index)
{
    PciBarClaimGuard guard(g_pci_bar_claim_registry.lock);
    PciBarClaimNode* previous = nullptr;
    PciBarClaimNode* node = find_claim_locked(pci_index, bar_index, &previous);
    if((nullptr != node) && device_id_equal(node->claim.owner, owner))
    {
        unlink_claim_node_locked(node, previous);
        node->claim.active = false;
        kmem_cache_free(g_pci_bar_claim_registry.cache, node);
    }
}

void release_pci_bars_for_owner(DeviceId owner)
{
    PciBarClaimGuard guard(g_pci_bar_claim_registry.lock);
    PciBarClaimNode* previous = nullptr;
    PciBarClaimNode* node = g_pci_bar_claim_registry.head;
    while(nullptr != node)
    {
        if(device_id_equal(node->claim.owner, owner))
        {
            PciBarClaimNode* next = node->next;
            unlink_claim_node_locked(node, previous);
            node->claim.active = false;
            kmem_cache_free(g_pci_bar_claim_registry.cache, node);
            node = next;
            continue;
        }

        previous = node;
        node = node->next;
    }
}

size_t pci_bar_claim_count()
{
    PciBarClaimGuard guard(g_pci_bar_claim_registry.lock);
    return g_pci_bar_claim_registry.count;
}

bool visit_pci_bar_claims(bool (*visitor)(void*, const PciBarClaim&), void* context)
{
    PciBarClaimGuard guard(g_pci_bar_claim_registry.lock);
    for(auto* node = g_pci_bar_claim_registry.head; node; node = node->next)
    {
        if(!visitor(context, node->claim))
            return false;
    }
    return true;
}
