#pragma once
#include <os1/ipc.h>

#include "proc/thread.hpp"
constexpr long kIpcBlocked = -4098;
long sys_ipc(PageFrameContainer&, Thread*, uint64_t operation, uint64_t fd, uint64_t argument);
void cancel_ipc_wait(Thread*);
// Drains already-signalled waiters only; does not poll endpoint/Operation readiness.
void ipc_drain_wakeups();
// Call after hardware dispatch and after releasing controller and input locks.
void ipc_refresh_input_waiters();

// Called once per BSP scheduler tick, independent of AP count.
void ipc_timer_tick(uint64_t elapsed_ns);
bool ipc_stop_requested(const Thread*);

void ipc_stop_children(Process* parent);

// Immutable endpoint identity check for the optional supervised self-export grant.
bool ipc_matching_endpoints(NativeObject* provider, NativeObject* client);

#ifdef OS1_HOST_TEST
// Test-only scheduling point after request reservation and before descriptor delivery.
extern void (*host_ipc_before_receive)();
extern int host_ipc_kernel_allocation_budget;
#endif

// Start once before publishing user processes. Dispatch runs in thread context.
bool ipc_start_kernel_worker(Process*, PageFrameContainer&);
// One nonblocking dispatch step; also used by host contract tests.
enum class KernelDispatch : uint8_t
{
    Any,
    Service,
    Export
};
bool ipc_run_kernel_request(KernelDispatch lane = KernelDispatch::Any);

// Atomic check/registration for a kernel dispatch lane; empty IPC selection.
bool ipc_wait_kernel_work(Thread*, KernelDispatch lane = KernelDispatch::Any);

// Kernel-thread timed sleep; shares acknowledged scheduler-frame retirement.
void kernel_sleep_for(Thread*, uint64_t delay_ns);
