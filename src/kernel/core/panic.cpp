// Minimal halt loop used after unrecoverable kernel failures.
#include "core/panic.hpp"

#include "console/console.hpp"
#include "debug/bootlog.hpp"
#include "debug/debug.hpp"
#include "drivers/watchdog/intel_tco.hpp"

[[noreturn]] void halt_forever()
{
    // During real-hardware bring-up, keep the failure on screen for a photo,
    // then reset so the next boot can save the RAM log. The TCO watchdog resets
    // on its own if nobody presses a key.
    if(bootlog_active())
    {
        asm volatile("cli");
        bootlog_mark(kBootLogFlagFatalStop);
        debug.set_mirror(console_mirror_debug_byte);
        debug("==== FATAL: os1 stopped. Photograph this screen, then press any key to")();
        debug("==== reset and save the boot log.")();
        if(intel_tco_watchdog_armed())
        {
            debug("==== The diagnostic watchdog will also reset the machine if no key is pressed.")();
        }
        bootlog_wait_for_keypress();
        debug("fatal: resetting to preserve boot log")();
        bootlog_reset_machine();
    }
    for(;;)
    {
        asm volatile("cli");
        asm volatile("hlt");
    }
}
