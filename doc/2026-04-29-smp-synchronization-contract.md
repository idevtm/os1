# SMP Synchronization Contract - 2026-04-29

Created: 2026-04-29. Updated against source: 2026-11-03 (`63e328d`).

## Current State

APs run per-CPU idle threads and user threads, install the shared IDT, take
local LAPIC timer ticks, and consume reschedule and TLB-shootdown IPIs. Per-CPU
run queues and load balancing are implemented. Process/thread registries,
physical allocation, console queues, and blocking paths have shared-state
synchronization; they are no longer collectively BSP-only.

This is the live synchronization contract carried forward from the original
pre-SMP plan. It records existing mechanisms and required ownership rules,
not a proof that every path satisfies them. The
[2026-11-03 review](2026-11-03-review.md) identifies the outstanding child-thread
publication defect and the limits of the current reclamation evidence.

## Kernel Primitives

The synchronization declarations live in [smp.hpp](../src/kernel/sync/smp.hpp):

- `Spinlock`: a named, non-recursive atomic lock with acquire/release semantics.
- `IrqGuard`: disables local external interrupts and restores the prior enabled
  state on destruction; it does not exclude another CPU.
- `IrqSpinGuard`: acquires the IRQ guard before the lock and releases the lock
  before restoring interrupts.
- `OS1_BSP_ONLY` and `OS1_LOCKED_BY(name)`: documentation annotations, both
  expanding to `[[maybe_unused]]`. They enforce neither affinity nor lock order.
- `KASSERT_ON_BSP()`: a runtime BSP assertion in freestanding debug builds. It
  is disabled in host tests and builds defining `NDEBUG`.

`WaitQueue` and `Completion` in
[wait_queue.hpp](../src/kernel/sync/wait_queue.hpp) support block-I/O, child-exit,
and console-read blocking. Wake helpers detach waiters under the queue lock,
then release it before making threads runnable.

## Current Ownership

| State | Existing protection or ownership |
| --- | --- |
| Physical page allocation/free | `g_page_frames_lock`; allocator initialization remains BSP-owned. |
| Process registry and PID allocation | `g_process_table_lock` in `proc/process.cpp`. |
| Thread registry and TID allocation | `g_thread_registry_lock` in `proc/thread.cpp`. |
| Runnable links and queue length | The owning CPU's `runq.lock`; balancing acquires a pair in ascending CPU-ID order. |
| Console input queues | `g_console_input_lock`; hardware polling/input production remains BSP-owned. |
| Console API writes | `g_console_output_lock` protects `write_console_bytes`/`write_console_line` and the debug mirror. Input echo and terminal switching do not take that lock; terminal selection retains BSP ownership. |
| Platform discovery | BSP initialization of `g_platform`; device IRQ affinity remains BSP-first. Registry locks do not make device lifecycle or ACPICA calls generally concurrent-safe. |

Some registry and allocator globals still carry `OS1_BSP_ONLY` alongside real
internal locks. Read the mutation path rather than treating that marker as a
complete ownership description. CPU-local scheduler state, startup publication,
and shared object lifetime need their own rules in addition to these locks.

## Locking Order

Lock acquisition must follow this order. This remains a design rule; there is
no static or runtime lock-tier checker. Do not introduce a new lock outside this
order without updating this document first.

1. CPU-local interrupt state: acquire `IrqGuard` before taking any lock that can also be touched from an interrupt path.
2. Physical memory allocator: `page_frames` lock.
3. Virtual memory/address-space ownership locks.
4. Process table lock.
5. Thread table and scheduler/run-queue locks.
6. Wait/IPC/event queues.
7. Console input queue lock.
8. Terminal/display locks.
9. Platform/PCI/device-list locks.
10. Individual driver/device locks.

Rules:

- Never allocate physical pages while holding a driver/device lock.
- Never call into console or debug output while holding the process, thread, scheduler, platform, or allocator lock unless the path is explicitly panic-only.
- Never sleep or block while holding a spinlock.
- Never acquire a lower-numbered lock while holding a higher-numbered lock.
- Interrupt handlers may only take locks that are IRQ-safe and must do so under `IrqGuard`.
- When taking two run-queue locks, acquire them in ascending CPU-ID order and
  release them in reverse order.
- Detach waiters and release the wait-queue lock before taking an earlier-tier
  scheduler/run-queue lock to wake them.
- Locks protect data ownership; they do not replace lifetime rules. Process,
  thread, page-table, and device lifetimes still need explicit ownership transitions.

## Publication And Reclamation Follow-Ups

The original migration to scheduled AP execution landed in May; it is recorded
in the [phased SMP plan](2026-05-05-phased-smp-enablement.md) and
[May review](2026-05-05-review-2.md). The remaining work is correctness under
that live runtime:

1. Complete a child's entry frame and ownership state before enqueueing it or
   sending a reschedule IPI. `load_user_program` currently rewrites the frame
   after ready-thread creation; see
   [R1](2026-11-03-review.md#r1--p1-child-entry-state-is-rewritten-after-scheduler-publication).
2. Define address-space reclamation and invalidation completion before adding
   shared address spaces, COW, or public mapping operations. The existing
   broadcast TLB IPI has no address-space/range acknowledgement protocol;
   having callers does not establish when frames are safe to reuse.
3. Establish actual ACPICA serialization before concurrent AML or SCI/GPE
   expansion. The OSL currently has dummy locks and nonblocking semaphores.
4. Keep allocation and device operations outside incompatible locks; add
   lock-order assertions and focused cross-CPU tests as enforcement evolves.
5. Retain SMP balance/spawn/exec coverage and add a deterministic publication
   regression. Passing QEMU runs alone do not close the unresolved BIOS
   child-launch failure or prove every lifetime rule.

`KASSERT_ON_BSP()` failures in enabled builds remain correctness bugs, not
diagnostic noise. Remove an ownership assertion only after the replacement
contract is explicit and exercised.
