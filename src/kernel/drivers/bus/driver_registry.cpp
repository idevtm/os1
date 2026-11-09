// Fixed-size early driver registry.
#include "drivers/bus/driver_registry.hpp"

#include "util/memory.h"

namespace
{
PciDriver g_pci_drivers[kMaxPciDrivers];
size_t g_pci_driver_count = 0;

bool pci_match_matches_device(const PciMatch& match, const PciDevice& device)
{
    if(0 == match.match_flags)
    {
        return false;
    }
    if((0 != (match.match_flags & kPciMatchVendorId)) && (match.vendor_id != device.vendor_id))
    {
        return false;
    }
    if((0 != (match.match_flags & kPciMatchDeviceId)) && (match.device_id != device.device_id))
    {
        return false;
    }
    if((0 != (match.match_flags & kPciMatchClassCode)) && (match.class_code != device.class_code))
    {
        return false;
    }
    if((0 != (match.match_flags & kPciMatchSubclass)) && (match.subclass != device.subclass))
    {
        return false;
    }
    if((0 != (match.match_flags & kPciMatchProgIf)) && (match.prog_if != device.prog_if))
    {
        return false;
    }
    return true;
}
}  // namespace

void driver_registry_reset()
{
    memset(g_pci_drivers, 0, sizeof(g_pci_drivers));
    g_pci_driver_count = 0;
}

bool driver_registry_add_pci_driver(const PciDriver& driver)
{
    if((nullptr == driver.name) || (nullptr == driver.probe) ||
       ((driver.match_count > 0) && (nullptr == driver.matches)) || (g_pci_driver_count >= kMaxPciDrivers))
    {
        return false;
    }
    g_pci_drivers[g_pci_driver_count++] = driver;
    return true;
}

size_t pci_driver_count()
{
    return g_pci_driver_count;
}

const PciDriver* pci_driver_at(size_t index)
{
    return (index < g_pci_driver_count) ? &g_pci_drivers[index] : nullptr;
}

bool pci_driver_matches_device(const PciDriver& driver, const PciDevice& device)
{
    if((nullptr == driver.matches) || (0 == driver.match_count))
    {
        return true;
    }

    for(size_t match_index = 0; match_index < driver.match_count; ++match_index)
    {
        if(pci_match_matches_device(driver.matches[match_index], device))
        {
            return true;
        }
    }
    return false;
}
