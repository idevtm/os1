#pragma once
#include <stdint.h>
namespace os1::input
{
struct HidBoot
{
    unsigned configuration{}, interface{}, protocol{}, endpoint{}, packet{}, interval{};
};
inline bool hid_boot(const uint8_t* p,
                     unsigned length,
                     HidBoot& out,
                     unsigned selection = 0,
                     unsigned max_packet = 8)
{
    out = {};
    if(!p || length < 9 || p[0] != 9 || p[1] != 2 || !p[5] ||
       (unsigned(p[2]) | unsigned(p[3]) << 8) != length)
        return false;
    HidBoot candidate{};
    unsigned matching = 0;
    bool active = false, found = false;
    for(unsigned at = 9; at < length; at += p[at])
    {
        if(length - at < 2 || p[at] < 2 || p[at] > length - at)
            return false;
        if(p[at + 1] == 4)
        {
            if(p[at] < 9)
                return false;
            active = !p[at + 3] && p[at + 5] == 3 && p[at + 6] == 1 &&
                     (p[at + 7] == 1 || p[at + 7] == 2);
            candidate = {p[5], p[at + 2], p[at + 7], 0, 0, 0};
        }
        else if(p[at + 1] == 5)
        {
            if(p[at] < 7)
                return false;
            unsigned packet = p[at + 4] | unsigned(p[at + 5]) << 8;
            if(active && !found && (p[at + 2] & 0x80) && (p[at + 2] & 15) && !(p[at + 2] & 0x70) &&
               (p[at + 3] & 3) == 3 && packet >= (candidate.protocol == 1 ? 8u : 3u) &&
               packet <= max_packet && p[at + 6])
            {
                candidate.endpoint = p[at + 2];
                candidate.packet = packet;
                candidate.interval = p[at + 6];
                active = false;  // One interrupt IN endpoint per interface.
                if(matching++ == selection)
                {
                    out = candidate;
                    found = true;
                }
            }
        }
    }
    return found;
}
}  // namespace os1::input
