#pragma once
#include <stdint.h>
namespace os1::input
{
struct Ps2Modifiers
{
    uint8_t shift = 0, control = 0, pause = 0;
    bool extended = false, keypad = false;
    bool press(uint8_t scancode, uint8_t& key)
    {
        if(pause)
        {
            --pause;
            return false;
        }
        if(scancode == 0xe1)
        {
            pause = 5;
            extended = false;
            return false;
        }
        if(scancode == 0xe0)
        {
            extended = true;
            return false;
        }
        const bool extra = extended;
        extended = false;
        keypad = extra;
        const bool released = scancode & 0x80;
        key = scancode & 0x7f;
        uint8_t* state = nullptr;
        uint8_t bit = 0;
        if(key == 0x1d)
        {
            state = &control;
            bit = extra ? 2 : 1;
        }
        else if(!extra && (key == 0x2a || key == 0x36))
        {
            state = &shift;
            bit = key == 0x2a ? 1 : 2;
        }
        if(state)
        {
            if(released)
                *state &= ~bit;
            else
                *state |= bit;
            return false;
        }
        return !released && (!extra || key == 0x1c || key == 0x35);
    }
};
}  // namespace os1::input
