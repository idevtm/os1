#pragma once
#include <os1/console.h>
#include <stdint.h>
namespace os1::input
{
inline unsigned navigation_key(uint8_t key)
{
    switch(key)
    {
        case 0x48:
            return OS1_CONSOLE_KEY_UP;
        case 0x50:
            return OS1_CONSOLE_KEY_DOWN;
        case 0x4b:
            return OS1_CONSOLE_KEY_LEFT;
        case 0x4d:
            return OS1_CONSOLE_KEY_RIGHT;
        case 0x47:
            return OS1_CONSOLE_KEY_HOME;
        case 0x4f:
            return OS1_CONSOLE_KEY_END;
        case 0x53:
            return OS1_CONSOLE_KEY_DELETE;
        default:
            return 0;
    }
}
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
        return !released && (!extra || key == 0x1c || key == 0x35 || navigation_key(key));
    }
};

char ps2_console_character(const Ps2Modifiers& modifiers, uint8_t key);
}  // namespace os1::input
