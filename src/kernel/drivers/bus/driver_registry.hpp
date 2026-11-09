// Static PCI driver registry for early bus probing.
#pragma once

#include <stddef.h>

#include "mm/page_frame.hpp"
#include "platform/types.hpp"

class VirtualMemory;

constexpr size_t kMaxPciDrivers = 16;

constexpr uint8_t kPciMatchVendorId = 1u << 0;
constexpr uint8_t kPciMatchDeviceId = 1u << 1;
constexpr uint8_t kPciMatchClassCode = 1u << 2;
constexpr uint8_t kPciMatchSubclass = 1u << 3;
constexpr uint8_t kPciMatchProgIf = 1u << 4;

struct PciMatch
{
    uint16_t vendor_id = 0;
    uint16_t device_id = 0;
    uint8_t class_code = 0;
    uint8_t subclass = 0;
    uint8_t prog_if = 0;
    uint8_t match_flags = 0;
};

struct PciDriver
{
    const char* name = nullptr;
    const PciMatch* matches = nullptr;
    size_t match_count = 0;
    bool (*probe)(VirtualMemory& kernel_vm,
                  PageFrameContainer& frames,
                  const PciDevice& device,
                  size_t device_index,
                  DeviceId id) = nullptr;
    void (*remove)(DeviceId id) = nullptr;
    bool (*suspend)(DeviceId id) = nullptr;
    bool (*resume)(DeviceId id) = nullptr;
    bool probe_early = false;  // Finish shared-controller routing before ordinary probes.
};

void driver_registry_reset();
bool driver_registry_add_pci_driver(const PciDriver& driver);
size_t pci_driver_count();
const PciDriver* pci_driver_at(size_t index);
bool pci_driver_matches_device(const PciDriver& driver, const PciDevice& device);
