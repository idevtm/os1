// Platform discovery sequence: ACPI discovery, topology publication, and PCI
// enumeration. Driver probing is deferred until interrupts are online.
#include "arch/x86_64/apic/ioapic.hpp"
#include "arch/x86_64/apic/lapic.hpp"
#include "debug/debug.hpp"
#include "handoff/boot_info.hpp"
#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "mm/boot_mapping.hpp"
#include "mm/virtual_memory.hpp"
#include "platform/acpi.hpp"
#include "platform/acpi_aml.hpp"
#include "platform/acpica_integration.hpp"
#include "platform/hpet.hpp"
#include "platform/pci.hpp"
#include "platform/platform.hpp"
#include "platform/state.hpp"
#include "platform/topology.hpp"
#include "sync/smp.hpp"
#include "util/memory.h"

bool platform_discover(const BootInfo& boot_info, VirtualMemory& kernel_vm)
{
    KASSERT_ON_BSP();
    if(lapic_x2apic_active())
    {
        debug("platform: x2APIC mode active; xAPIC MMIO required")();
        return false;
    }
    platform_reset_state();

    if(!acpica_initialize_tables(kernel_vm, boot_info))
    {
        debug("platform: ACPICA table init failed status=")(acpica_last_status())();
        return false;
    }

    const bool acpi_available = discover_acpi_platform(kernel_vm,
                                                       boot_info,
                                                       g_platform.lapic_base,
                                                       g_platform.cpus,
                                                       g_platform.cpu_count,
                                                       g_platform.ioapics,
                                                       g_platform.ioapic_count,
                                                       g_platform.overrides,
                                                       g_platform.override_count,
                                                       g_platform.ecam_regions,
                                                       g_platform.ecam_region_count,
                                                       g_platform.hpet,
                                                       g_platform.acpi_fixed,
                                                       g_platform.acpi_definition_blocks,
                                                       g_platform.acpi_definition_block_count);
    if(!acpi_available)
    {
        (void)kernel_vm;
        debug("platform: ACPI required; legacy MP discovery removed")();
        return false;
    }

    debug("acpi: namespace load start")();
    if(!acpi_namespace_load(kernel_vm,
                            g_platform.acpi_definition_blocks,
                            g_platform.acpi_definition_block_count))
    {
        debug("acpi: namespace load failed err=")(acpi_namespace_last_error())(" obj=")(
            acpi_namespace_last_object())();
        return false;
    }

    debug("acpi: namespace load ready")();
    debug("acpi: device info build start")();
    if(!acpica_count_device_objects(g_platform.acpi_device_capacity))
    {
        debug("acpi: device count failed err=")(acpi_namespace_last_error())(" obj=")(
            acpi_namespace_last_object())();
        return false;
    }

    if(0u != g_platform.acpi_device_capacity)
    {
        g_platform.acpi_devices = static_cast<AcpiDeviceInfo*>(
            kcalloc(g_platform.acpi_device_capacity, sizeof(AcpiDeviceInfo)));
        if(nullptr == g_platform.acpi_devices)
        {
            debug("acpi: device table alloc failed count=")(g_platform.acpi_device_capacity)();
            g_platform.acpi_device_capacity = 0;
            return false;
        }
    }

    if(!acpica_build_device_info_with_capacity(g_platform.acpi_devices,
                                               g_platform.acpi_device_capacity,
                                               g_platform.acpi_device_count,
                                               g_platform.acpi_pci_routes,
                                               g_platform.acpi_pci_route_count))
    {
        kfree(g_platform.acpi_devices);
        g_platform.acpi_devices = nullptr;
        g_platform.acpi_device_capacity = 0;
        debug("acpi: device info build failed err=")(acpi_namespace_last_error())(" obj=")(
            acpi_namespace_last_object())();
        return false;
    }
    debug("acpi: device info build ready devices=")(g_platform.acpi_device_count)(" routes=")(
        g_platform.acpi_pci_route_count)();

    // Some legacy BIOS machines advertise HPET in ACPI but wedge as soon as
    // the kernel maps or probes the MMIO block. Keep discovery for visibility,
    // but disable runtime HPET use on the BIOS boot path and fall back to PIT.
    if((BootSource::BiosLegacy == boot_info.source) && g_platform.hpet.present)
    {
        debug("hpet: disabled on bios boot path")();
        g_platform.hpet = {};
    }

    debug("platform: lapic mmio map")();
    if(!map_mmio_range(kernel_vm, g_platform.lapic_base, kPageSize))
    {
        return false;
    }
    for(size_t i = 0; i < g_platform.ioapic_count; ++i)
    {
        debug("platform: ioapic mmio map addr=0x")(g_platform.ioapics[i].address, 16)();
        if(!map_mmio_range(kernel_vm, g_platform.ioapics[i].address, kPageSize))
        {
            return false;
        }
    }
    if(g_platform.hpet.present && !map_mmio_range(kernel_vm, g_platform.hpet.physical_address, kPageSize))
    {
        return false;
    }
    debug("platform: hpet init")();
    if(!platform_hpet_initialize())
    {
        return false;
    }
    debug("platform: cpu topology publish")();
    if(!allocate_cpus_from_topology())
    {
        return false;
    }

    g_platform.acpi_active = true;
    debug("platform: pci enumerate")();
    if(!enumerate_pci(kernel_vm,
                      g_platform.ecam_regions,
                      g_platform.ecam_region_count,
                      g_platform.devices,
                      g_platform.device_count))
    {
        return false;
    }

    platform_cache_enumerated_pci_routes();
    debug("acpi: enumerated route cache ready routes=")(g_platform.acpi_pci_route_count)();
    g_platform.initialized = true;
    return true;
}
