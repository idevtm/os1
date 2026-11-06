#pragma once
#include "drivers/bus/driver_registry.hpp"
struct BlockDevice;
const PciDriver& ehci_pci_driver();
BlockDevice* ehci_storage_block_device(size_t index);
