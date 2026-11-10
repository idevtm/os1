// PCI bus probing through the fixed driver registry.
#include "drivers/bus/pci_bus.hpp"

#include "debug/debug.hpp"
#include "drivers/bus/device.hpp"
#include "drivers/bus/driver_registry.hpp"
#include "freestanding/string.hpp"
#include "platform/hardware.hpp"
#include "platform/platform.hpp"
#include "platform/types.hpp"

namespace
{
bool probe_one(VirtualMemory& vm, PageFrameContainer& frames, const PciDriver& driver, size_t index)
{
    DeviceId id{DeviceBus::Pci, static_cast<uint16_t>(index)};
    auto admission = device_binding_begin_probe(id, static_cast<uint16_t>(index), driver.name);
    if(admission == BindingAdmission::Existing)
        return false;
    if(admission == BindingAdmission::NoMemory)
    {
        hardware_record_probe_failure(index, driver.name);
        debug("pci: ")(driver.name)(" binding allocation failed")();
        return false;
    }
    bool success = driver.probe(vm, frames, platform_pci_devices()[index], index, id);
    (void)device_binding_finish_probe(id, success);
    if(!success)
    {
        hardware_record_probe_failure(index, driver.name);
        debug("pci: ")(driver.name)(" probe failed; resources require driver retirement")();
    }
    return success;
}
}  // namespace
bool pci_bus_probe_driver(VirtualMemory& vm, PageFrameContainer& frames, const PciDriver& driver)
{
    bool success = true;
    for(size_t i = 0; i < platform_pci_device_count(); ++i)
        if(pci_driver_matches_device(driver, platform_pci_devices()[i]))
            success = probe_one(vm, frames, driver, i) && success;
    return success;
}

bool pci_bus_probe_all(VirtualMemory& kernel_vm, PageFrameContainer& frames)
{
    hardware_reset_bindings();
    const PciDevice* devices = platform_pci_devices();
    const size_t device_count = platform_pci_device_count();
    // Early drivers complete before ordinary devices, regardless of PCI BDF order.
    for(unsigned phase = 0; phase < 2u; ++phase)
    {
        for(size_t device_index = 0; device_index < device_count; ++device_index)
        {
            for(size_t driver_index = 0; driver_index < pci_driver_count(); ++driver_index)
            {
                const PciDriver* driver = pci_driver_at(driver_index);
                if((nullptr == driver) ||
                   !pci_driver_matches_device(*driver, devices[device_index]))
                {
                    continue;
                }

                if(driver->probe_early != (phase == 0u))
                    break;  // The first matching driver retains ownership in either phase.
                (void)probe_one(kernel_vm, frames, *driver, device_index);
                break;
            }
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
                if(!driver->quiesce || !device_binding_begin_remove(id))
                    return false;
                bool retired = driver->quiesce(id);
                if(!device_binding_finish_remove(id, retired) || !retired)
                    return false;
                driver->remove(id);
                device_binding_remove(id);
                return true;
            }
        }
    }
    return false;
}
