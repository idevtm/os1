// Shared virtio PCI transport helpers will grow here as more virtio devices
// land. The first live user is still virtio-blk.
#include "drivers/virtio/pci_transport.hpp"

#include "drivers/bus/resource.hpp"
#include "handoff/memory_layout.h"
#include "mm/boot_mapping.hpp"
#include "platform/pci_config.hpp"
#include "util/memory.h"

namespace
{
struct CapabilityRegion
{
    uint8_t bar = 0xff;
    uint32_t offset = 0;
    uint32_t length = 0;
};

bool region_fits(const CapabilityRegion& region, const PciBarResource& resource)
{
    return uint64_t(region.offset) + region.length <= resource.size;
}

[[nodiscard]] bool map_claimed_bar(VirtualMemory& kernel_vm,
                                   DeviceId owner,
                                   uint16_t pci_index,
                                   uint8_t bar_index,
                                   PciBarResource& resource)
{
    if(!claim_pci_bar(owner, pci_index, bar_index, resource))
    {
        return false;
    }
    if((PciBarType::Mmio32 != resource.type) && (PciBarType::Mmio64 != resource.type))
    {
        return false;
    }
    return map_mmio_range(kernel_vm, resource.base, resource.size);
}
}  // namespace

void virtio_pci_transport_reset(VirtioPciTransport& transport)
{
    memset(&transport, 0, sizeof(transport));
}

void virtio_pci_write_device_status(VirtioPciTransport& transport, uint8_t status)
{
    if(nullptr != transport.common_cfg)
    {
        transport.common_cfg->device_status = status;
    }
}

bool virtio_pci_bind_transport(VirtualMemory& kernel_vm,
                               DeviceId owner,
                               const PciDevice& device,
                               size_t device_cfg_size,
                               VirtioPciTransport& transport)
{
    virtio_pci_transport_reset(transport);

    CapabilityRegion common{}, notify{}, device_config{}, isr{};
    uint32_t notify_multiplier = 0;

    uint8_t capability = device.capability_pointer;
    for(size_t guard = 0; capability != 0 && guard < 48; ++guard)
    {
        if(capability < 0x40u || capability > 0xfcu || capability % 4u)
            return false;
        const uint8_t cap_id = pci_config_read8(device, capability);
        const uint8_t cap_next = pci_config_read8(device, capability + 1u);
        if(cap_id == 0x09u)
        {
            const uint8_t type = pci_config_read8(device, capability + 3u);
            if(type >= kVirtioPciCapCommonCfg && type <= kVirtioPciCapDeviceCfg)
            {
                const uint8_t cap_length = pci_config_read8(device, capability + 2u);
                if(cap_length <= offsetof(VirtioPciCapability, bar) ||
                   cap_length > 256u - capability)
                    return false;
                const uint8_t bar = pci_config_read8(device, capability + 4u);
                if(bar >= 6u)
                {
                    capability = cap_next;
                    continue;
                }
                const size_t required = sizeof(VirtioPciCapability) +
                                        (type == kVirtioPciCapNotifyCfg ? sizeof(uint32_t) : 0);
                if(cap_length < required)
                    return false;
                const CapabilityRegion region{bar,
                                              pci_config_read32(device, capability + 8u),
                                              pci_config_read32(device, capability + 12u)};
                switch(type)
                {
                    case kVirtioPciCapCommonCfg:
                        common = region;
                        break;
                    case kVirtioPciCapNotifyCfg:
                        notify = region;
                        notify_multiplier = pci_config_read32(device, capability + 16u);
                        break;
                    case kVirtioPciCapDeviceCfg:
                        device_config = region;
                        break;
                    case kVirtioPciCapIsrCfg:
                        isr = region;
                        break;
                }
            }
        }
        capability = cap_next;
    }

    if(common.bar == 0xff || notify.bar == 0xff || device_config.bar == 0xff ||
       common.length < sizeof(VirtioPciCommonCfg) || notify.length < sizeof(uint16_t) ||
       device_config.length < device_cfg_size || (isr.bar != 0xff && isr.length == 0))
        return false;

    PciBarResource common_resource{}, notify_resource{}, device_resource{}, isr_resource{};
    if(!map_claimed_bar(kernel_vm, owner, owner.index, common.bar, common_resource) ||
       !map_claimed_bar(kernel_vm, owner, owner.index, notify.bar, notify_resource) ||
       !map_claimed_bar(kernel_vm, owner, owner.index, device_config.bar, device_resource) ||
       (isr.bar != 0xff && !map_claimed_bar(kernel_vm, owner, owner.index, isr.bar, isr_resource)))
        return false;

    if(!region_fits(common, common_resource) || !region_fits(notify, notify_resource) ||
       !region_fits(device_config, device_resource) ||
       (isr.bar != 0xff && !region_fits(isr, isr_resource)))
        return false;

    pci_enable_mmio_bus_mastering(device);
    transport.owner = owner;
    transport.device = &device;
    transport.common_cfg =
        kernel_physical_pointer<volatile VirtioPciCommonCfg>(common_resource.base + common.offset);
    transport.device_cfg =
        kernel_physical_pointer<volatile uint8_t>(device_resource.base + device_config.offset);
    transport.notify_base = notify_resource.base + notify.offset;
    transport.notify_length = notify.length;
    transport.notify_multiplier = notify_multiplier;
    if(isr.bar != 0xff)
        transport.isr_status =
            kernel_physical_pointer<volatile uint8_t>(isr_resource.base + isr.offset);
    return true;
}

