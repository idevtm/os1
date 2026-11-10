#pragma once
#include <stdint.h>
namespace os1::input
{
struct Ps2Pointer
{
    uint8_t packet[3]{};
    unsigned used{};
    bool byte(uint8_t value, unsigned& buttons, int& dx, int& dy)
    {
        if(!used && !(value & 8))
            return false;
        packet[used++] = value;
        if(used != 3)
            return false;
        used = 0;
        if(packet[0] & 0xc0)
            return false;
        buttons = packet[0] & 7;
        dx = int(packet[1]) - ((packet[0] & 0x10) ? 256 : 0);
        dy = -(int(packet[2]) - ((packet[0] & 0x20) ? 256 : 0));
        return true;
    }
};
struct Ps2Keys
{
    uint8_t keys[32]{};
    bool extended{};
    unsigned pause{};
    bool byte(uint8_t value)
    {
        if(pause)
        {
            --pause;
            return false;
        }
        if(value == 0xe1)
        {
            pause = 5;
            extended = false;
            return false;
        }
        if(value == 0xe0)
        {
            extended = true;
            return false;
        }
        unsigned code = value & 127, usage = 0;
        if(extended)
        {
            switch(code)
            {
                case 0x1c:
                    usage = 88;
                    break;
                case 0x1d:
                    usage = 228;
                    break;
                case 0x35:
                    usage = 84;
                    break;
                case 0x37:
                    usage = 70;
                    break;
                case 0x38:
                    usage = 230;
                    break;
                case 0x47:
                    usage = 74;
                    break;
                case 0x48:
                    usage = 82;
                    break;
                case 0x49:
                    usage = 75;
                    break;
                case 0x4b:
                    usage = 80;
                    break;
                case 0x4d:
                    usage = 79;
                    break;
                case 0x4f:
                    usage = 77;
                    break;
                case 0x50:
                    usage = 81;
                    break;
                case 0x51:
                    usage = 78;
                    break;
                case 0x52:
                    usage = 73;
                    break;
                case 0x53:
                    usage = 76;
                    break;
                case 0x5b:
                    usage = 227;
                    break;
                case 0x5c:
                    usage = 231;
                    break;
                case 0x5d:
                    usage = 101;
                    break;
            }
        }
        else
        {
            constexpr uint8_t map[] = {
                0,   41, 30,  31, 32, 33, 34,  35, 36, 37, 38, 39,  45, 46, 42,  43, 20, 26,
                8,   21, 23,  28, 24, 12, 18,  19, 47, 48, 40, 224, 4,  22, 7,   9,  10, 11,
                13,  14, 15,  51, 52, 53, 225, 49, 29, 27, 6,  25,  5,  17, 16,  54, 55, 56,
                229, 85, 226, 44, 57, 58, 59,  60, 61, 62, 63, 64,  65, 66, 67,  83, 71, 95,
                96,  97, 86,  92, 93, 94, 87,  89, 90, 91, 98, 99,  0,  0,  100, 68, 69};
            if(code < sizeof(map))
                usage = map[code];
        }
        extended = false;
        if(!usage)
            return false;
        uint8_t mask = 1u << (usage % 8), before = keys[usage / 8];
        if(value & 128)
            keys[usage / 8] &= ~mask;
        else
            keys[usage / 8] |= mask;
        return before != keys[usage / 8];
    }
};
}  // namespace os1::input
