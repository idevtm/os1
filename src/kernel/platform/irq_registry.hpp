// IRQ route ownership records shared by legacy IOAPIC routes and dynamic MSI
// or MSI-X vectors.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "platform/types.hpp"

// Register one ISA/IOAPIC-backed route owned by a platform or PCI device.
bool platform_register_isa_irq_route(
    DeviceId owner, uint8_t source_irq, uint32_t gsi, uint16_t flags, uint8_t vector);

// Register one local APIC vector record owned by a platform device.
bool platform_register_local_apic_irq_route(DeviceId owner, uint16_t source_id, uint8_t vector);

// Allocate one dynamic vector and register it as a local APIC route.
bool platform_allocate_local_apic_irq_route(DeviceId owner, uint16_t source_id, uint8_t& vector);

// Register one MSI-backed vector record owned by a PCI device.
bool platform_register_msi_irq_route(DeviceId owner, uint16_t source_id, uint8_t vector);

// Register one MSI-X-backed vector record owned by a PCI device.
bool platform_register_msix_irq_route(DeviceId owner, uint16_t source_id, uint8_t vector);

// Copy the active route for one vector. Return false when unowned.
bool platform_find_irq_route(uint8_t vector, IrqRoute& route);

// Release one registered route and free its dynamic vector when needed.
bool platform_release_irq_route(uint8_t vector);

// Release every route owned by the supplied device.
void platform_release_irq_routes_for_owner(DeviceId owner);
