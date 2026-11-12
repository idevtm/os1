#pragma once
#include "proc/thread.hpp"
constexpr long kVfsBlocked = -4096;
// Export denied admission before any file I/O or wait registration.
constexpr long kVfsRetry = -4099;
constexpr uint32_t kVfsRead = 0;
constexpr uint32_t kVfsWrite = 11;
long sys_vfs(PageFrameContainer&, Thread*, int fd, uint64_t operation, uint64_t argument);
long vfs_read_or_block(PageFrameContainer&, Thread*, OpenInstance*, uint64_t buffer, size_t length);
long vfs_write_or_block(
    PageFrameContainer&, Thread*, OpenInstance*, uint64_t buffer, size_t length);
bool start_vfs_worker(Process*, PageFrameContainer&);
void wake_vfs_worker_on_tick();
void service_vfs_waiters(PageFrameContainer&);
void cancel_vfs_wait(Thread*);
