// Public platform facade. The implementation is split by responsibility under
// platform/, but callers should use this narrow API for bring-up, IRQ routing,
// and read-only device/topology observation.
#pragma once

#include <stddef.h>

#include "platform/acpi_aml.hpp"
#include "platform/types.hpp"

class VirtualMemory;
struct BlockDevice;
struct BootInfo;

// Discover machine topology, map interrupt controllers, and enumerate PCI. This
// stage does not activate device drivers yet.
bool platform_discover(const BootInfo& boot_info, VirtualMemory& kernel_vm);

// Probe supported devices from the discovered PCI table and publish driver
// facades once the generic interrupt machinery is online.
// `enable_xhci` = false leaves xHCI controllers unbound (boot option os1.noxhci).
bool platform_probe_devices(VirtualMemory& kernel_vm, bool enable_xhci = true);

// Route an ISA IRQ through the discovered IOAPIC override table onto one IDT
// vector chosen by the caller.
bool platform_route_isa_irq(DeviceId owner, int bus_irq, uint8_t vector);

// Route one exact GSI onto one IDT vector chosen by the caller.
bool platform_route_gsi_irq(DeviceId owner, uint32_t gsi, uint16_t flags, uint8_t vector);

// Return the currently selected generic block device, if any.
const BlockDevice* platform_block_device();

// Return the discovered HPET record when one was parsed and initialized.
const HpetInfo* platform_hpet();

// Read the current HPET main-counter value when HPET is available.
bool platform_hpet_read_main_counter(uint64_t& counter_value);

// Sample a validated high-resolution clock; false means unavailable.
// This clock has a separate epoch from the IPC deadline clock.
bool platform_measurement_sample(uint64_t& nanoseconds, uint64_t& resolution_ns);

// Return the public virtio-blk summary when a virtio block device is present.
const VirtioBlkDevice* platform_virtio_blk();

// Return the number of PCI functions found during ECAM enumeration.
size_t platform_pci_device_count();

// Return the fixed PCI device table owned by platform state.
const PciDevice* platform_pci_devices();

// Return the number of active IRQ resource records.
size_t platform_irq_route_count();

// Return the fixed IRQ route table owned by platform state.
const IrqRoute* platform_irq_routes();

// Return the number of ACPI namespace devices parsed from DSDT/SSDT AML.
size_t platform_acpi_device_count();

// Return the ACPI device table owned by platform state.
const AcpiDeviceInfo* platform_acpi_devices();

// Read boot-discovered routes only. A cache miss never evaluates AML.
const AcpiPciRoute* platform_cached_acpi_pci_route(uint8_t bus,
                                                   uint8_t slot,
                                                   uint8_t function,
                                                   uint8_t pin);

// BSP boot only, after PCI enumeration and before user execution.
// Resolve missing enumerated-device routes into the bounded observation cache.
void platform_cache_enumerated_pci_routes();

// Suspend active bound devices in reverse bind order and place ACPI companions in D3.
bool platform_suspend_devices();

// Resume active bound devices in bind order and place ACPI companions in D0.
bool platform_resume_devices();
