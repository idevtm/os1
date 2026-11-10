// PCI bus probing through the static driver registry.
#pragma once

#include "mm/page_frame.hpp"
#include "platform/types.hpp"

class VirtualMemory;
struct PciDriver;
bool pci_bus_probe_driver(VirtualMemory&, PageFrameContainer&, const PciDriver&);

bool pci_bus_probe_all(VirtualMemory& kernel_vm, PageFrameContainer& frames);
bool pci_bus_remove_device(DeviceId id);
