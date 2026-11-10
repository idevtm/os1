#pragma once
#include "hardware/presentation.hpp"
#include "hardware/tree.hpp"

namespace os1::hardware::names
{
inline const char* vendor(unsigned id, bool usb)
{
    if(usb)
        switch(id)
        {
            case 0x8087:
                return "Intel";
            case 0x0951:
                return "Kingston";
            case 0x17ef:
                return "Lenovo";
            case 0x0bda:
                return "Realtek";
            case 0x0cf3:
                return "Qualcomm Atheros";
            case 0x13d3:
                return "IMC Networks";
            case 0x046d:
                return "Logitech";
            default:
                return "USB";
        }
    switch(id)
    {
        case 0x8086:
            return "Intel";
        case 0x10de:
            return "NVIDIA";
        case 0x10ec:
            return "Realtek";
        case 0x168c:
            return "Qualcomm Atheros";
        case 0x1b73:
            return "Fresco Logic";
        case 0x1af4:
            return "Virtio";
        case 0x1234:
            return "QEMU";
        case 0x1022:
            return "AMD";
        case 0x1002:
            return "AMD/ATI";
        default:
            return "PCI";
    }
}
inline void describe(char (&out)[96],
                     unsigned vendor_id,
                     unsigned device_id,
                     unsigned class_code,
                     unsigned subclass,
                     bool usb)
{
    // Small descriptive hints. Numeric identifiers remain authoritative.
    const char* model = nullptr;
    if(usb)
    {
        if(vendor_id == 0x0951 && device_id == 0x1666)
            model = "Kingston DataTraveler 3.0";
        if(vendor_id == 0x17ef && device_id == 0x6190)
            model = "Lenovo Calliope USB Keyboard G2";
        if(vendor_id == 0x8087 && device_id == 0x0024)
            model = "Intel integrated rate-matching hub";
        if(vendor_id == 0x0bda && device_id == 0x0139)
            model = "Realtek RTS5139 card reader";
        if(vendor_id == 0x13d3 && device_id == 0x5134)
            model = "IMC Networks integrated webcam";
        if(vendor_id == 0x0cf3 && device_id == 0x3005)
            model = "Qualcomm Atheros Bluetooth adapter";
    }
    if(model)
    {
        copy(out, sizeof(out), model);
        return;
    }
    copy(out, sizeof(out), vendor(vendor_id, usb));
    unsigned n = 0;
    while(out[n])
        ++n;
    out[n++] = ' ';
    copy(out + n,
         sizeof(out) - n,
         usb ? presentation::usb_class(class_code) : presentation::pci_class(class_code, subclass));
}
}  // namespace os1::hardware::names
