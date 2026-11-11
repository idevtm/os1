// waitpid syscall helpers. The scheduler and process reaper call back here to
// wake parent threads when child process state changes.
#pragma once

#include <stdint.h>

#include "mm/page_frame.hpp"
#include "proc/thread.hpp"

// Try to complete waitpid for `thread`, writing status to user memory when done.
bool try_complete_wait_pid(PageFrameContainer& frames,
                           Thread* thread,
                           uint64_t pid,
                           uint64_t user_status_pointer,
                           long& result);
// Complete waitpid if possible, otherwise atomically block the current thread
// before a child-exit wake can be missed.
bool try_complete_or_block_wait_pid(PageFrameContainer& frames,
                                    Thread* thread,
                                    uint64_t pid,
                                    uint64_t user_status_pointer,
                                    long& result);
// Wake threads blocked on child-exit waits after a process exits or reaps.
[[nodiscard]] bool wake_child_waiters(PageFrameContainer& frames);
