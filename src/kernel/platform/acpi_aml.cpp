// ACPI AML facade. ACPICA owns namespace loading, device discovery, routing,
// and power hooks.
#include "platform/acpi_aml.hpp"

#include "platform/acpica_integration.hpp"
#include "platform/platform.hpp"
#include "platform/state.hpp"

bool acpi_namespace_load(VirtualMemory& kernel_vm,
                         const AcpiDefinitionBlock* definition_blocks,
                         size_t definition_block_count)
{
    (void)kernel_vm;
    (void)definition_blocks;
    (void)definition_block_count;
    return acpica_load_namespace();
}

const char* acpi_namespace_last_error()
{
    return acpica_namespace_last_error();
}

const char* acpi_namespace_last_object()
{
    return acpica_namespace_last_object();
}

bool acpi_build_device_info(AcpiDeviceInfo* devices,
                            size_t& device_count,
                            AcpiPciRoute* routes,
                            size_t& route_count)
{
    return acpica_build_device_info(devices, device_count, routes, route_count);
}

bool acpi_resolve_pci_route(uint8_t bus,
                            uint8_t slot,
                            uint8_t function,
                            uint8_t pin,
                            uint32_t& irq,
                            uint16_t& flags)
{
    bool source_is_gsi = false;
    return acpi_resolve_pci_route_details(bus, slot, function, pin, irq, flags, source_is_gsi);
}

bool acpi_resolve_pci_route_details(uint8_t bus,
                                    uint8_t slot,
                                    uint8_t function,
                                    uint8_t pin,
                                    uint32_t& irq,
                                    uint16_t& flags,
                                    bool& source_is_gsi)
{
    return acpica_resolve_pci_route_details(bus, slot, function, pin, irq, flags, source_is_gsi);
}

bool acpi_device_supports_power_state(const char* path, AcpiPowerState state)
{
    return acpica_device_supports_power_state(path, state);
}

bool acpi_set_device_power_state(const char* path, AcpiPowerState state)
{
    return acpica_set_device_power_state(path, state);
}

bool acpi_read_named_integer(const char* path, uint64_t& value)
{
    return acpica_read_named_integer(path, value);
}

void platform_cache_enumerated_pci_routes()
{
    for(size_t index = 0; index < g_platform.device_count; ++index)
    {
        if(g_platform.acpi_pci_route_count >= kAcpiMaxPciRoutes)
        {
            break;
        }
        const PciDevice& device = g_platform.devices[index];
        if((device.interrupt_pin == 0) || (device.interrupt_pin > 4))
        {
            continue;
        }
        const uint8_t pin = device.interrupt_pin - 1u;
        if(platform_cached_acpi_pci_route(device.bus, device.slot, device.function, pin))
        {
            continue;
        }
        AcpiPciRoute route{};
        if(!acpi_resolve_pci_route_details(device.bus,
                                           device.slot,
                                           device.function,
                                           pin,
                                           route.irq,
                                           route.flags,
                                           route.source_is_gsi))
        {
            continue;
        }
        route.active = true;
        route.bus_number = device.bus;
        route.slot = device.slot;
        route.function = device.function;
        route.pin = pin;
        route.source_device_index = kAcpiDeviceIndexNone;
        g_platform.acpi_pci_routes[g_platform.acpi_pci_route_count++] = route;
    }
}
