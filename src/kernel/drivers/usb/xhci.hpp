// xHCI PCI driver entry points.
#pragma once

#include "drivers/bus/driver_registry.hpp"

const PciDriver& xhci_pci_driver();
struct BlockDevice;
BlockDevice* xhci_storage_block_device(size_t index);

// Called on the BSP timer; dispatches input collected by IRQ or AP polling.
void xhci_poll_input();

// Boot-thread summary; uses the existing diagnostic screen/log sinks.
void xhci_diagnostic_summary();
