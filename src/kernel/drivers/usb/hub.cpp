#include "drivers/usb/hub.hpp"
namespace usb::hub
{
bool parse_descriptor(const uint8_t* p, size_t bytes, Descriptor& out)
{
    out = {};
    if(!p || bytes < 9 || p[1] != 0x29 || !p[2] || p[2] > 15)
        return false;
    size_t size = 7 + 2 * ((unsigned(p[2]) + 1 + 7) / 8);
    if(p[0] != size || bytes < size)
        return false;
    out = {p[2], uint16_t(2u * p[5] + 20)};
    return true;
}
bool configuration(const uint8_t* p, size_t bytes, uint8_t& value)
{
    value = 0;
    if(!p || bytes < 9 || p[0] != 9 || p[1] != 2 || !p[5] ||
       (unsigned(p[2]) | (unsigned(p[3]) << 8)) != bytes)
        return false;
    bool found = false;
    for(size_t at = 9; at < bytes; at += p[at])
    {
        if(bytes - at < 2 || p[at] < 2 || p[at] > bytes - at)
            return false;
        if(p[at + 1] == 4)
        {
            if(p[at] < 9)
                return false;
            if(p[at + 3] == 0 && p[at + 5] == 9)
                found = true;
        }
    }
    if(found)
        value = p[5];
    return found;
}
bool port_status(const Transport& t, uint8_t port, uint32_t& status)
{
    uint8_t bytes[4]{};
    if(!t.control(t.context, 0xa3, 0, 0, port, bytes, sizeof(bytes)))
        return false;
    status = uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 |
             uint32_t(bytes[3]) << 24;
    return true;
}
bool stable_high_speed(uint32_t status)
{
    return (status & 0x503u) == 0x503u && !(status & (0x1fu << 16 | 0x21cu));
}
unsigned unsupported_speed(uint32_t status)
{
    // Connected, enabled, powered, and stable; neither high-speed nor faulted.
    if((status & 0x103u) != 0x103u || (status & (0x1fu << 16 | 0x41cu)))
        return 0;
    return status & 0x200u ? 15 : 120;
}
bool enumerate_port(const Transport& t, uint8_t port, bool debounce)
{
    uint32_t status = 0;
    if(!port_status(t, port, status))
        return false;
    if(!(status & 1) || (!debounce && (status & (1u << 16))))
        return true;
    if(debounce)
    {
        t.delay_ms(t.context, 100);
        if(!port_status(t, port, status))
            return false;
    }
    if(!(status & 1) || (status & 8))
        return true;
    if(!t.control(t.context, 0x23, 3, 4, port, nullptr, 0))
        return false;
    bool ready = false;
    for(unsigned attempt = 0; attempt < 50; ++attempt)
    {
        t.delay_ms(t.context, 10);
        if(!port_status(t, port, status))
            return false;
        if(!(status & 1))
            break;
        if(!(status & 16) && (status & 2))
        {
            ready = true;
            break;
        }
    }
    // A late reset completion could expose another device at address zero.
    // Abort the tree; the controller caller must isolate it before continuing.
    if(!ready)
        return false;
    for(unsigned bit = 0; bit < 5; ++bit)
        if((status & (1u << (16 + bit))) &&
           !t.control(t.context, 0x23, 1, uint16_t(16 + bit), port, nullptr, 0))
            return false;
    t.delay_ms(t.context, 10);  // Reset recovery before address-zero traffic.
    if(!port_status(t, port, status))
        return false;
    if(stable_high_speed(status) || (t.split_children && unsupported_speed(status)))
    {
        if(!t.child(t.context, port))
            return false;
    }
    else
    {
        unsigned speed = unsupported_speed(status);
        if(speed && t.unsupported)
            t.unsupported(t.context, port, speed);
        if((status & (0x1fu << 16)) || !t.control(t.context, 0x23, 1, 1, port, nullptr, 0) ||
           !port_status(t, port, status) || (status & 0x12))
            return false;
    }

    return true;
}
bool power_ports(const Transport& t, const Descriptor& hub)
{
    if(!hub.ports || hub.ports > 15)
        return false;
    for(uint8_t port = 1; port <= hub.ports; ++port)
        if(!t.control(t.context, 0x23, 3, 8, port, nullptr, 0))
            return false;
    return true;
}
bool enumerate(const Transport& t, const Descriptor& hub)
{
    if(!power_ports(t, hub))
        return false;
    t.delay_ms(t.context, hub.power_delay_ms);
    for(uint8_t port = 1; port <= hub.ports; ++port)
        if(!enumerate_port(t, port))
            return false;
    return true;
}
}  // namespace usb::hub
