// PCI MSI/MSI-X programming and INTx fallback.
#include "platform/pci_msi.hpp"

#include "platform/acpi_aml.hpp"
#include "platform/pci_capability.hpp"
#include "platform/pci_config.hpp"

#if !defined(OS1_HOST_TEST)
#include "arch/x86_64/interrupt/vector_allocator.hpp"
#include "core/kernel_state.hpp"
#include "drivers/bus/resource.hpp"
#include "handoff/memory_layout.h"
#include "mm/boot_mapping.hpp"
#include "platform/irq_registry.hpp"
#include "platform/platform.hpp"
#include "platform/state.hpp"
#include "util/align.hpp"
#endif

namespace
{
constexpr uint16_t kPciMsiEnable = 1u << 0;
constexpr uint16_t kPciMsiMultipleMessageEnableMask = 0x7u << 4;
constexpr uint16_t kPciMsixFunctionMask = 1u << 14;
constexpr uint16_t kPciMsixEnable = 1u << 15;
constexpr size_t kPciMsixTableEntrySize = 16u;
}  // namespace

void pci_build_msi_message(uint32_t destination_apic_id,
                           uint8_t vector,
                           uint64_t& address,
                           uint32_t& data)
{
    address = 0xFEE00000ull | (static_cast<uint64_t>(destination_apic_id) << 12);
    data = vector;
}

void pci_msix_write_table_entry(volatile void* table_base,
                                uint16_t entry_index,
                                uint64_t address,
                                uint32_t data,
                                bool masked)
{
    auto* entry = static_cast<volatile uint32_t*>(table_base) +
                  (static_cast<size_t>(entry_index) * (kPciMsixTableEntrySize / sizeof(uint32_t)));
    entry[0] = static_cast<uint32_t>(address & 0xFFFFFFFFull);
    entry[1] = static_cast<uint32_t>(address >> 32);
    entry[2] = data;
    entry[3] = masked ? 1u : 0u;
}

#if !defined(OS1_HOST_TEST)
namespace
{
[[nodiscard]] uint32_t bsp_apic_id()
{
    return (g_platform.cpu_count > 0) ? g_platform.cpus[0].apic_id : 0u;
}

void clear_vector_handler(uint8_t vector)
{
    interrupts.set_vector_handler(vector, nullptr, nullptr);
}

bool register_dynamic_interrupt_route(IrqRouteKind kind,
                                      DeviceId owner,
                                      uint16_t source_id,
                                      uint8_t vector)
{
    switch(kind)
    {
        case IrqRouteKind::Msi:
            return platform_register_msi_irq_route(owner, source_id, vector);
        case IrqRouteKind::Msix:
            return platform_register_msix_irq_route(owner, source_id, vector);
        case IrqRouteKind::LegacyIsa:
        case IrqRouteKind::LocalApic:
            return false;
    }
    return false;
}

bool allocate_handler_vector(InterruptHandler handler, void* handler_data, uint8_t& vector)
{
    if(!irq_allocate_vector(vector))
    {
        return false;
    }
    interrupts.set_vector_handler(vector, handler, handler_data);
    return true;
}

bool pci_enable_msix_interrupt(VirtualMemory& kernel_vm,
                               DeviceId owner,
                               const PciDevice& device,
                               uint16_t source_id,
                               InterruptHandler handler,
                               void* handler_data,
                               PciInterruptHandle& handle)
{
    PciMsixCapabilityInfo msix{};
    if(!pci_parse_msix_capability(device, msix) || (0 == msix.table_size))
    {
        return false;
    }

    PciBarResource table_resource{};
    if(!claim_pci_bar(owner, owner.index, msix.table_bar, table_resource))
    {
        return false;
    }
    if((PciBarType::Mmio32 != table_resource.type) && (PciBarType::Mmio64 != table_resource.type))
    {
        return false;
    }
    const uint64_t table_size_bytes =
        static_cast<uint64_t>(msix.table_size) * kPciMsixTableEntrySize;
    if((msix.table_offset > table_resource.size) ||
       (table_size_bytes > (table_resource.size - msix.table_offset)))
    {
        return false;
    }

    const uint64_t table_physical = table_resource.base + msix.table_offset;
    const uint64_t table_map_start = align_down(table_physical, kPageSize);
    const uint64_t table_map_end = align_up(table_physical + table_size_bytes, kPageSize);
    if(!map_mmio_range(kernel_vm, table_map_start, table_map_end - table_map_start))
    {
        return false;
    }

    uint8_t vector = 0;
    if(!allocate_handler_vector(handler, handler_data, vector))
    {
        return false;
    }

    uint64_t address = 0;
    uint32_t data = 0;
    pci_build_msi_message(bsp_apic_id(), vector, address, data);

    auto* table = kernel_physical_pointer<volatile void>(table_physical);
    uint16_t control = pci_config_read16(device, static_cast<uint16_t>(msix.offset + 2u));
    pci_config_write16(device,
                       static_cast<uint16_t>(msix.offset + 2u),
                       static_cast<uint16_t>(control | kPciMsixFunctionMask));
    pci_msix_write_table_entry(table, 0, address, data, true);
    pci_msix_write_table_entry(table, 0, address, data, false);
    control = static_cast<uint16_t>((control | kPciMsixEnable) & ~kPciMsixFunctionMask);
    pci_config_write16(device, static_cast<uint16_t>(msix.offset + 2u), control);
    pci_disable_intx(device, true);

    if(!register_dynamic_interrupt_route(IrqRouteKind::Msix, owner, source_id, vector))
    {
        pci_config_write16(device,
                           static_cast<uint16_t>(msix.offset + 2u),
                           static_cast<uint16_t>((control | kPciMsixFunctionMask) & ~kPciMsixEnable));
        clear_vector_handler(vector);
        (void)irq_free_vector(vector);
        return false;
    }

    handle.mode = PciInterruptMode::Msix;
    handle.vector = vector;
    handle.capability_offset = msix.offset;
    handle.msix_table_bar = msix.table_bar;
    handle.source_id = source_id;
    return true;
}

bool pci_enable_msi_interrupt(DeviceId owner,
                              const PciDevice& device,
                              uint16_t source_id,
                              InterruptHandler handler,
                              void* handler_data,
                              PciInterruptHandle& handle)
{
    PciMsiCapabilityInfo msi{};
    if(!pci_parse_msi_capability(device, msi))
    {
        return false;
    }

    uint8_t vector = 0;
    if(!allocate_handler_vector(handler, handler_data, vector))
    {
        return false;
    }

    uint64_t address = 0;
    uint32_t data = 0;
    pci_build_msi_message(bsp_apic_id(), vector, address, data);

    pci_config_write32(device, static_cast<uint16_t>(msi.offset + 4u), static_cast<uint32_t>(address));
    uint16_t data_offset = static_cast<uint16_t>(msi.offset + 8u);
    if(msi.is_64_bit)
    {
        pci_config_write32(device,
                           static_cast<uint16_t>(msi.offset + 8u),
                           static_cast<uint32_t>(address >> 32));
        data_offset = static_cast<uint16_t>(msi.offset + 12u);
    }
    pci_config_write16(device, data_offset, static_cast<uint16_t>(data));

    uint16_t control = static_cast<uint16_t>(msi.control & ~kPciMsiMultipleMessageEnableMask);
    control = static_cast<uint16_t>(control | kPciMsiEnable);
    pci_config_write16(device, static_cast<uint16_t>(msi.offset + 2u), control);
    pci_disable_intx(device, true);

    if(!register_dynamic_interrupt_route(IrqRouteKind::Msi, owner, source_id, vector))
    {
        pci_config_write16(device,
                           static_cast<uint16_t>(msi.offset + 2u),
                           static_cast<uint16_t>(control & ~kPciMsiEnable));
        clear_vector_handler(vector);
        (void)irq_free_vector(vector);
        return false;
    }

    handle.mode = PciInterruptMode::Msi;
    handle.vector = vector;
    handle.capability_offset = msi.offset;
    handle.source_id = source_id;
    return true;
}

bool pci_enable_intx_interrupt(DeviceId owner,
                               const PciDevice& device,
                               uint16_t source_id,
                               InterruptHandler handler,
                               void* handler_data,
                               PciInterruptHandle& handle)
{
    if(0 == device.interrupt_pin)
    {
        return false;
    }

    uint32_t route_irq = 0;
    uint16_t route_flags = 0;
    const bool have_acpi_route =
        acpi_resolve_pci_route(device.bus,
                               device.slot,
                               device.function,
                               static_cast<uint8_t>(device.interrupt_pin - 1u),
                               route_irq,
                               route_flags);
    if(!have_acpi_route && ((0xFFu == device.interrupt_line) || (device.interrupt_line > 15u)))
    {
        return false;
    }

    uint8_t vector = 0;
    if(!allocate_handler_vector(handler, handler_data, vector))
    {
        return false;
    }
    pci_disable_intx(device, false);
    const bool routed = have_acpi_route
                            ? platform_route_gsi_irq(owner, route_irq, route_flags, vector)
                            : platform_route_isa_irq(owner, device.interrupt_line, vector);
    if(!routed)
    {
        clear_vector_handler(vector);
        (void)irq_free_vector(vector);
        return false;
    }

    handle.mode = PciInterruptMode::LegacyIntx;
    handle.vector = vector;
    handle.source_id = source_id;
    return true;
}
}  // namespace
#endif

