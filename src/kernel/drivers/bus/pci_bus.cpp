// PCI bus probing through the fixed driver registry.
#include "drivers/bus/pci_bus.hpp"

#include "debug/debug.hpp"
#include "drivers/bus/device.hpp"
#include "drivers/bus/driver_registry.hpp"
#include "freestanding/string.hpp"
#include "platform/platform.hpp"
#include "platform/types.hpp"

bool pci_bus_probe_all(VirtualMemory& kernel_vm, PageFrameContainer& frames)
{
    const PciDevice* devices = platform_pci_devices();
    const size_t device_count = platform_pci_device_count();
    for(size_t device_index = 0; device_index < device_count; ++device_index)
    {
        DeviceId id{DeviceBus::Pci, static_cast<uint16_t>(device_index)};
        (void)device_binding_set_state(id, DeviceState::Probing);
        for(size_t driver_index = 0; driver_index < pci_driver_count(); ++driver_index)
        {
            const PciDriver* driver = pci_driver_at(driver_index);
            if((nullptr == driver) || !pci_driver_matches_device(*driver, devices[device_index]))
            {
                continue;
            }

            if(driver->probe(kernel_vm, frames, devices[device_index], device_index, id))
            {
                break;
            }

            // One misbehaving device must not stop bring-up: real machines carry
            // controllers that emulator-tested drivers have never seen.
            debug("pci: ")(driver->name)(" probe failed for ")(devices[device_index].bus, 16, 2)(":")(
                devices[device_index].slot, 16, 2)(".")(devices[device_index].function, 16, 1)(
                "; continuing without it")();
            (void)device_binding_set_state(id, DeviceState::Failed);
            break;
        }
    }
    return true;
}

bool pci_bus_remove_device(DeviceId id)
{
    if(const DeviceBinding* binding = device_binding_find(id))
    {
        for(size_t driver_index = 0; driver_index < pci_driver_count(); ++driver_index)
        {
            const PciDriver* driver = pci_driver_at(driver_index);
            if((nullptr != driver) && (nullptr != driver->remove) &&
               freestanding::strings_equal(binding->driver_name, driver->name))
            {
                driver->remove(id);
                device_binding_remove(id);
                return true;
            }
        }
    }
    return false;
}
