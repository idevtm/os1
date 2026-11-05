// Blocking console-read syscall helpers. These functions complete immediately
// when a line is queued or put the current thread to sleep until input arrives.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mm/page_frame.hpp"
#include "proc/thread.hpp"

// Wake console-read waiters after input arrives.
void wake_console_readers(PageFrameContainer& frames);
// Returns false only after ownership was transferred into a registered wait.
bool read_or_block(PageFrameContainer& frames, Thread* thread, uint64_t fd,
                   uint64_t user_buffer, size_t length, long& result);
// Detach queued requests; claimed wakeups retain io_pins until their last access.
void cancel_console_read(Thread* thread);
