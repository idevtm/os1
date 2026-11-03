// Console stream implementation shared by kernel status messages and syscall
// write plumbing.
#include "console/console.hpp"

#include "core/kernel_state.hpp"
#include "debug/debug.hpp"
#include "sync/smp.hpp"

namespace
{
Spinlock g_console_output_lock{"console-output"};
}

void write_console_bytes(const char* data, size_t length)
{
    if(nullptr == data)
    {
        return;
    }

    IrqSpinGuard guard(g_console_output_lock);
    for(size_t i = 0; i < length; ++i)
    {
        debug.write_unmirrored(data[i]);
        if(active_terminal)
        {
            active_terminal->write(data[i]);
        }
    }
}

void write_console_line(const char* text)
{
    if(nullptr == text)
    {
        return;
    }

    IrqSpinGuard guard(g_console_output_lock);
    for(const char* cursor = text; '\0' != *cursor; ++cursor)
    {
        debug.write_unmirrored(*cursor);
    }
    debug.write_unmirrored('\n');
    if(active_terminal)
    {
        active_terminal->write_line(text);
    }
}

void console_mirror_debug_byte(char c)
{
    IrqGuard irq;
    if(!g_console_output_lock.try_lock())
    {
        return;
    }
    if(active_terminal)
    {
        active_terminal->write(c);
    }
    g_console_output_lock.unlock();
}
