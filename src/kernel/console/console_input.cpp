// Canonical line-input implementation shared by serial and keyboard sources.
// It maintains a small completed-line queue for blocking read syscalls.
#include "console/console_input.hpp"

#include <stdlib.h>

#include "arch/x86_64/cpu/io_port.hpp"
#include "console/terminal.hpp"
#include "debug/debug.hpp"
#include "sync/wait_queue.hpp"
#include "sync/smp.hpp"
#include "util/ctype.hpp"
#include "util/memory.h"

OS1_BSP_ONLY extern Terminal* active_terminal;

Spinlock g_console_input_lock{"console-input"};

namespace
{
constexpr uint16_t kSerialPortBase = 0x3F8;
constexpr uint16_t kSerialLineStatusPort = kSerialPortBase + 5;
constexpr uint8_t kSerialDataReady = 0x01;
constexpr size_t kConsoleInputCompletedLineCapacity = 8;

// Serial and keyboard input are consumed on the BSP, while blocked readers may
// be running on any CPU.
OS1_LOCKED_BY(g_console_input_lock) char g_pending_line[kConsoleInputMaxLineBytes]{};
OS1_LOCKED_BY(g_console_input_lock) size_t g_pending_length = 0;
OS1_LOCKED_BY(g_console_input_lock) char g_completed_lines[kConsoleInputCompletedLineCapacity][kConsoleInputMaxLineBytes]{};
OS1_LOCKED_BY(g_console_input_lock) size_t g_completed_lengths[kConsoleInputCompletedLineCapacity]{};
OS1_LOCKED_BY(g_console_input_lock) size_t g_completed_head = 0;
OS1_LOCKED_BY(g_console_input_lock) size_t g_completed_tail = 0;
OS1_LOCKED_BY(g_console_input_lock) size_t g_completed_count = 0;
WaitQueue g_console_read_waiters{"console-read"};

void echo_byte(char c)
{
    debug.write(c);
    if(active_terminal)
    {
        active_terminal->write(c);
    }
}

void echo_backspace()
{
    debug.write('\b');
    debug.write(' ');
    debug.write('\b');
    if(active_terminal)
    {
        active_terminal->write('\b');
    }
}

void commit_pending_line()
{
    if(g_completed_count == kConsoleInputCompletedLineCapacity)
    {
        debug("console input line dropped: queue full")();
        g_pending_length = 0;
        return;
    }

    memcpy(g_completed_lines[g_completed_tail], g_pending_line, g_pending_length);
    g_completed_lines[g_completed_tail][g_pending_length] = '\n';
    g_completed_lengths[g_completed_tail] = g_pending_length + 1;
    g_completed_tail = (g_completed_tail + 1) % kConsoleInputCompletedLineCapacity;
    ++g_completed_count;
    g_pending_length = 0;
}

void handle_input_char(char ascii)
{
    if('\r' == ascii)
    {
        ascii = '\n';
    }
    else if((0x7F == static_cast<unsigned char>(ascii)) || ('\b' == ascii))
    {
        ascii = '\b';
    }

    if('\b' == ascii)
    {
        if(g_pending_length > 0)
        {
            --g_pending_length;
            echo_backspace();
        }
        return;
    }

    if('\n' == ascii)
    {
        echo_byte('\n');
        commit_pending_line();
        return;
    }

    if(!isprint(ascii))
    {
        return;
    }

    if((g_pending_length + 1) >= kConsoleInputMaxLineBytes)
    {
        debug("console input line dropped: line too long")();
        return;
    }

    g_pending_line[g_pending_length++] = ascii;
    echo_byte(ascii);
}
}  // namespace

void console_input_initialize()
{
    KASSERT_ON_BSP();
    IrqSpinGuard guard(g_console_input_lock);
    memset(g_pending_line, 0, sizeof(g_pending_line));
    memset(g_completed_lines, 0, sizeof(g_completed_lines));
    memset(g_completed_lengths, 0, sizeof(g_completed_lengths));
    g_pending_length = 0;
    g_completed_head = 0;
    g_completed_tail = 0;
    g_completed_count = 0;
    g_console_read_waiters.lock.reset("console-read");
    g_console_read_waiters.head = nullptr;
    g_console_read_waiters.name = "console-read";
}

void console_input_on_keyboard_char(char ascii)
{
    KASSERT_ON_BSP();
    IrqSpinGuard guard(g_console_input_lock);
    handle_input_char(ascii);
}

void console_input_poll_serial()
{
    KASSERT_ON_BSP();
    // Runs from the timer tick with interrupts off. Without a UART the status
    // port reads 0xFF ("data ready" forever), which hung the ASUS G74SX at its
    // first tick; skip absent ports and bound the drain either way.
    constexpr uint32_t kMaxBytesPerPoll = 16;
    if(!debug.serial_available())
    {
        return;
    }
    IrqSpinGuard guard(g_console_input_lock);
    for(uint32_t i = 0; (i < kMaxBytesPerPoll) && ((inb(kSerialLineStatusPort) & kSerialDataReady) != 0); ++i)
    {
        handle_input_char((char)inb(kSerialPortBase));
    }
}

bool console_input_has_line()
{
    IrqSpinGuard guard(g_console_input_lock);
    return g_completed_count > 0;
}

bool console_input_pop_line(char* buffer, size_t buffer_size, size_t& line_length)
{
    IrqSpinGuard guard(g_console_input_lock);
    line_length = 0;
    if((nullptr == buffer) || (0 == buffer_size) || (0 == g_completed_count))
    {
        return false;
    }

    const size_t next_length = g_completed_lengths[g_completed_head];
    if(next_length > buffer_size)
    {
        return false;
    }

    memcpy(buffer, g_completed_lines[g_completed_head], next_length);
    memset(g_completed_lines[g_completed_head], 0, sizeof(g_completed_lines[g_completed_head]));
    g_completed_lengths[g_completed_head] = 0;
    g_completed_head = (g_completed_head + 1) % kConsoleInputCompletedLineCapacity;
    --g_completed_count;
    line_length = next_length;
    return true;
}

WaitQueue& console_input_read_wait_queue()
{
    return g_console_read_waiters;
}
