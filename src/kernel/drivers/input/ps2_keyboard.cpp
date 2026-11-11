// PS/2 keyboard scan-code decoder. It converts controller bytes into console
// input characters while also forwarding raw hotkeys to terminal switching.
#include "drivers/input/ps2_keyboard.hpp"

#include "arch/x86_64/cpu/io_port.hpp"
#include "console/console_input.hpp"
#include "console/terminal_switcher.hpp"
#include "debug/bootlog.hpp"
#include "debug/debug.hpp"
#include "drivers/input/service.hpp"
#include "drivers/timer/tsc_clock.hpp"
#include "input/ps2_modifiers.hpp"
#include "input/ps2_packets.hpp"
#include "sync/smp.hpp"

namespace
{
// Bring-up diagnostics: log the first scancodes so a boot log shows whether
// keyboard IRQs arrive at all on a new machine.
constexpr uint32_t kLoggedScancodeLimit = 32;
uint32_t g_logged_scancodes = 0;
}  // namespace

os1::input::Ps2Modifiers modifiers;
namespace
{
os1::input::Ps2Keys native_keys;
os1::input::Ps2Pointer native_pointer;
uint64_t keyboard_identity, pointer_identity;
void pointer_byte(uint8_t byte)
{
    unsigned buttons = 0;
    int dx = 0, dy = 0;
    if(native_pointer.byte(byte, buttons, dx, dy))
        native_input::pointer(pointer_identity, buttons, dx, dy);
}
bool wait_write()
{
    for(unsigned i = 0; i < 1000; ++i)
    {
        if(!(inb(0x64) & 2))
            return true;
        tsc_clock_delay_us(10);
    }
    return false;
}
bool command(uint8_t byte)
{
    if(!wait_write())
        return false;
    outb(0x64, byte);
    return true;
}
bool read_aux(uint8_t& byte)
{
    for(unsigned i = 0; i < 1000; ++i)
    {
        unsigned status = inb(0x64);
        if(status & 1)
        {
            byte = inb(0x60);
            if(status & 32)
                return !(status & 0xc0);
        }
        tsc_clock_delay_us(10);
    }
    return false;
}
bool aux_command(uint8_t byte)
{
    if(!command(0xd4) || !wait_write())
        return false;
    outb(0x60, byte);
    uint8_t reply = 0;
    return read_aux(reply) && reply == 0xfa;
}
bool initialize_aux()
{
    // Boot-only transaction, before other CPUs or user input can consume bytes.
    IrqGuard guard;
    if(!command(0xa8) || !aux_command(0xf6))
        return false;
    if(!command(0x20))
        return false;
    unsigned config = 0;
    bool got_config = false;
    for(unsigned i = 0; i < 1000; ++i)
    {
        if(inb(0x64) & 1)
        {
            config = inb(0x60);
            got_config = true;
            break;
        }
        tsc_clock_delay_us(10);
    }
    if(!got_config || !command(0x60) || !wait_write())
        return false;
    outb(0x60, (config | 2) & ~32u);
    if(!aux_command(0xf4))
        return false;
    pointer_identity = native_input::attach(OS1_INPUT_POINTER, "PS/2 relative pointer");
    return pointer_identity != 0;
}
}  // namespace

void Keyboard::irq_handler(Keyboard* object)
{
    (void)object;
    // hook for keyboard handler
    const uint16_t KBDATAPORT = 0x60; /* kbd data port */
                                      //	const uint16_t KBSTATUS =	0x61;	/* kbd status */
    const uint16_t KBSTATPORT = 0x64; /* kbd status port */
    const uint8_t KBINRDY = 0x01;
    //	const uint8_t	KBOUTRDY = 0x02;

    uint8_t status = inb(KBSTATPORT);
    if((status & KBINRDY) == 0)
        return;

    // read scan code
    uint8_t scancode = inb(KBDATAPORT);
    if(status & 0xc0)
        return;
    if(status & 32)
    {
        pointer_byte(scancode);
        return;
    }
    if(native_keys.byte(scancode))
        native_input::keyboard(keyboard_identity, native_keys.keys);
    if(bootlog_diagnostics_enabled() && (g_logged_scancodes < kLoggedScancodeLimit))
    {
        ++g_logged_scancodes;
        debug("ps2: scancode 0x")(scancode, 16, 2)();
    }
    uint8_t key = 0;
    if(!modifiers.press(scancode, key))
        return;
    if(!kernel_keyboard_hook(scancode))
        return;
    if(const char character = os1::input::ps2_console_character(modifiers, key))
        console_input_on_keyboard_char(character);
}

bool Keyboard::initialize()
{
    keyboard_identity = native_input::attach(OS1_INPUT_KEYBOARD, "PS/2 keyboard");
    interrupts_.set_irq_handler(12, (void (*)(void*))irq_handler, this);
    initialize_aux();
    interrupts_.set_irq_handler(1, (void (*)(void*))irq_handler, this);  // hook keyboard irq
    return true;                                                         // I guess
}

void Keyboard::set_active_terminal(Terminal* terminal)
{
    active_terminal_ = terminal;
}
