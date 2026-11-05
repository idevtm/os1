#pragma once
#include <os1/ipc.h>

#include "proc/thread.hpp"
constexpr long kIpcBlocked = -4098;
long sys_ipc(PageFrameContainer&, Thread*, uint64_t operation, uint64_t fd, uint64_t argument);
void cancel_ipc_wait(Thread*);
// Drains already-signalled waiters only; does not poll endpoint/Operation readiness.
void ipc_drain_wakeups();

// Called once per BSP scheduler tick, independent of AP count.
void ipc_timer_tick(uint64_t elapsed_ns);
bool ipc_stop_requested(const Thread*);

void ipc_stop_children(Process* parent);

// Immutable endpoint identity check for the optional supervised self-export grant.
bool ipc_matching_endpoints(NativeObject* provider, NativeObject* client);

#ifdef OS1_HOST_TEST
// Test-only scheduling point after request reservation and before descriptor delivery.
extern void (*host_ipc_before_receive)();
#endif
