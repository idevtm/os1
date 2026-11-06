// Current platform device-probe loop for built-in PCI drivers.
#include "platform/device_probe.hpp"

#include "debug/debug.hpp"
#include "drivers/block/virtio_blk.hpp"
#include "drivers/bus/driver_registry.hpp"
#include "drivers/bus/pci_bus.hpp"
#include "drivers/net/virtio_net.hpp"
#include "drivers/usb/ehci.hpp"
#include "drivers/usb/xhci.hpp"
#include "mm/page_frame.hpp"
#include "mm/virtual_memory.hpp"
#include "platform/state.hpp"
#include "storage/block_device.hpp"

extern PageFrameContainer page_frames;

bool platform_probe_devices(VirtualMemory& kernel_vm, bool enable_xhci)
{
    platform_reset_driver_state();
    driver_registry_reset();
    if(!enable_xhci)
    {
        debug("xhci: disabled by boot option")();
    }
    if(!driver_registry_add_pci_driver(virtio_blk_pci_driver()) ||
       !driver_registry_add_pci_driver(virtio_net_pci_driver()) ||
       (enable_xhci && !driver_registry_add_pci_driver(xhci_pci_driver())) ||
       !driver_registry_add_pci_driver(ehci_pci_driver()) ||
       !pci_bus_probe_all(kernel_vm, page_frames))
    {
        return false;
    }
    if(!g_platform.virtio_blk_public.present)
    {
        debug("virtio-blk: no device present")();
    }
    g_platform.block_device = virtio_blk_block_device();
    return run_virtio_blk_smoke() && run_virtio_net_smoke();
}
