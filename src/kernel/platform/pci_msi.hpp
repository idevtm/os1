// MSI/MSI-X helpers for PCI devices.
#pragma once

#include <stdint.h>

#include "arch/x86_64/interrupt/interrupt.hpp"
#include "platform/types.hpp"

class VirtualMemory;

enum class PciInterruptMode : uint8_t
{
    None = 0,
    LegacyIntx = 1,
    Msi = 2,
    Msix = 3,
};

struct PciInterruptHandle
{
    PciInterruptMode mode = PciInterruptMode::None;
    uint8_t vector = 0;
    uint8_t msix_table_bar = 0;
    uint16_t capability_offset = 0;
    uint16_t source_id = 0;
};

void pci_build_msi_message(uint32_t destination_apic_id,
                           uint8_t vector,
                           uint64_t& address,
                           uint32_t& data);

void pci_build_bsp_msi_message(uint8_t vector, uint64_t& address, uint32_t& data);

void pci_msix_write_table_entry(volatile void* table_base,
                                uint16_t entry_index,
                                uint64_t address,
                                uint32_t data,
                                bool masked);

bool pci_enable_best_interrupt(VirtualMemory& kernel_vm,
                               DeviceId owner,
                               const PciDevice& device,
                               uint16_t source_id,
                               InterruptHandler handler,
                               void* handler_data,
                               PciInterruptHandle& handle);

void pci_release_interrupt(const PciDevice& device, PciInterruptHandle& handle);