bool virtio_pci_negotiate_features(VirtioPciTransport& transport,
                                   uint64_t required_features,
                                   uint64_t accepted_features)
{
    if(nullptr == transport.common_cfg)
    {
        return false;
    }

    transport.common_cfg->device_feature_select = 0;
    const uint64_t device_features_low = transport.common_cfg->device_feature;
    transport.common_cfg->device_feature_select = 1;
    const uint64_t device_features_high = transport.common_cfg->device_feature;
    const uint64_t device_features = device_features_low | (device_features_high << 32);
    if((device_features & required_features) != required_features)
    {
        return false;
    }

    const uint64_t negotiated_features = device_features & accepted_features;
    transport.common_cfg->driver_feature_select = 0;
    transport.common_cfg->driver_feature = static_cast<uint32_t>(negotiated_features);
    transport.common_cfg->driver_feature_select = 1;
    transport.common_cfg->driver_feature = static_cast<uint32_t>(negotiated_features >> 32);
    virtio_pci_write_device_status(
        transport,
        static_cast<uint8_t>(transport.common_cfg->device_status | kVirtioStatusFeaturesOk));
    return 0 != (transport.common_cfg->device_status & kVirtioStatusFeaturesOk);
}

volatile uint16_t* virtio_pci_setup_queue(VirtioPciTransport& transport,
                                          uint16_t queue_index,
                                          uint16_t queue_size,
                                          uint64_t desc_physical,
                                          uint64_t avail_physical,
                                          uint64_t used_physical)
{
    if(transport.common_cfg == nullptr || queue_size == 0)
        return nullptr;

    transport.common_cfg->queue_select = queue_index;
    const uint16_t device_queue_size = transport.common_cfg->queue_size;
    if(device_queue_size < 3u || queue_size > device_queue_size)
        return nullptr;

    const uint64_t offset =
        uint64_t(transport.common_cfg->queue_notify_off) * transport.notify_multiplier;
    if(offset + sizeof(uint16_t) > transport.notify_length)
        return nullptr;
    const uint64_t address = transport.notify_base + offset;
    if(address % alignof(uint16_t))
        return nullptr;
    auto* notify_register = kernel_physical_pointer<volatile uint16_t>(address);
    if(!notify_register)
        return nullptr;

    transport.common_cfg->queue_size = queue_size;
    transport.common_cfg->queue_desc = desc_physical;
    transport.common_cfg->queue_driver = avail_physical;
    transport.common_cfg->queue_device = used_physical;
    transport.common_cfg->queue_enable = 1;
    return notify_register;
}

bool virtio_pci_bind_queue_interrupt(VirtualMemory& kernel_vm,
                                     VirtioPciTransport& transport,
                                     uint16_t queue_index,
                                     uint16_t source_id,
                                     InterruptHandler handler,
                                     void* handler_data)
{
    if((nullptr == transport.common_cfg) || (nullptr == transport.device))
    {
        return false;
    }

    if(!pci_enable_best_interrupt(
           kernel_vm, transport.owner, *transport.device, source_id, handler, handler_data, transport.interrupt))
    {
        transport.common_cfg->msix_config = kVirtioNoVector;
        transport.common_cfg->queue_select = queue_index;
        transport.common_cfg->queue_msix_vector = kVirtioNoVector;
        return false;
    }

    transport.common_cfg->msix_config = kVirtioNoVector;
    transport.common_cfg->queue_select = queue_index;
    if(PciInterruptMode::Msix == transport.interrupt.mode)
    {
        transport.common_cfg->queue_msix_vector = 0;
        if(0 != transport.common_cfg->queue_msix_vector)
        {
            return false;
        }
    }
    else
    {
        transport.common_cfg->queue_msix_vector = kVirtioNoVector;
    }

    return true;
}

void virtio_pci_notify_queue(volatile uint16_t* notify_register, uint16_t queue_index)
{
    if(notify_register)
        *notify_register = queue_index;
}
