// Canonical console-input buffer shared by serial polling, PS/2 keyboard input,
// and blocking read syscalls.
#pragma once

#include <stddef.h>

#include "sync/smp.hpp"

struct WaitQueue;

constexpr size_t kConsoleInputMaxLineBytes = 128;

extern Spinlock g_console_input_lock;

// Reset input queues and current line state.
void console_input_initialize();
// Route one decoded keyboard byte to the canonical or owned input queue.
void console_input_on_keyboard_char(char ascii);
// Poll the serial port and feed any pending characters into console input.
void console_input_poll_serial();
// Return true when a complete line is available for readers.
bool console_input_has_line();
// Return the wait queue used by blocking console-read syscalls.
WaitQueue& console_input_read_wait_queue();

// Reserve a bounded prefix; copying to userspace happens with no input lock held.
// A successful reservation must be finished exactly once, committing only delivered bytes.
bool console_input_reserve(char* buffer, size_t capacity, size_t& count, long* status = nullptr);
void console_input_finish(size_t delivered, bool commit = true);

// Identity is a retained kernel object, never a user-visible token.
long console_input_acquire(const void* owner, bool handoff = false);
void console_input_release(const void* owner);
long console_input_key(const void* owner, unsigned& byte);
bool console_input_ready(const void* owner);
