// Minimal halt loop used after unrecoverable kernel failures.
#include "core/panic.hpp"

#include "debug/bootlog.hpp"
#include "debug/debug.hpp"

[[noreturn]] void halt_forever()
{
    // During real-hardware bring-up, reset instead of halting so the next boot
    // can save the RAM log that explains this failure.
    if(bootlog_active())
    {
        bootlog_mark(kBootLogFlagFatalStop);
        debug("fatal: resetting to preserve boot log")();
        bootlog_reset_machine();
    }
    for(;;)
    {
        asm volatile("cli");
        asm volatile("hlt");
    }
}