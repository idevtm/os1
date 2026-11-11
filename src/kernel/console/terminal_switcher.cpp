// Fixed F1-F12 terminal switching policy extracted from the kernel entry file.
#include "console/terminal_switcher.hpp"

#include "console/console.hpp"
#include "core/kernel_state.hpp"
#include "sync/smp.hpp"

bool kernel_keyboard_hook(uint16_t scancode)
{
    KASSERT_ON_BSP();
    uint16_t hotkey[kNumTerminals] = {
        0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58};
    int index = -1;
    for(unsigned i = 0; i < kNumTerminals; i++)
    {
        if(scancode == hotkey[i])
        {
            index = i;
            break;
        }
    }

    if(index >= 0)
        console_switch_terminal(index);
    return true;
}
