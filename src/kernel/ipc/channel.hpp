#pragma once
#include <os1/ipc.h>

#include "proc/thread.hpp"
constexpr long kIpcBlocked = -4098;
long sys_ipc(PageFrameContainer&, Thread*, uint64_t operation, uint64_t fd, uint64_t argument);
void cancel_ipc_wait(Thread*);
// Drains already-signalled waiters only; does not poll endpoint/Operation readiness.
void ipc_drain_wakeups();
