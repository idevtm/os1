// Console stream implementation shared by kernel status messages and syscall
// write plumbing.
#include "console/console.hpp"

#include <os1/display.h>
#include <os1/resource.h>

#include "core/kernel_state.hpp"
#include "debug/debug.hpp"
#include "driver/display.h"
#include "drivers/display/text_display.hpp"
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

long set_console_column(unsigned column)
{
    IrqSpinGuard guard(g_console_output_lock);
    if(column >= (active_terminal ? active_terminal->columns() : 80))
        return OS1_ERR_INVALID;
    bool indicator = active_terminal && active_terminal->cursor_column() != column;
    if(active_terminal && !active_terminal->set_column(column))
        return OS1_ERR_INVALID;
    if(indicator)
    {
        debug.write_unmirrored('\n');
        for(unsigned i = 0; i < column; ++i)
            debug.write_unmirrored(' ');
        debug.write_unmirrored('^');
    }
    return 0;
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

void console_echo_backspace()
{
    IrqSpinGuard guard(g_console_output_lock);
    debug.write_unmirrored('\b');
    debug.write_unmirrored(' ');
    debug.write_unmirrored('\b');
    if(active_terminal)
        active_terminal->write('\b');
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

#ifndef OS1_HOST_TEST
namespace
{
NativeObject* graphics_owner;
bool native_display_active;
}  // namespace

long console_display_acquire(NativeObject* owner, bool native)
{
    IrqSpinGuard guard(g_console_output_lock);
    if(graphics_owner)
        return OS1_ERR_WOULD_BLOCK;
    auto& display = *static_cast<FramebufferTextDisplay*>(g_text_display->instance);
    if(!display.available || (native_display_active && !native))
        return OS1_ERR_UNSUPPORTED;
    graphics_owner = owner;
    display.graphics_owned = true;
    return native_display_active ? 1 : 0;
}

void console_display_release(NativeObject* owner, bool restored, bool native)
{
    IrqSpinGuard guard(g_console_output_lock);
    if(graphics_owner != owner)
        return;
    graphics_owner = nullptr;
    auto& display = *static_cast<FramebufferTextDisplay*>(g_text_display->instance);
    native_display_active |= native;
    if(native_display_active)
        return;
    display.graphics_owned = false;
    if(!restored)
    {
        display.available = false;
        return;
    }
    reset_framebuffer_text_display(display);
    if(active_terminal)
        active_terminal->link(g_text_display);
}

long console_display_fill(NativeObject* owner,
                          const os1_display_driver& driver,
                          const os1_framebuffer_surface& surface,
                          const os1_display_fill& fill)
{
    IrqSpinGuard guard(g_console_output_lock);
    if(graphics_owner != owner)
        return OS1_ERR_CLOSED;
    driver.fill(&surface, fill.x, fill.y, fill.width, fill.height, fill.rgb);
    return 0;
}

void console_switch_terminal(unsigned index)
{
    IrqSpinGuard guard(g_console_output_lock);
    if(active_terminal == &terminal[index])
        return;
    if(active_terminal)
        active_terminal->unlink();
    active_terminal = &terminal[index];
    active_terminal->link(g_text_display);
    keyboard.set_active_terminal(active_terminal);
}
#endif
