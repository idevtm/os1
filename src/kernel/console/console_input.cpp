// Canonical line-input implementation shared by serial and keyboard sources.
// It maintains a small completed-line queue for blocking read syscalls.
#include "console/console_input.hpp"

#include <os1/vfs.h>
#include <stdlib.h>

#if !defined(OS1_HOST_TEST)
#include "arch/x86_64/cpu/io_port.hpp"
#endif
#include "console/terminal.hpp"
#include "debug/debug.hpp"
#include "sync/wait_queue.hpp"
#include "sync/smp.hpp"
#include "util/ctype.hpp"
#include "util/memory.h"

OS1_BSP_ONLY extern Terminal* active_terminal;

#ifdef OS1_HOST_TEST
void (*host_console_after_ready)() = nullptr;
#endif

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
OS1_LOCKED_BY(g_console_input_lock) bool g_pending_overflow = false;
OS1_LOCKED_BY(g_console_input_lock) char g_completed_lines[kConsoleInputCompletedLineCapacity][kConsoleInputMaxLineBytes]{};
OS1_LOCKED_BY(g_console_input_lock) size_t g_completed_lengths[kConsoleInputCompletedLineCapacity]{};
OS1_LOCKED_BY(g_console_input_lock) long g_completed_status[kConsoleInputCompletedLineCapacity]{};
OS1_LOCKED_BY(g_console_input_lock) size_t g_completed_head = 0;
OS1_LOCKED_BY(g_console_input_lock) size_t g_completed_tail = 0;
OS1_LOCKED_BY(g_console_input_lock) size_t g_completed_count = 0;
OS1_LOCKED_BY(g_console_input_lock) bool g_read_reserved = false;
OS1_LOCKED_BY(g_console_input_lock) size_t g_reserved_bytes = 0;
WaitQueue g_console_read_waiters{"console-read"};
OS1_LOCKED_BY(g_console_input_lock) const void* g_input_owner = nullptr;
OS1_LOCKED_BY(g_console_input_lock) unsigned char g_keys[64]{};
OS1_LOCKED_BY(g_console_input_lock) size_t g_key_head = 0, g_key_count = 0;
OS1_LOCKED_BY(g_console_input_lock) bool g_key_overflow = false;

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

void commit_pending_line(bool newline = true)
{
    const long status = g_pending_overflow ? OS1_ERR_RANGE : 0;
    if(g_pending_overflow)
    {
        g_pending_length = 0;
        g_pending_overflow = false;
        newline = false;
    }
    if(g_completed_count == kConsoleInputCompletedLineCapacity)
    {
        debug("console input line dropped: queue full")();
        g_pending_length = 0;
        return;
    }

    if(newline)
        g_pending_line[g_pending_length++] = '\n';
    memcpy(g_completed_lines[g_completed_tail], g_pending_line, g_pending_length);
    g_completed_lengths[g_completed_tail] = g_pending_length;
    g_completed_status[g_completed_tail] = status;
    g_completed_tail = (g_completed_tail + 1) % kConsoleInputCompletedLineCapacity;
    ++g_completed_count;
    g_pending_length = 0;
}

void handle_input_char(char ascii)
{
    if(g_input_owner)
    {
        if(g_key_count == sizeof(g_keys))
            g_key_overflow = true;
        else
        {
            g_keys[(g_key_head + g_key_count) % sizeof(g_keys)] = static_cast<unsigned char>(ascii);
            ++g_key_count;
        }
        return;
    }
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
        if(g_pending_overflow)
            return;
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

    if(ascii == 4)
    {
        commit_pending_line(false);
        return;
    }

    if(!isprint(ascii))
    {
        return;
    }

    if((g_pending_length + 1) >= kConsoleInputMaxLineBytes)
    {
        if(!g_pending_overflow)
            debug("console input line discarded: too long")();
        g_pending_overflow = true;
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
    memset(g_completed_status, 0, sizeof(g_completed_status));
    g_pending_length = 0;
    g_pending_overflow = false;
    g_read_reserved = false;
    g_reserved_bytes = 0;
    g_completed_head = 0;
    g_completed_tail = 0;
    g_completed_count = 0;
    g_input_owner = nullptr;
    g_key_head = g_key_count = 0;
    g_key_overflow = false;
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
#if !defined(OS1_HOST_TEST)
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
#endif
}

bool console_input_has_line()
{
    IrqSpinGuard guard(g_console_input_lock);
    return !g_input_owner && g_completed_count > 0 && !g_read_reserved;
}

WaitQueue& console_input_read_wait_queue()
{
    return g_console_read_waiters;
}

bool console_input_reserve(char* buffer, size_t capacity, size_t& count, long* status)
{
    IrqSpinGuard guard(g_console_input_lock);
    count = 0;
    if(status)
        *status = 0;
    if(g_input_owner || !buffer || !capacity || !g_completed_count || g_read_reserved)
        return false;
    if(status)
        *status = g_completed_status[g_completed_head];
    count = g_completed_lengths[g_completed_head];
    if(count > capacity) count = capacity;
    memcpy(buffer, g_completed_lines[g_completed_head], count);
    g_read_reserved = true;
    g_reserved_bytes = count;
    return true;
}

long console_input_acquire(const void* owner)
{
    IrqSpinGuard guard(g_console_input_lock);
    if(!owner)
        return OS1_ERR_INVALID;
    if(g_input_owner || g_read_reserved || g_pending_length || g_pending_overflow ||
       g_completed_count)
        return OS1_ERR_WOULD_BLOCK;
    g_input_owner = owner;
    g_key_head = g_key_count = 0;
    g_key_overflow = false;
    return 0;
}
void console_input_release(const void* owner)
{
    IrqSpinGuard guard(g_console_input_lock);
    if(g_input_owner != owner)
        return;
    g_input_owner = nullptr;
    g_key_head = g_key_count = 0;
    g_key_overflow = false;
}
long console_input_key(const void* owner, unsigned& byte)
{
    IrqSpinGuard guard(g_console_input_lock);
    if(g_input_owner != owner)
        return OS1_ERR_CLOSED;
    if(g_key_overflow)
    {
        g_key_overflow = false;
        return OS1_ERR_RANGE;
    }
    if(!g_key_count)
        return OS1_ERR_WOULD_BLOCK;
    byte = g_keys[g_key_head];
    g_key_head = (g_key_head + 1) % sizeof(g_keys);
    --g_key_count;
    return 0;
}
bool console_input_ready(const void* owner)
{
    bool ready;
    {
        IrqSpinGuard guard(g_console_input_lock);
        ready = g_input_owner != owner || g_key_count || g_key_overflow;
    }
#ifdef OS1_HOST_TEST
    if(host_console_after_ready) host_console_after_ready();
#endif
    return ready;
}

void console_input_finish(size_t delivered, bool commit)
{
    IrqSpinGuard guard(g_console_input_lock);
    if(!g_read_reserved) return;
    if(!commit || delivered > g_reserved_bytes)
    {
        g_read_reserved = false;
        g_reserved_bytes = 0;
        return;
    }
    const size_t remaining = g_completed_lengths[g_completed_head] - delivered;
    for(size_t i = 0; i < remaining; ++i)
        g_completed_lines[g_completed_head][i] = g_completed_lines[g_completed_head][i + delivered];
    g_completed_lengths[g_completed_head] = remaining;
    if(remaining == 0) {
        g_completed_head = (g_completed_head + 1) % kConsoleInputCompletedLineCapacity;
        --g_completed_count;
    }
    g_read_reserved = false;
    g_reserved_bytes = 0;
}
