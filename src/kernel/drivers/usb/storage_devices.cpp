#include "drivers/usb/storage_devices.hpp"

#include "drivers/usb/ehci.hpp"
#include "drivers/usb/xhci.hpp"
BlockDevice* usb_storage_block_device(size_t index)
{
    for(size_t i = 0;; ++i)
    {
        auto* device = xhci_storage_block_device(i);
        if(!device)
            return ehci_storage_block_device(index);
        if(!index)
            return device;
        --index;
    }
}
