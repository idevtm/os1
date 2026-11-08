// Console stream implementation shared by kernel status messages and syscall
// write plumbing.
#include "console/console.hpp"

#include <os1/resource.h>

#include "core/kernel_state.hpp"
#include "debug/debug.hpp"
#include "sync/smp.hpp"
#include "terminal/line.hpp"

namespace
{
Spinlock g_console_output_lock{"console-output"};
}

os1_console_geometry console_geometry()
{
    IrqSpinGuard guard(g_console_output_lock);
    return active_terminal
               ? os1_console_geometry{active_terminal->columns(), active_terminal->rows()}
               : os1_console_geometry{80, 25};
}
void clear_console()
{
    IrqSpinGuard guard(g_console_output_lock);
    for(char c : "\033[2J\033[H")
        if(c)
            debug.write_unmirrored(c);
    if(active_terminal)
        active_terminal->clear();
}

long redraw_console_line(int delta, const char* text, size_t bytes)
{
    IrqSpinGuard guard(g_console_output_lock);
    unsigned columns = active_terminal ? active_terminal->columns() : 80;
    if(!os1::terminal::valid_line(columns, delta, text, bytes))
        return OS1_ERR_INVALID;
    if(active_terminal && !active_terminal->redraw_line(delta, text, bytes))
        return OS1_ERR_INVALID;
    // Serial terminals need no cursor capability. Keep a sequential transcript.
    debug.write_unmirrored('\n');
    for(size_t i = 0; i < bytes; ++i)
        debug.write_unmirrored(text[i]);
    return 0;
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