bool pci_enable_best_interrupt(VirtualMemory& kernel_vm,
                               DeviceId owner,
                               const PciDevice& device,
                               uint16_t source_id,
                               InterruptHandler handler,
                               void* handler_data,
                               PciInterruptHandle& handle)
{
    handle = {};
#if defined(OS1_HOST_TEST)
    (void)kernel_vm;
    (void)owner;
    (void)device;
    (void)source_id;
    (void)handler;
    (void)handler_data;
    return false;
#else
    return pci_enable_msix_interrupt(
               kernel_vm, owner, device, source_id, handler, handler_data, handle) ||
           pci_enable_msi_interrupt(owner, device, source_id, handler, handler_data, handle) ||
           pci_enable_intx_interrupt(owner, device, source_id, handler, handler_data, handle);
#endif
}

void pci_release_interrupt(const PciDevice& device, PciInterruptHandle& handle)
{
#if defined(OS1_HOST_TEST)
    (void)device;
#else
    switch(handle.mode)
    {
        case PciInterruptMode::Msix:
        {
            uint16_t control = pci_config_read16(device, static_cast<uint16_t>(handle.capability_offset + 2u));
            control = static_cast<uint16_t>((control | kPciMsixFunctionMask) & ~kPciMsixEnable);
            pci_config_write16(device, static_cast<uint16_t>(handle.capability_offset + 2u), control);
            break;
        }
        case PciInterruptMode::Msi:
        {
            uint16_t control = pci_config_read16(device, static_cast<uint16_t>(handle.capability_offset + 2u));
            control = static_cast<uint16_t>(control & ~kPciMsiEnable);
            pci_config_write16(device, static_cast<uint16_t>(handle.capability_offset + 2u), control);
            break;
        }
        case PciInterruptMode::LegacyIntx:
        case PciInterruptMode::None:
            break;
    }

    if(0 != handle.vector)
    {
        clear_vector_handler(handle.vector);
        (void)platform_release_irq_route(handle.vector);
    }
#endif
    handle = {};
}
