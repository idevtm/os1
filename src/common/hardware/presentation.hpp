#pragma once
#include <stdint.h>

namespace os1::hardware::presentation
{
template<class Sink>
void number(Sink& sink, uint64_t value, unsigned base = 10, unsigned width = 0)
{
    char text[65]{};
    unsigned at = 64;
    do
    {
        text[--at] = "0123456789abcdef"[value % base];
        value /= base;
    } while(value);
    while(at && 64 - at < width)
        text[--at] = '0';
    sink.text(text + at);
}
template<class Sink>
void capacity(Sink& sink, uint64_t value)
{
    const char* units[] = {" B", " KiB", " MiB", " GiB", " TiB", " PiB", " EiB"};
    unsigned index = 0;
    uint64_t scale = 1;
    while(index < 6 && value / scale >= 1024)
    {
        scale *= 1024;
        ++index;
    }
    number(sink, value / scale);
    if(index && value % scale)
    {
        sink.text(".");
        // Bound the intermediate even for UINT64_MAX.
        number(sink, ((value % scale) / (scale / 1024)) * 100 / 1024, 10, 2);
    }
    sink.text(units[index]);
}
template<class Sink>
void usb_version(Sink& sink, uint64_t value)
{
    if(value > 0xffff || (value & 15) > 9 || ((value >> 4) & 15) > 9 || ((value >> 8) & 15) > 9 ||
       ((value >> 12) & 15) > 9)
    {
        sink.text("invalid_bcd(0x");
        number(sink, value, 16, 4);
        sink.text(")");
        return;
    }
    number(sink, (value >> 12) * 10 + ((value >> 8) & 15));
    sink.text(".");
    number(sink, ((value >> 4) & 15) * 10 + (value & 15), 10, 2);
}
template<class Sink>
void speed(Sink& sink, uint64_t value)
{
    number(sink, value / 10);
    if(value % 10)
    {
        sink.text(".");
        number(sink, value % 10);
    }
    sink.text(" Mbit/s");
}
inline const char* pci_class(unsigned code, unsigned subclass)
{
    switch(code)
    {
        case 1:
            return "Storage controller";
        case 2:
            return "Network controller";
        case 3:
            return "Display controller";
        case 4:
            return "Multimedia controller";
        case 6:
            return "Bridge";
        case 12:
            return subclass == 3 ? "USB controller" : "Serial bus controller";
        default:
            return "PCI function";
    }
}
inline const char* usb_class(unsigned code)
{
    switch(code)
    {
        case 1:
            return "Audio";
        case 3:
            return "HID";
        case 8:
            return "Mass storage";
        case 9:
            return "Hub";
        case 14:
            return "Video";
        case 224:
            return "Wireless";
        case 255:
            return "Vendor-specific";
        default:
            return "USB function";
    }
}
}  // namespace os1::hardware::presentation
