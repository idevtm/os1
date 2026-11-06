#pragma once
#include "proc/thread.hpp"
constexpr long kVfsBlocked = -4096;
constexpr uint32_t kVfsRead = 0;
constexpr uint32_t kVfsWrite = 11;
long sys_vfs(PageFrameContainer&, Thread*, int fd, uint64_t operation, uint64_t argument);
long vfs_read_or_block(PageFrameContainer&, Thread*, OpenInstance*, uint64_t buffer, size_t length);
long vfs_write_or_block(
    PageFrameContainer&, Thread*, OpenInstance*, uint64_t buffer, size_t length);
void wake_vfs_waiters(PageFrameContainer&);
void cancel_vfs_wait(Thread*);
