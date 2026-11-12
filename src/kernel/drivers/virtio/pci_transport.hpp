// Shared virtio PCI transport definitions.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "platform/pci_msi.hpp"
#include "platform/types.hpp"

class VirtualMemory;
struct PciBarResource;

struct [[gnu::packed]] VirtioPciCapability
{
    uint8_t cap_vndr;
    uint8_t cap_next;
    uint8_t cap_len;
    uint8_t cfg_type;
    uint8_t bar;
    uint8_t id;
    uint8_t padding[2];
    uint32_t offset;
    uint32_t length;
};

struct [[gnu::packed]] VirtioPciCommonCfg
{
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t device_status;
    uint8_t config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
};

struct VirtioPciTransport
{
    DeviceId owner{DeviceBus::Pci, 0};
    const PciDevice* device = nullptr;
    volatile VirtioPciCommonCfg* common_cfg = nullptr;
    volatile uint8_t* device_cfg = nullptr;
    volatile uint8_t* isr_status = nullptr;
    uint64_t notify_base = 0;
    uint32_t notify_length = 0;
    uint32_t notify_multiplier = 0;
    PciInterruptHandle interrupt{};
};

constexpr uint16_t kVirtioNoVector = 0xFFFF;
constexpr uint32_t kVirtioStatusAcknowledge = 1u << 0;
constexpr uint32_t kVirtioStatusDriver = 1u << 1;
constexpr uint32_t kVirtioStatusDriverOk = 1u << 2;
constexpr uint32_t kVirtioStatusFeaturesOk = 1u << 3;
constexpr uint64_t kVirtioFeatureVersion1 = 1ull << 32;
constexpr uint8_t kVirtioPciCapCommonCfg = 1;
constexpr uint8_t kVirtioPciCapNotifyCfg = 2;
constexpr uint8_t kVirtioPciCapIsrCfg = 3;
constexpr uint8_t kVirtioPciCapDeviceCfg = 4;

void virtio_pci_transport_reset(VirtioPciTransport& transport);
bool virtio_pci_bind_transport(VirtualMemory& kernel_vm,
                               DeviceId owner,
                               const PciDevice& device,
                               size_t device_cfg_size,
                               VirtioPciTransport& transport);
void virtio_pci_write_device_status(VirtioPciTransport& transport, uint8_t status);
bool virtio_pci_negotiate_features(VirtioPciTransport& transport,
                                   uint64_t required_features,
                                   uint64_t accepted_features);
volatile uint16_t* virtio_pci_setup_queue(VirtioPciTransport& transport,
                                          uint16_t queue_index,
                                          uint16_t queue_size,
                                          uint64_t desc_physical,
                                          uint64_t avail_physical,
                                          uint64_t used_physical);
bool virtio_pci_bind_queue_interrupt(VirtualMemory& kernel_vm,
                                     VirtioPciTransport& transport,
                                     uint16_t queue_index,
                                     uint16_t source_id,
                                     InterruptHandler handler,
                                     void* handler_data);
void virtio_pci_notify_queue(volatile uint16_t* notify_register, uint16_t queue_index);
