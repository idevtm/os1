# Phased SMP Enablement for `os1` - 2026-05-05

> Source-first design doc. Where this document and other docs disagree, the
> source code at the time of writing wins. Mismatches are called out inline.
>
> Scope: take `os1` from the current "APs boot and park" state up to full user-process
> SMP scheduling with basic load balancing. Each phase is defined so it can be merged,
> smoke-tested, and rolled back independently.

> Status update: this plan has been implemented in source. Keep it as the
> historical implementation plan for the 2026-05-05 SMP landing; for the
> post-landing assessment and the remaining follow-up work, see
> [2026-05-05-review-2.md](../reviews/2026-05-05-review-2.md) and
> [2026-05-05-smp-enablement-review.md](../reviews/2026-05-05-smp-enablement-review.md).

## 1. Current state

### 1.1 What SMP groundwork already exists

CPU records and topology:

- ACPI MADT discovery in [src/kernel/platform/acpi.cpp](../../../src/kernel/platform/acpi.cpp)
  populates `g_platform.cpus[]` in [src/kernel/platform/state.cpp](../../../src/kernel/platform/state.cpp).
- [src/kernel/platform/topology.cpp:16-52](../../../src/kernel/platform/topology.cpp#L16-L52)
  walks `g_platform.cpus[]` and calls `cpu_alloc()` for every non-BSP enabled
  entry, which appends a per-CPU page-aligned `struct cpu` to the boot list.
- `struct cpu` in [src/kernel/arch/x86_64/cpu/cpu.hpp:51-76](../../../src/kernel/arch/x86_64/cpu/cpu.hpp#L51-L76)
  already carries `current_thread`, `interrupt_frame`, `gdt[]`, `tss`, `id`,
  `booted`, `magic`, and an embedded `kstackhi` page-aligned kernel stack.
  The page-low layout makes `cpu_cur()` a one-instruction read off `%gs:0`
  with a stack-base fallback ([cpu.hpp:108-121](../../../src/kernel/arch/x86_64/cpu/cpu.hpp#L108-L121)).

AP bring-up path:

- The trampoline in [src/kernel/arch/x86_64/asm/cpu_start.asm](../../../src/kernel/arch/x86_64/asm/cpu_start.asm)
  is copied to `kApTrampolineAddress`, hands long-mode entry to a 64-bit thunk,
  loads CR3, sets RSP to `cpu_page + PAGE_SIZE`, and calls the C++ entry pointer
  stored in `kApStartupRipAddress`.
- That entry pointer is `init` in [cpu.cpp:160-173](../../../src/kernel/arch/x86_64/cpu/cpu.cpp#L160-L173).
  `init()` runs `cpu_init()` (GDT/TSS/GS, syscall MSRs), then `ioapic_init()`,
  then `lapic_init()`, sets `booted = 1`, then enters `cpu_idle_loop()` which
  is `cli; hlt;` forever.
- `cpu_boot_others` in [cpu.cpp:175-211](../../../src/kernel/arch/x86_64/cpu/cpu.cpp#L175-L211)
  drives INIT/STARTUP IPIs through `lapic_start_cpu` ([lapic.cpp:152-185](../../../src/kernel/arch/x86_64/apic/lapic.cpp#L152-L185)).

Per-CPU state primitives that already work today:

- `cpu_cur()->current_thread` is the canonical current-thread slot
  ([thread.cpp:292-314](../../../src/kernel/proc/thread.cpp#L292-L314)). `set_current_thread`
  already touches only the local CPU.
- `cpu_set_kernel_stack` writes `cpu_cur()->tss.rsp0` ([cpu.cpp:91-94](../../../src/kernel/arch/x86_64/cpu/cpu.cpp#L91-L94)),
  so the ring-3 entry path is already CPU-local.
- `current_cpu_id()` in [event_ring.cpp:34-37](../../../src/kernel/debug/event_ring.cpp#L34-L37)
  returns the local APIC ID; events already record per-CPU origin.

Synchronization vocabulary:

- `Spinlock` (test-and-set on `volatile uint32_t`, acquire/release semantics)
  and `IrqGuard` (RAII `cli`/`sti` with prior-state restore) both live in
  [src/kernel/sync/smp.hpp](../../../src/kernel/sync/smp.hpp).
- `OS1_BSP_ONLY` is a documentation marker (`[[maybe_unused]]`); it is *not*
  a lock and not enforced. `KASSERT_ON_BSP()` is the runtime check.
- The locking-order contract in
  [docs/kernel/smp-synchronization.md](../../kernel/smp-synchronization.md)
  defines a 10-tier order. It is in force for every new lock added below.

Already SMP-safe (have working spinlocks today):

- Event ring: [event_ring.cpp:17,135-148](../../../src/kernel/debug/event_ring.cpp#L17)
  (`g_event_ring_lock`, IRQ-saved).
- Kmem caches: per-cache `Spinlock` + `g_named_cache_registry_lock`
  ([kmem.cpp:118,280](../../../src/kernel/mm/kmem.cpp#L280)), `CacheLockGuard` is IRQ-safe.
- DMA registry: `g_dma_allocation_registry.lock`
  ([mm/dma.cpp:20,29-50](../../../src/kernel/mm/dma.cpp#L20-L50)).
- IRQ route registry, device-binding registry, PCI BAR claim registry, ARP cache:
  all use IRQ-safe `Spinlock` guards.

### 1.2 What still remains BSP-only

Concrete BSP-only callers, observed from `KASSERT_ON_BSP()` placement in source:

- All scheduler mutation: `init_tasks`, `initialize_thread_table`,
  `relink_runnable_threads`, `create_kernel_thread`, `create_user_thread`,
  `mark_thread_ready`, `block_current_thread`, `wake_blocked_thread`,
  `mark_current_thread_dying` ([thread.cpp:103-464](../../../src/kernel/proc/thread.cpp)).
- Process registry creation (`g_next_pid`, `g_kernel_process`,
  `g_process_head/tail`, `g_process_cache`).
- Page frame allocation and free paths (`PageFrameContainer` has no lock today;
  it is annotated `OS1_BSP_ONLY` and the bitmap is mutated without a guard).
- Console input queues in [console_input.cpp](../../../src/kernel/console/console_input.cpp)
  (`g_pending_line`, `g_completed_lines`, indices).
- `terminal[kNumTerminals]` and `active_terminal` selection.
- `PlatformState g_platform` (cpu list, ioapic list, mcfg, overrides).
- The non-trivial fact: **APs install a zeroed IDT.** The trampoline executes
  `lidt [P_IDT]` against `kApStartupIdtAddress`, which `clear_ap_startup_idt()`
  zero-fills before each STARTUP IPI ([cpu.cpp:43-50,200](../../../src/kernel/arch/x86_64/cpu/cpu.cpp#L43-L50)).
  After `cpu_idle_loop` runs `cli; hlt`, the AP never reloads a real IDT.
  The global `Interrupts::initialize()` and `lidt(IDT, ...)` in
  [interrupt.cpp:110-174](../../../src/kernel/arch/x86_64/interrupt/interrupt.cpp#L110-L174)
  only run on the BSP; the IDT array itself is process-wide and could in
  principle be shared, but APs never `lidt` it.
- AP timer: `lapic_init()` sets the LVT entry for the timer to
  `MASKED | PERIODIC | T_LTIMER` and writes a default initial count of 10000000
  ([lapic.cpp:67-116](../../../src/kernel/arch/x86_64/apic/lapic.cpp#L67-L116)), but
  no AP ever calls `lapic_timer_start_periodic`. `initialize_scheduler_timer`
  in [kernel_main.cpp:216-260](../../../src/kernel/core/kernel_main.cpp#L216-L260) runs
  on the BSP only.
- `g_boot_irq_thread` is a single statically-allocated `Thread` used to give the
  BSP a current_thread before `enter_first_thread` ([kernel_main.cpp:55,500-504](../../../src/kernel/core/kernel_main.cpp#L55)).
  APs have `cpu_cur()->current_thread == nullptr`.

### 1.3 Global structures and subsystems that block real SMP today

Ordered by how visibly they break under real AP scheduling:

1. **Single global runnable thread list.** `next_runnable_thread` in
   [thread_queue.cpp](../../../src/kernel/sched/thread_queue.cpp) walks `first_thread()
   ... next_thread(t)` and is called by `schedule_next` after
   `relink_runnable_threads` rewrites `next` pointers in place. There is no
   per-CPU run queue and no scheduler lock. The minute two CPUs call
   `schedule_next` concurrently this corrupts the linked list.
2. **One idle thread for the whole system.** `g_idle_thread` is set the first
   time `create_kernel_thread` runs ([thread.cpp:227-234](../../../src/kernel/proc/thread.cpp#L227-L234)).
   APs cannot share it: only one CPU can have `state == Running` on it.
3. **Page frame allocator has no lock.** Every allocation in `proc/`, `mm/`,
   `drivers/`, `platform/` flows through `page_frames`. SMP scheduling can't
   start until this is locked or made per-CPU.
4. **APs install a null IDT.** Any IRQ that lands on an AP today (which is
   possible because the IOAPIC routes ISA IRQs in `INT_LOGICAL | INT_LOWEST`
   mode with destination `0xff << 24` — see
   [ioapic.cpp:159-160](../../../src/kernel/arch/x86_64/apic/ioapic.cpp#L159-L160) — and
   would deliver to the lowest-priority CPU including parked APs) will fault.
   Today this is masked only by APs running with `cli`. As soon as APs enable
   interrupts, this becomes an immediate triple-fault risk.
5. **No IPI mechanism for the kernel.** `lapic_start_cpu` exists, but there is
   no general "send vector V to APIC ID N (or all-but-self)" helper. Cross-CPU
   wakeup, reschedule, and TLB shootdown all need this.
6. **No TLB shootdown.** `VirtualMemory::activate()` writes CR3 on the local CPU
   only. Removing or downgrading a user mapping while another CPU holds a TLB
   entry for it is silently incorrect.
7. **Wait/wakeup is registry-walking.** `wake_block_io_waiters` and
   `wake_child_waiters` walk the global thread registry under no lock. There is
   no wait-queue object; the ad-hoc completion key is `&request.completed`.
8. **`relink_runnable_threads` rebuilds `Thread::next` pointers globally.**
   Each `schedule_next` mutates these pointers; concurrent rebuilds corrupt
   the round-robin walk.
9. **Console input queue is BSP-only.** Keyboard IRQ handlers append to
   `g_pending_line` etc. with no lock. As soon as keyboard or serial IRQs can
   land on an AP, this races with BSP readers.
10. **Driver completion paths assume one waker.** `block_request_complete` in
    [storage/block_device.cpp:66-76](../../../src/kernel/storage/block_device.cpp#L66-L76)
    relies on the BSP being both submitter and (eventually) waker, and uses a
    compiler barrier rather than an atomic store.

## 2. Target end state

The end state of this plan: APs run real work, every CPU has a private
scheduler context, user processes are scheduled across all CPUs, and a basic
load balancer keeps run queues from drifting too far apart. Concretely:

- **APs execute real work.** Every booted AP installs the global IDT, runs the
  per-CPU LAPIC timer, takes its share of user threads, and can be the
  wake-up target of cross-CPU IPIs.

- **Per-CPU runtime state.** `struct cpu` grows additional CPU-local fields
  beyond `current_thread`:
  - `Thread* idle_thread` (one per CPU; replaces the global `g_idle_thread`).
  - `RunQueue runq` (per-CPU queue of `Ready` threads, see below).
  - `uint64_t timer_ticks` (per-CPU tick counter; the global `g_timer_ticks`
    becomes a derived sum or stays as a coarse "ticks-since-boot anywhere"
    metric).
  - `uint64_t reschedule_pending` (set by IPI handler, cleared at scheduling
    boundaries).
  - `Thread* irq_stack_thread` (per-CPU equivalent of `g_boot_irq_thread`).
  - lightweight scheduler counters: `enqueue_count`, `dequeue_count`,
    `idle_ticks`, `migrate_in`, `migrate_out`.

  CPU-local access stays through `cpu_cur()`, which is already routed through
  `%gs:0`. No new TLS machinery is needed.

- **Run-queue design (recommendation).** Per-CPU FIFO doubly-linked queues
  keyed off `Thread::next` (currently used for round-robin link rebuilding).
  A thread sits on at most one queue. The current `relink_runnable_threads`
  whole-registry rebuild is removed. Queue mutation requires the per-CPU
  scheduler lock (`runq.lock`). Cross-CPU enqueue happens via the wakeup IPI
  path: the wakeup-issuing CPU acquires the target CPU's `runq.lock`, links
  the thread, releases the lock, and sends `RESCHED_VECTOR` to the target.

  Justification: a single global queue would force every wakeup, every tick,
  and every block to take the same lock, which is exactly the contention
  pattern this work is trying to avoid. Per-CPU queues are also the only
  natural target for the load-balancer in step 4. The current global
  registry can keep being used for `observe`, `reaper`, and anything that
  iterates across processes/threads — those paths are not on the schedule
  hot path.

- **Full user-process SMP scheduling.** `schedule_next` becomes per-CPU. Any
  `Ready` thread can run on any CPU. There is no CPU-pinning by default.
  Affinity is supported as an optional per-thread mask but not relied on.

- **Basic load balancing.** A periodic balancer (every N timer ticks on each
  CPU, plus an explicit run on AP idle entry) compares the local run queue
  length against the global average and pulls one ready thread from the
  busiest other CPU when the local queue is empty or significantly shorter.
  No work-stealing during the schedule fast path; balancing is rare.

- **IRQ affinity / routing considerations.**
  - The HPET-calibrated LAPIC timer becomes per-CPU: each CPU runs its own
    `lapic_timer_start_periodic` after CPU bring-up. The same dynamic vector
    is used everywhere because LAPIC LVT entries are per-CPU MMIO. The PIT
    fallback remains BSP-only and IOAPIC-routed; if PIT is the source, only
    one CPU sees ticks and SMP scheduling falls back to one tick per HPET-less
    boot via IPI broadcast (see Phase 4 risks).
  - Device IRQs continue to use IOAPIC `INT_LOGICAL | INT_LOWEST` until a
    later affinity policy lands. We tighten one rule first: handlers must be
    safe on any CPU, because today's IOAPIC programming already broadcasts
    with destination `0xff << 24`.
  - MSI/MSI-X destination CPU selection becomes a per-driver knob through
    [platform/pci_msi.cpp](../../../src/kernel/platform/pci_msi.cpp); default is
    "lowest priority anycast" matching the IOAPIC default.
  - We add `IPI_RESCHED` and `IPI_TLB_SHOOTDOWN` vectors in the
    `kDynamicIrqVectorBase..kDynamicIrqVectorLimit` range allocated through
    [interrupt/vector_allocator.cpp](../../../src/kernel/arch/x86_64/interrupt/vector_allocator.cpp).
  - APs run with their own `tss.rsp0` already populated through
    `cpu_set_kernel_stack` whenever they get a current_thread; this part needs
    no change.

## 3. Synchronization primitives required

The list below names every primitive needed to take this plan to completion,
its place in the existing locking order, and the rule for when each is allowed.

### 3.1 Atomic layer

- Continue to use the GCC `__atomic_*` builtins directly. They already back
  `Spinlock` and event-ring counters. The only new requirement is to wrap the
  three operations the scheduler will need into named helpers in
  `src/kernel/sync/atomic.hpp` (new):
  - `atomic_load_acquire<T>(const volatile T*)`
  - `atomic_store_release<T>(volatile T*, T)`
  - `atomic_compare_exchange_strong<T>(volatile T*, T*, T)`
  - `atomic_fetch_add<T>(volatile T*, T)`
  - `atomic_exchange<T>(volatile T*, T)`

  These are intentionally type-parameterized wrappers and must never
  proliferate ad-hoc memory orderings. Three orderings only: `relaxed`
  (counters), `acquire`/`release` (lock-equivalent ownership transfer),
  `seq_cst` (only for the few cases where ordering across multiple atomic
  variables is the point — e.g. the IPI fence below).

### 3.2 Memory-ordering / barrier rules

x86_64 has a strong memory model: regular loads do acquire, regular stores do
release, the only reordering is `store -> load` against a different address.
Concrete project rules:

- Reads of `cpu_cur()->current_thread` are fine without explicit fences as long
  as they happen on the local CPU.
- Cross-CPU publication (e.g. enqueueing a thread on a remote run queue, then
  raising `RESCHED_VECTOR`) must follow the lock-protected pattern: take the
  target's `runq.lock`, link, release. The lock release is a release fence; the
  lock acquire on the receiver is an acquire fence. No bare publication of
  pointers across CPUs without a lock or explicit `__atomic_thread_fence`.
- The completion-flag pattern in
  [block_device.cpp:66-76](../../../src/kernel/storage/block_device.cpp#L66-L76) must
  switch to `atomic_store_release(&request.completed, true)`, and waiters must
  use `atomic_load_acquire`. The current empty `asm` barriers are insufficient
  on any non-x86 future port and are misleading even on x86 because the
  compiler cannot recognize them as ordering across atomic operations.
- TLB shootdown receivers must execute `mov %%cr3, %%rax; mov %%rax, %%cr3`
  (or a finer `invlpg`) after acknowledging the IPI, not before, so that the
  initiator's release fence happens-before the receiver's flush.

### 3.3 Spinlocks / IRQ-safe spinlocks

- `Spinlock` (existing, [sync/smp.hpp:43-89](../../../src/kernel/sync/smp.hpp#L43-L89))
  stays unchanged for code that runs only with interrupts disabled or only in
  thread context.
- For locks that may be taken from both thread context and IRQ context, the
  required idiom is the same one already used by `kmem`, `dma`, `event_ring`,
  IRQ registry, device binding, BAR claims, and ARP cache:

  ```cpp
  IrqGuard irq;
  the_lock.lock();
  // ...
  the_lock.unlock();
  ```

  This must always be wrapped in an RAII helper struct named per-subsystem so
  the ordering (IrqGuard before lock, unlock before IrqGuard destructor) is
  enforced by construction order. Several such helpers already exist; we copy
  that pattern.

- New per-CPU `runq.lock` (Spinlock, IRQ-safe). Tier 5 in the existing locking
  order.
- New global `g_page_frames_lock` (Spinlock, IRQ-safe). Tier 2.
- New `g_console_input_lock` (Spinlock, IRQ-safe). Tier 7 in the existing
  order.
- New `g_process_table_lock` (Spinlock, IRQ-safe). Tier 4.
- The thread *registry* lock (`g_thread_registry_lock`, Tier 5) is separate
  from per-CPU `runq.lock` (Tier 5). They are siblings in tier; the rule is
  "never hold registry while taking a runq, and never hold a runq while taking
  registry." Because they are co-tier, callers must pick one and never nest.

**Forbidden cases** (these must be enforced by code review and assertions
where cheap):

- Calling `kmem_cache_alloc`, `page_frames.allocate`, or `kmalloc` while
  holding *any* spinlock. The allocator's own lock is fine; the rule prevents
  inversion when the allocator someday grows reclaim hooks.
- Calling `debug()` / `write_console_line` / serial output while holding
  process, thread, scheduler, platform, or allocator locks. Console paths must
  remain reentrancy-safe but are not tested under spinlocks.
- Sending an IPI while holding any lock other than the target's own
  `runq.lock` (which is the lock the receiver will take after waking).

### 3.4 Wait queues

Today the kernel has typed wait state on the `Thread` itself
([thread.hpp:53-63](../../../src/kernel/proc/thread.hpp#L53-L63)) and registry walks
to find waiters. Under SMP this becomes a hot global walk and a wakeup-loss
risk. We introduce a `WaitQueue` object:

```cpp
struct WaitQueue {
  Spinlock lock;
  Thread* head;          // intrusive list via Thread::wait_link
  const char* name;
};
```

- `wait_queue_block_current(WaitQueue&, ThreadWaitState)` enqueues current
  thread, releases the queue lock, calls `schedule_next(false)`.
- `wait_queue_wake_one(WaitQueue&)` and `wait_queue_wake_all(WaitQueue&)` pop
  threads, re-enqueue them on their home CPU's run queue, and emit `IPI_RESCHED`.
- The `BlockIo` completion-flag pattern becomes a `WaitQueue` per outstanding
  request (or per device queue). The current address-keyed wakeup
  (`wake_block_io_waiters(uint64_t completion_flag)`) is dropped.
- The `ChildExit` and `ConsoleRead` waits become wait queues owned by the
  parent process and the console input subsystem respectively.

Tier 6 in the locking order — never call into wait queues while holding any
allocator, process, thread-registry, or run-queue lock.

### 3.5 Mutex / sleepable lock

A sleepable mutex is needed exactly when a lock holder may need to sleep
(allocator backoff, page-fault recovery, blocking I/O). For SMP enablement
itself, no new sleepable mutex is required: every scheduler-critical lock is a
spinlock and is held for bounded time. We defer `Mutex` until VFS work
introduces it.

If introduced earlier, the contract is: a `Mutex` may be acquired only from
thread context, never from IRQ context, never while holding any spinlock.
Tier 5/6 boundary.

### 3.6 Completion / event primitive

- `Completion` is a thin wrapper around `WaitQueue` plus a `bool done`. Used
  for "caller waits for one signal" patterns. Replaces the bare
  `request.completed` plus `wake_block_io_waiters` flow in
  [storage/block_device.cpp](../../../src/kernel/storage/block_device.cpp).
- `Event` (level-triggered, not auto-resetting) is the same shape with
  explicit `event_signal_all` and `event_clear`. Used by, e.g., a future
  "device ready" gate.

### 3.7 Lock-ordering and "must not sleep while holding spinlock" rules

Ordering follows the contract in
[docs/kernel/smp-synchronization.md](../../kernel/smp-synchronization.md).
The new entries fold in as:

| Tier | Existing                          | Added by this plan                       |
|------|-----------------------------------|------------------------------------------|
| 1    | Local IRQ state (IrqGuard)        | (no change)                              |
| 2    | `page_frames` lock (NEW)          | `g_page_frames_lock`                     |
| 3    | VM/address-space lock             | (used during teardown / TLB shootdown)   |
| 4    | Process table lock (NEW)          | `g_process_table_lock`                   |
| 5    | Thread registry lock + per-CPU runqueue locks (NEW) | `g_thread_registry_lock`, `cpu->runq.lock` |
| 6    | Wait-queue locks (NEW)            | per-`WaitQueue` lock                     |
| 7    | Console input lock (NEW)          | `g_console_input_lock`                   |
| 8    | Terminal/display locks            | when added                               |
| 9    | Platform/PCI/device-list locks    | (already locked)                         |
| 10   | Per-driver locks                  | (already locked)                         |

Hard rules:

- Sleeping (calling `schedule_next(false)` voluntarily) is allowed only on
  `WaitQueue::block_current` paths and must be the last thing the caller does
  while holding any lock — i.e. release queue lock, then yield.
- Spinlocks are never held across `iretq` boundaries. The trap entry asm
  [arch/x86_64/asm/multitask.asm](../../../src/kernel/arch/x86_64/asm/multitask.asm)
  unconditionally `iretq`s; if a thread is interrupted while holding a
  spinlock, the IrqGuard nesting ensures another CPU can still progress, but
  the held lock blocks rescheduling for the holder. So: keep critical sections
  short and never disable preemption while holding a spinlock.
- Cross-tier ordering is one-directional: lower tier → higher tier. A receiver
  IPI handler that needs to enqueue onto its own run queue runs at tier 5 with
  no other locks held.

## 4. Phased implementation plan

The plan is dependency-ordered. Each phase is independently mergeable. Each
phase ends with a smoke test that proves it; if the smoke fails, the phase
reverts cleanly because no later phase has shipped yet.

### Phase 0 — Locking audit and prerequisite primitives

**Goal:** Have every primitive the rest of the plan needs, without changing
runtime behavior.

Work:

- Add `src/kernel/sync/atomic.hpp` with the wrappers from §3.1.
- Convert the four registries that already ad-hoc-spinlock (event_ring, kmem,
  dma, irq_registry, device-bindings, BAR claims, ARP cache) to use an
  `IrqSpinGuard` template instead of duplicating the IrqGuard/Spinlock pattern.
  No semantic change.
- Add `WaitQueue`, `Completion` types in `src/kernel/sync/wait_queue.hpp`. Not
  yet wired in.
- Add an `Ipi` abstraction in `src/kernel/arch/x86_64/apic/ipi.{hpp,cpp}`
  exposing `ipi_send(uint8_t apic_id, uint8_t vector)` and
  `ipi_send_all_but_self(uint8_t vector)` using LAPIC ICR. Not yet used.
- Add `g_page_frames_lock`, `g_process_table_lock`,
  `g_thread_registry_lock`, `g_console_input_lock` as Spinlocks but without
  taking them yet — leave the existing `KASSERT_ON_BSP` in place. The intent
  is to make the next phase's diff small.

Risk and rollback:

- Risk: an IrqSpinGuard refactor accidentally changes lock-ordering. Mitigation:
  per-subsystem refactor PRs, identical observable behavior, host tests
  (existing) for `kmem`, `dma`, `arp_cache`, `irq_route_registry` must pass
  unchanged.
- Rollback: drop the new files; the existing primitives keep working.

Smoke: existing host tests pass. No QEMU smoke change.

### Phase 1 — Per-CPU state and per-CPU idle threads

**Goal:** Every CPU has its own idle thread, run queue (still empty), and
scheduler-local fields, but BSP scheduling is unchanged.

Work:

- Extend `struct cpu` ([cpu.hpp:51-76](../../../src/kernel/arch/x86_64/cpu/cpu.hpp#L51-L76))
  with `Thread* idle_thread`, `RunQueue runq`, `uint64_t timer_ticks`,
  `uint64_t reschedule_pending`, scheduler counters. Update the static
  offset asserts at the bottom of the header — the assembly multitask code
  reads `CPU_CURRENT_THREAD` and `CPU_TSS_RSP0` directly, so any new fields
  must come *after* `tss` to preserve `tss_rsp0_offset` ([cpu.hpp:73](../../../src/kernel/arch/x86_64/cpu/cpu.hpp#L73)).
- Replace `g_idle_thread` ([thread.cpp:22](../../../src/kernel/proc/thread.cpp#L22))
  with `cpu_cur()->idle_thread`. Provide `idle_thread_for_cpu(cpu*)`. The
  existing `idle_thread()` function returns `cpu_cur()->idle_thread`.
- During boot, BSP creates its own idle thread (existing path). Each AP, after
  `cpu_init()`, calls a new `ap_create_idle_thread()` that allocates one
  kernel thread with `kernel_idle_thread` as its entry — this still uses the
  BSP-locked thread cache, so APs must be still parked at this point. We
  pre-create AP idle threads from the BSP before APs leave the trampoline:
  for each `cpu*` other than `g_cpu_boot`, call `create_kernel_thread(...,
  kernel_idle_thread, ...)` with that AP as the assigned home and stash the
  returned thread in `cpu->idle_thread`. The AP later resumes onto it.

Risk and rollback:

- Risk: layout changes in `struct cpu` desync from `cpu.inc`. Mitigation: the
  existing `CPU_STATIC_ASSERT` macros must be extended for any newly-relied-on
  offset, and `multitask.asm` must keep working without source changes.
- Risk: BSP creates AP idle threads while APs are running, racing the still-
  BSP-only thread registry. Mitigation: AP idle threads are created *before*
  `cpu_boot_others` is called (i.e. before any AP executes any C++).
- Rollback: revert the struct changes; `idle_thread()` returns the old global.

Smoke: BSP behavior unchanged. New host test: a stub `cpu_cur()` returns
distinct `cpu*` records, each with its own idle thread pointer. QEMU smoke:
`observe cpus` shows the same parked-AP state as today — the code paths for
APs are still `cli;hlt`.

### Phase 2 — APs install the global IDT and run idle

**Goal:** Each AP runs through the same IDT as the BSP, has interrupts enabled,
and parks on its own idle thread instead of `cpu_idle_loop`.

Work:

- Move `Interrupts::initialize()` such that the IDT itself is built once on the
  BSP (already true), and APs only `lidt` the same `IDT[]` array. Add
  `cpu_load_idt()` (or fold into `cpu_init()`). Today `cpu_init()` runs
  before `Interrupts::initialize()` on the BSP; AP ordering must be: trampoline
  → cpu_init → lapic_init → ioapic_init → cpu_load_idt → switch to per-CPU idle
  thread → `sti; hlt`-as-a-thread.
- Replace `cpu_idle_loop` ([cpu.cpp:16-26](../../../src/kernel/arch/x86_64/cpu/cpu.cpp#L16-L26))
  with a call sequence that publishes `cpu_cur()->idle_thread` as
  `current_thread`, sets TSS RSP0, and `sti; hlt`s in a loop — i.e. the same
  body as `kernel_idle_thread` ([sched/idle.cpp](../../../src/kernel/sched/idle.cpp))
  but reached without a context switch (the AP arrives on the trampoline kernel
  stack, then we explicitly call `enter_first_thread(idle_thread,
  idle_thread->kernel_stack_top)` from the AP `init()` path).
- Initialize `cpu_cur()->id` to the local APIC ID via `lapic[ID] >> 24` so
  `current_cpu_id()` reports the right value during early AP execution.
- The AP's `cpu_init` already wires syscall MSRs; that's harmless because no
  user code lands on the AP yet.

Risk and rollback:

- Risk: an IRQ targets a parked-but-IF=1 AP before the IDT is loaded. Mitigation:
  keep `cli` from trampoline entry until *after* `cpu_load_idt` and the per-CPU
  idle thread is the current_thread. Only then `sti`.
- Risk: IOAPIC delivers a legacy IRQ to the AP because of `INT_LOWEST`
  ([ioapic.cpp:159](../../../src/kernel/arch/x86_64/apic/ioapic.cpp#L159)). All ISA
  IRQ vectors already have a registered handler on the BSP because
  `Interrupts::initialize` runs before AP scheduling; the handler is
  CPU-agnostic for trivial vectors (LAPIC error, spurious). Risky vectors —
  PIT IRQ and keyboard IRQ — must be re-routed to BSP only via per-vector
  IOAPIC redirection masks before APs come up. This is a code change in
  `ioapic_enable_gsi` to allow a destination override; for Phase 2 we
  hard-pin all device IRQs to the BSP APIC ID until Phase 6.
- Risk: `cpu_cur()->current_thread` reads still go through `%gs:0` after the
  IDT is loaded; the trampoline sets up GS, but only `KernelGsBase` (MSR
  0xC0000101) is written, not `GsBase` (0xC0000100). Today the kernel only
  uses GS in kernel mode and never `swapgs`es from kernel→kernel transitions,
  so `KernelGsBase` is currently irrelevant for kernel-mode reads. Mitigation:
  in `cpu_init` write *both* GsBase and KernelGsBase to the same `cpu*`. This
  is one extra `wrmsr`.
- Rollback: AP path returns to `cpu_idle_loop` (`cli; hlt`); BSP unaffected.

Smoke (QEMU, `-smp 2..4`):
- `observe cpus` shows all CPUs as `BOOTED`. Today they already do — what's
  new is the AP runs with IF=1.
- A new event `OS1_KERNEL_EVENT_AP_ONLINE` is recorded once per AP from the AP
  itself, proving the event ring lock is good enough.

### Phase 3 — Per-CPU LAPIC timer and AP timer ticks

**Goal:** Every CPU services its own scheduler tick. The scheduler is still
BSP-only, but APs now generate ticks they can use later.

Work:

- Refactor `initialize_scheduler_timer` ([kernel_main.cpp:216-260](../../../src/kernel/core/kernel_main.cpp#L216-L260))
  into two halves:
  - BSP-only: HPET calibration, vector allocation, source selection.
  - Per-CPU: `lapic_timer_start_periodic(vector, initial_count)` — runs on BSP
    after calibration, and on each AP at the end of the Phase-2 idle setup.
- In the IRQ dispatch path ([core/irq_dispatch.cpp](../../../src/kernel/core/irq_dispatch.cpp)):
  - `g_timer_ticks` becomes per-CPU `cpu_cur()->timer_ticks`. The observable
    `g_timer_ticks` exposed in observe records becomes a sum or stays as the
    BSP value with a new per-CPU snapshot. Recommendation: keep `g_timer_ticks`
    as "boot tick anywhere" via an atomic increment, and add per-CPU
    `cpu->timer_ticks` for scheduling decisions.
  - `console_input_poll_serial()` is BSP-only (it touches the BSP-only console
    queue). Gate it on `cpu_cur() == g_cpu_boot`.

Risk and rollback:

- Risk: PIT fallback is the active source. PIT is one external IRQ delivered
  by the IOAPIC. Under Phase 2's "pin device IRQs to BSP" rule, only the BSP
  ticks. Under PIT, AP scheduling cannot work without an IPI broadcast tick.
  Mitigation: refuse to enable AP scheduling under PIT; record a kernel event
  `OS1_KERNEL_EVENT_TIMER_SOURCE` with a new flag bit `…_AP_TICKS_DISABLED`
  and keep APs idle. SMP scheduling requires HPET-calibrated LAPIC.
- Risk: LAPIC timer divisor is per-CPU; a CPU offline/online flap could leave
  a stale TICR. Mitigation: each AP re-runs `lapic_timer_start_periodic` from
  scratch on every entry into Phase-2 idle.
- Rollback: AP-side `lapic_timer_start_periodic` call is gated by a build flag
  and a runtime `g_smp_ap_timer_enabled` boolean; flipping it off restores
  Phase-2 behavior (AP idle, no AP ticks).

Smoke (QEMU, `-smp 4`, HPET available): a new event ring counter records
`OS1_KERNEL_EVENT_AP_TICK` once per AP within ~100 ms of bring-up, proving the
LAPIC timer fired on every AP.

### Phase 4 — Reschedule IPI path and per-CPU scheduler entry

**Goal:** A scheduler tick on any CPU calls `schedule_next` for that CPU.
Wakeups from one CPU can target another CPU. APs still don't pull live work
yet because no thread is enqueued on their run queue, but the plumbing is
done.

Work:

- Allocate `RESCHED_VECTOR` from `vector_allocator` and wire it in `Ipi`.
- `irq_dispatch.cpp` recognizes `RESCHED_VECTOR`, sets
  `cpu_cur()->reschedule_pending = 1`, EOIs, and triggers the same
  end-of-IRQ scheduling check as the timer tick.
- Make `schedule_next` per-CPU:
  - Replace `relink_runnable_threads` global rebuild with per-CPU run-queue
    pop.
  - The "current thread" passed to the round-robin walk is `cpu_cur()->current_thread`.
  - The fallback when nothing is ready is `cpu_cur()->idle_thread`.
- Convert the wake paths:
  - `mark_thread_ready` and `wake_blocked_thread` in
    [thread.cpp:316-411](../../../src/kernel/proc/thread.cpp#L316-L411) take a target
    `cpu*` (default: `cpu_cur()`), enqueue on that CPU's run queue under
    `cpu->runq.lock`, and emit a `RESCHED_VECTOR` IPI to that CPU only if
    `cpu != cpu_cur()`.
  - `wake_block_io_waiters` becomes a `Completion::signal_all` call once Phase
    5 lands; for now it iterates the global registry but routes each wakeup
    through the per-CPU enqueue helper.
- Keep the global `g_thread_registry_lock` for *registry mutation only*
  (create/destroy/iteration). Add it under `KASSERT_ON_BSP`'s old guards.

Risk and rollback:

- Risk: cross-CPU enqueue into a remote run queue races with the remote CPU's
  schedule_next pop. Mitigation: per-CPU `runq.lock` is the canonical SMP
  exercise; both sides take it.
- Risk: a thread is enqueued on CPU A's run queue but currently `Running` on
  CPU B (because B preempted it and forgot to clear). This is exactly the
  "double-enqueue" bug. Mitigation: enqueue checks `state == Ready` and trips
  an assertion if `Running` from a different CPU. The transition
  `Running → Ready` always happens on the running CPU at preemption.
- Risk: deadlock between two CPUs each taking the other's runq.lock.
  Mitigation: a CPU only takes another CPU's `runq.lock` for *enqueue*, never
  for arbitrary inspection. Migration in Phase 7 takes both with a fixed
  numerical ordering on `cpu_id`.
- Rollback: `g_smp_ap_scheduling_enabled` boolean gates AP participation; off
  reverts to "AP idle thread runs forever," which is exactly Phase 3's
  behavior.

Smoke: a kernel thread spawned on CPU 0 calls `schedule_next(true)` and lands
back on CPU 0 (no AP work yet). A test path explicitly enqueues a kernel
thread on CPU 1's run queue via the new helper and emits `RESCHED_VECTOR`;
event ring records the IPI receipt and the SCHED_TRANSITION on CPU 1.

### Phase 5 — Wait queues and lock conversion of BSP-only globals

**Goal:** Replace the globals that the runnable scheduler now races against.

Work:

- `PageFrameContainer::allocate/free/reserve` paths in
  [src/kernel/mm/page_frame.cpp](../../../src/kernel/mm/page_frame.cpp) take
  `g_page_frames_lock` under an IrqSpinGuard.
- Process table mutation takes `g_process_table_lock`. Iteration paths used by
  `observe` and `reaper` take it as a reader (or as the same spinlock for
  simplicity until contention is measured).
- Thread registry mutation takes `g_thread_registry_lock`. Iteration paths in
  `wake_block_io_waiters`, `wake_child_waiters`, `first_runnable_user_thread`,
  `runnable_thread_count` take it.
- Console input takes `g_console_input_lock`. The keyboard IRQ handler still
  runs on the BSP under Phase-2's pinning rule, so this is preparation, but it
  means `block_current_thread_on_console_read` and `try_complete_console_read`
  can be safe under SMP.
- Convert `BlockIoWaitState` flow:
  - `BlockRequest` carries a `Completion` (or owns a `WaitQueue`).
  - `block_request_complete` calls `completion.signal()` instead of writing
    `request.completed = true`.
  - `block_request_wait` calls `completion.wait()`.
- Convert `ChildExitWaitState` to a per-process `WaitQueue` woken on
  `mark_current_thread_dying`.
- Drop every `KASSERT_ON_BSP()` whose protected state is now lock-protected.
  The remaining `KASSERT_ON_BSP()` calls (boot-only init, BSP-pinned device
  IRQ paths) are explicitly preserved.

Risk and rollback:

- Risk: a missed callsite leaves an unprotected mutation. Mitigation: the
  refactor must compile-fail when a BSP-only annotation is removed without
  adding a lock — encode this by replacing `OS1_BSP_ONLY` on the four
  converted globals with a `OS1_LOCKED_BY(name)` annotation that is purely
  documentation but easy to grep.
- Risk: lock-ordering inversion in code that already takes another lock and
  then wakes a waiter. Mitigation: WaitQueue wakeup never holds the queue lock
  while calling into the run-queue path. Wakeup steps are: take queue lock,
  unlink waiter, release queue lock, call enqueue (which takes runq.lock).
- Rollback: each conversion is independent; revert per-subsystem.

Smoke: existing block-I/O smoke (`run_virtio_blk_threaded_smoke`) still
passes. New host test: a `WaitQueue` with N waiters and one signal wakes
exactly one; with broadcast, wakes all.

### Phase 6 — Kernel-thread SMP first

**Goal:** Kernel-only threads can run on any CPU. User threads still pinned to
BSP.

Work:

- Spawn additional kernel-only worker threads (e.g. a deferred-reaper, a
  net-RX dispatch thread) and let them be enqueued on AP run queues.
- `create_kernel_thread` now picks a home CPU (round-robin across booted
  CPUs) and enqueues on that CPU's run queue.
- User threads still go to BSP only: `create_user_thread` enqueues on
  `g_cpu_boot->runq` regardless of caller CPU.
- Re-enable per-CPU `console_input_poll_serial` *only* on the BSP (already
  the case) and validate that a kernel-thread on an AP can call
  `kernel_event::record` safely (event ring is already locked).

Risk and rollback:

- Risk: a kernel thread on an AP touches BSP-only state by accident.
  Mitigation: every remaining `OS1_BSP_ONLY` annotation is reviewed; anything
  reachable from a kernel thread must be lock-protected by Phase 5.
- Risk: cross-CPU TLB shootdown not yet implemented. Kernel threads only use
  the kernel address space (CR3 = `g_kernel_root_cr3`); the kernel address
  space isn't unmapped during normal runtime, so this risk is low until user
  threads enter the picture in Phase 7.
- Rollback: route kernel threads back to BSP-only enqueue.

Smoke: a "ping" kernel thread on each AP increments a per-CPU counter; the
shell `events` command shows ticks from each CPU.

### Phase 7 — User-process SMP next

**Goal:** User threads run on any CPU.

Work:

- `create_user_thread` and `mark_thread_ready` for user threads enqueue on the
  least-loaded CPU's run queue (initially round-robin, balanced in Phase 8).
- TLB shootdown:
  - Add `IPI_TLB_SHOOTDOWN` vector. `VirtualMemory::unmap` /
    `VirtualMemory::protect` paths that downgrade or remove a user mapping
    must broadcast a shootdown to every CPU whose `current_thread` belongs to
    a process sharing that CR3 (or to all CPUs as a coarse first cut).
  - Receivers reload CR3 (or `invlpg` on the affected page if the shootdown
    payload carries the address) before EOI.
- TLB shootdown caveat: today user address spaces are not modified after
  process creation. The shootdown is preparation for `mmap`, `munmap`,
  page-out, copy-on-write — none of which exist yet. For Phase 7 we ship the
  IPI mechanism and use it on `reap_process` (which destroys the address
  space).
- `cpu_set_kernel_stack` is already per-CPU. The
  [arch/x86_64/cpu/syscall.cpp](../../../src/kernel/arch/x86_64/cpu/syscall.cpp)
  SYSCALL entry uses `swapgs` + `gs:CPU_TSS_RSP0` for stack switching, and
  Phase 2's GsBase fix ensures correctness when a user thread enters via
  syscall on an AP.
- Console-read waiters: `block_current_thread_on_console_read` already targets
  the per-process `WaitQueue` after Phase 5. The keyboard IRQ still runs on
  BSP only, but the BSP wakeup IPI now reaches the user thread on whatever
  CPU it last ran on.

Risk and rollback:

- Risk: SYSCALL/SYSRET path on AP uses uninitialized TSS. Mitigation: the AP
  path in Phase 2 calls `cpu_init()` which loads the TSS descriptor, and
  `cpu_set_kernel_stack` updates RSP0 every time `set_current_thread` runs.
- Risk: shootdown storm during process exit. Mitigation: shootdown only when
  the destroyed CR3 was actually loaded on a remote CPU; check `cpu->current_thread->address_space_cr3`
  remotely under that CPU's runq lock.
- Risk: `g_kernel_root_cr3` cached on AP idle thread; CR3 reload during
  shootdown is wasteful if the AP is idle. Mitigation: skip shootdown for CPUs
  whose `current_thread == cpu->idle_thread` and whose
  `idle_thread->address_space_cr3 == g_kernel_root_cr3`.
- Rollback: re-pin user threads to BSP via the `g_smp_user_threads_enabled`
  switch.

Smoke: `observe cpus` shows different `current_pid` values across CPUs while
two user processes run busy-yield.

### Phase 8 — Basic load balancing

**Goal:** Run-queue lengths converge over time.

Work:

- Each CPU at every Nth timer tick (e.g. N=64, ~64 ms at 1000 Hz) computes
  `local_count = cpu->runq.length` and `global_avg = sum(all_runqs) /
  ncpu`. If `local_count + 1 < global_avg`, walk the other CPUs in fixed
  order, find the one with the largest `runq.length`, and pull one ready
  thread (the tail) under both `runq.lock`s with the lower `cpu_id` taken
  first. Migrating a thread takes the global-avg snapshot under a relaxed
  read; precision is not the goal.
- An idle AP whose run queue has been empty for ≥ M consecutive ticks calls
  the same balancer immediately (don't wait N ticks).
- Affinity (optional): a `Thread::affinity_mask` field. The balancer respects
  it. Default mask is "any CPU."

Risk and rollback:

- Risk: ping-pong migration. Mitigation: a thread migrated within the last
  K ticks is not re-migrated. Track in `Thread::last_migration_tick`.
- Risk: lock-order violation when two CPUs balance simultaneously and pick
  each other. Mitigation: numeric `cpu_id` ordering; the lower id is acquired
  first.
- Risk: unfair starvation of CPU 0 (which also handles all device IRQs under
  Phase 2's pin-to-BSP rule). Mitigation: the balancer uses
  `runq.length`, not "interrupt time," so device IRQs don't bias it.
- Rollback: `g_smp_balancer_enabled = false` reverts to round-robin home-CPU
  assignment.

Smoke: spawn 4×N user yield threads on a 4-CPU QEMU instance; after 1 second,
no run queue contains more than `(N + 1)` threads.

## 5. Testing

### 5.1 Host tests (`tests/host/`)

The host harness in [tests/host/CMakeLists.txt](../../../tests/host/CMakeLists.txt)
already builds `kmem`, `dma`, `arp_cache`, `irq_route_registry`,
`irq_vector_allocator`, `event_ring`, and others. New host tests:

| Phase | Test | Asserts |
|------|------|---------|
| 0 | `atomic_tests.cpp` | `atomic_load_acquire`/`store_release`/`compare_exchange` round-trip; `atomic_fetch_add` linear under concurrent threads. |
| 0 | `wait_queue_tests.cpp` | enqueue/dequeue order; `signal_one` wakes one; `signal_all` wakes all; concurrent waiters with mutex lock simulating Spinlock. |
| 0 | `completion_tests.cpp` | one-shot signal pattern; double-signal idempotent; wait before signal and wait after signal both succeed. |
| 1 | `cpu_record_tests.cpp` | `cpu_alloc` returns distinct page-aligned records; `cpu_cur()` stub returns the configured record under `OS1_HOST_TEST`. |
| 4 | `runqueue_tests.cpp` | enqueue/dequeue order; FIFO under stress; cross-CPU enqueue path delivers exactly once. |
| 5 | `lock_order_tests.cpp` | static-list test that names the locks taken by representative kernel paths; fails if a lower-tier lock is taken inside a higher-tier one. |
| 6 | `kernel_thread_dispatch_tests.cpp` | `create_kernel_thread` round-robins across N stub CPUs; respects affinity. |
| 7 | `tlb_shootdown_tests.cpp` | shootdown payload assembly: which CPUs need invalidation given a CR3 + page set; never broadcasts to CPUs not running a matching CR3. |
| 8 | `load_balancer_tests.cpp` | given run-queue lengths, picks correct donor; respects last-migration-tick; respects affinity mask; no oscillation. |

The "scheduler primitives" host test family covers `next_runnable_thread`
behavior under per-CPU mode by stubbing `cpu_cur()` to return distinct CPUs.
The existing `task_registry_tests.cpp` is extended for the locked registry.

### 5.2 QEMU smoke tests (incremental milestones)

Each smoke is a separate boot scenario in the root `CMakeLists.txt`. They
follow the existing pattern (`smoke_*` targets) and assert observable kernel
events.

| Smoke | Asserts | Phase |
|-------|---------|-------|
| `smoke_smp_ap_online` | `observe cpus` reports `BOOTED` for `-smp 4`; `events` ring contains 3× `OS1_KERNEL_EVENT_AP_ONLINE`. | 2 |
| `smoke_smp_ap_idle` | After 100 ms, each AP idle thread is `Running` per `observe cpus`; no panics. | 2 |
| `smoke_smp_ap_ticks` | `events` ring records timer ticks attributed to each AP CPU id. | 3 |
| `smoke_smp_kernel_work` | Per-CPU "ping" kernel thread increments per-CPU counters; `events` shows N kernel threads with N distinct CPUs. | 6 |
| `smoke_smp_user_work` | Two `/bin/yield` user processes with `-smp 4`; `observe cpus` shows them with different CPU ids; both reach exit. | 7 |
| `smoke_smp_cross_cpu_wakeup` | A user thread blocks on console read; keyboard input on BSP wakes it; `events` shows `RESCHED_VECTOR` IPI from BSP to the AP that resumes. | 7 |
| `smoke_smp_irq_routed_to_ap` | Reroute a single MSI vector (e.g. virtio-blk completion) to a specific AP via the per-vector affinity hook; `events` shows `OS1_KERNEL_EVENT_IRQ` with that CPU id. | 7-8 |
| `smoke_smp_balance` | Spawn 16 yield processes on a `-smp 4` instance; after 500 ms, max run-queue length minus min is ≤ 1. | 8 |

The existing `smoke_all` aggregator adds the new smokes one by one as each
phase merges.

### 5.3 Specific assertions for race-prone paths

- Event ring entries record `cpu` field today
  ([event_ring.cpp:34-37](../../../src/kernel/debug/event_ring.cpp#L34-L37)). Smokes
  must inspect this field; do not rely on serial log ordering.
- `observe cpus` returns one record per booted CPU with `current_pid` /
  `current_tid` populated ([syscall/observe.cpp:316-335](../../../src/kernel/syscall/observe.cpp#L316-L335)).
  Use it as the cross-CPU truth source.

## 6. Observability

The kernel event ring already exists with 256 records and per-event CPU id
([event_ring.hpp,cpp](../../../src/kernel/debug/event_ring.cpp), backed by the
`OS1_OBSERVE_EVENTS` snapshot). The work below extends it for SMP visibility
without changing its lock or shape.

### 6.1 New kernel events (extend `enum` in
[src/uapi/os1/observe.h](../../../src/uapi/os1/observe.h)):

| Name | Args | When |
|------|------|------|
| `OS1_KERNEL_EVENT_AP_ONLINE` | `arg0=apic_id` | Each AP after IDT load and per-CPU idle entry. |
| `OS1_KERNEL_EVENT_AP_TICK` | `arg0=apic_id, arg1=local_tick_count` | Once per second per CPU (rate-limited). |
| `OS1_KERNEL_EVENT_IPI_RESCHED` | `arg0=src_apic, arg1=dst_apic` | Emit at IPI send and at IPI receive (`flags = BEGIN/SUCCESS`). |
| `OS1_KERNEL_EVENT_IPI_TLB_SHOOTDOWN` | `arg0=src_apic, arg1=dst_apic_mask, arg2=cr3` | At send and at acknowledge. |
| `OS1_KERNEL_EVENT_THREAD_MIGRATE` | `arg0=src_cpu, arg1=dst_cpu, arg2=tid` | When the balancer pulls a thread. |
| `OS1_KERNEL_EVENT_RUNQ_DEPTH` | `arg0=cpu, arg1=depth` | Sampled at each balance check (rate-limited). |

Existing `OS1_KERNEL_EVENT_SCHED_TRANSITION` already records old/new
pid/tid — the per-CPU CPU id field is already present in the record. No
schema change needed there.

### 6.2 New observe records / fields

`Os1ObserveCpuRecord` ([observe.h:100-107](../../../src/uapi/os1/observe.h#L100-L107))
gains:

- `uint32_t runq_depth` — current run-queue length.
- `uint64_t timer_ticks` — per-CPU local tick count.
- `uint64_t idle_ticks` — ticks the CPU has been on its idle thread.
- `uint64_t migrate_in` / `uint64_t migrate_out` — balance counters.

`Os1ObserveSystemRecord` keeps the global `tick_count` field but it now
becomes "any-CPU monotonic" — explicitly documented.

A new `OS1_OBSERVE_IRQ_AFFINITY` snapshot may be added later if per-vector
affinity tuning becomes a feature; for the SMP enablement work itself we route
every device IRQ to the BSP and re-route ad-hoc.

### 6.3 Counters

Bare per-CPU counters in `struct cpu` (read by `observe cpus`):

- `enqueue_count`, `dequeue_count`, `idle_ticks`, `migrate_in`, `migrate_out`,
  `ipi_received`, `ipi_sent`.

Bare global counters (under their respective locks):

- `g_thread_registry_lock_contention` — non-zero on spin retries (sampled, not
  precise).
- `g_page_frames_lock_contention` — same idea.

The existing `kmem` per-cache counters already provide allocator-level
contention visibility; no new fields needed there.

### 6.4 Shell coverage

The shell programs in [src/user/programs/](../../../src/user/programs) gain (or
extend) commands:

- `cpu` — already exists; show new run-queue and migration fields.
- `events` — already exists; new event types are decoded by the user-side
  formatter.

## 7. Concrete recommendation

### 7.1 Best scheduler structure for `os1`

**Per-CPU FIFO run queue, no global queue, optional affinity, periodic
work-pull balancer.** Rationale:

- The current scheduler is registry-walking round-robin
  ([thread_queue.cpp](../../../src/kernel/sched/thread_queue.cpp)); per-CPU FIFO is
  the closest evolution that doesn't require redesigning blocking and waking
  paths. Each thread has at most one "next" pointer; that pointer becomes the
  run-queue link, and the global registry retains its `registry_next` for
  iteration.
- A single global queue would force every wakeup, every tick, and every block
  to take one lock. That contention pattern is the exact one this work is
  trying to break.
- Work-pulling load balancing (Phase 8) is preferable to work-stealing during
  the schedule fast path because the schedule path must remain bounded and
  predictable while the project still has weak preemption semantics.
- Priority/nice/CFS-style fair scheduling is **out of scope**. `os1`'s
  workloads don't justify the complexity yet, and the project's stated
  preference is "clarity over cleverness." Add priorities later if and when a
  workload demands it.

### 7.2 What to implement first

The dependency-ordered first three steps:

1. Phase 0 (atomic wrappers, `WaitQueue` skeleton, `Ipi` skeleton, deferred
   per-subsystem locks). Zero behavior change. Lands the vocabulary.
2. Phase 1 (per-CPU `struct cpu` extensions, per-CPU idle thread). Still
   BSP-only at runtime. Lands the storage.
3. Phase 2 (APs install IDT, run their own idle thread with IF=1). The first
   real SMP runtime change. Smoke `smoke_smp_ap_online` and
   `smoke_smp_ap_idle` ship with this phase.

### 7.3 What to defer

- **Sleepable `Mutex`.** Not needed for the spinlock-only scheduler. Land it
  when VFS introduces the first naturally sleepable lock.
- **Priority scheduling, fair scheduling, deadline scheduling.** Out of
  scope. Add when a workload requires it.
- **CPU affinity policy.** Ship the field, default it to "any CPU," do not
  expose in user ABI yet.
- **Threaded IRQs.** Out of scope. Device IRQs continue to run in interrupt
  context.
- **Cross-CPU console input.** Keyboard IRQ stays pinned to BSP through Phase
  7; revisit when the descriptor model lands and console reads become a
  per-process resource.
- **Stop-the-world safepoints.** Not needed for SMP enablement. TLB
  shootdown handles the only correctness case (address-space teardown); other
  uses are not yet present.
- **Hot-unplug of CPUs.** Out of scope. CPUs are booted once at boot and never
  removed.

### 7.4 Likely files / subsystems to change

| File / subsystem | What changes | Phase |
|---|---|---|
| [src/kernel/sync/smp.hpp](../../../src/kernel/sync/smp.hpp) | `OS1_BSP_ONLY` becomes documentation-only; new `IrqSpinGuard` template. | 0 |
| `src/kernel/sync/atomic.hpp` (new) | Atomic wrappers. | 0 |
| `src/kernel/sync/wait_queue.hpp/.cpp` (new) | `WaitQueue`, `Completion`. | 0 |
| `src/kernel/arch/x86_64/apic/ipi.hpp/.cpp` (new) | `ipi_send`, `ipi_send_all_but_self`. | 0 |
| [src/kernel/arch/x86_64/cpu/cpu.hpp](../../../src/kernel/arch/x86_64/cpu/cpu.hpp) | `struct cpu` gains run queue, idle thread, timer state. Static asserts updated. | 1 |
| [src/kernel/arch/x86_64/cpu/cpu.cpp](../../../src/kernel/arch/x86_64/cpu/cpu.cpp) | `init()` AP path loads IDT, switches to per-CPU idle thread, enables IF, starts LAPIC timer. `cpu_init` writes both GsBase and KernelGsBase. | 2-3 |
| [src/kernel/sched/thread_queue.cpp](../../../src/kernel/sched/thread_queue.cpp) | Replaced: per-CPU FIFO run queue. | 4 |
| [src/kernel/sched/scheduler.cpp](../../../src/kernel/sched/scheduler.cpp) | Reads from `cpu_cur()->runq`. Idle fallback uses `cpu_cur()->idle_thread`. | 4 |
| [src/kernel/sched/idle.cpp](../../../src/kernel/sched/idle.cpp) | Used as the entry function for every per-CPU idle thread; no behavior change. | 1 |
| [src/kernel/proc/thread.cpp](../../../src/kernel/proc/thread.cpp) | Drop `g_idle_thread`; many `KASSERT_ON_BSP` removals as locks land. Wake paths target a `cpu*`. | 4-5 |
| [src/kernel/proc/process.cpp](../../../src/kernel/proc/process.cpp) | Locked registry. | 5 |
| [src/kernel/proc/reaper.cpp](../../../src/kernel/proc/reaper.cpp) | Issues TLB shootdown when a user CR3 is destroyed. | 7 |
| [src/kernel/mm/page_frame.cpp](../../../src/kernel/mm/page_frame.cpp) | Locked allocator. | 5 |
| [src/kernel/mm/virtual_memory.cpp](../../../src/kernel/mm/virtual_memory.cpp) | Unmap/protect paths trigger TLB shootdown via IPI. | 7 |
| [src/kernel/storage/block_device.cpp](../../../src/kernel/storage/block_device.cpp) | Replaces ad-hoc completion flag with `Completion`. | 5 |
| [src/kernel/console/console_input.cpp](../../../src/kernel/console/console_input.cpp) | Locked queue; keyboard IRQ stays BSP-pinned. | 5 |
| [src/kernel/core/irq_dispatch.cpp](../../../src/kernel/core/irq_dispatch.cpp) | Per-CPU `g_timer_ticks`; reschedule IPI handling; `console_input_poll_serial` BSP-gated. | 3-4 |
| [src/kernel/core/kernel_main.cpp](../../../src/kernel/core/kernel_main.cpp) | `initialize_scheduler_timer` split into BSP-once and per-CPU-each. AP idle thread pre-creation moved here. | 1-3 |
| [src/kernel/arch/x86_64/apic/lapic.cpp](../../../src/kernel/arch/x86_64/apic/lapic.cpp) | `lapic_init` already runs per-CPU; gain explicit `lapic_send_ipi(apic_id, vector)` if not folded into the new `Ipi` module. | 0 |
| [src/kernel/arch/x86_64/apic/ioapic.cpp](../../../src/kernel/arch/x86_64/apic/ioapic.cpp) | `ioapic_enable_gsi` gains an optional destination override for BSP-pinning device IRQs. | 2 |
| [src/kernel/platform/topology.cpp](../../../src/kernel/platform/topology.cpp) | Pre-creates AP idle threads alongside `cpu_alloc`. | 1 |
| [src/uapi/os1/observe.h](../../../src/uapi/os1/observe.h) | New event types; new `Os1ObserveCpuRecord` fields. ABI-version-bumped if shape changes. | 6-8 |
| [src/kernel/syscall/observe.cpp](../../../src/kernel/syscall/observe.cpp) | Populate new CPU fields. | 6-8 |
| [src/kernel/debug/event_ring.cpp](../../../src/kernel/debug/event_ring.cpp) | No structural change; lock already SMP-safe. | — |
| [tests/host/](../../../tests/host) | New test files per §5.1. | 0-8 |
| Root `CMakeLists.txt` | New `smoke_smp_*` targets per §5.2. | 2-8 |

### 7.5 Mismatches between docs and code, called out

While reading the source for this design, the following doc/code mismatches
were observed (also flagged in
[docs/archive/reviews/2026-05-05-review.md](../reviews/2026-05-05-review.md), but worth carrying here
because they affect SMP planning):

- [docs/ARCHITECTURE.md](../../ARCHITECTURE.md) describes process and thread tables
  as fixed arrays. Source has dynamic `kmem`-backed registries with
  `registry_next` linkage ([proc/process.cpp](../../../src/kernel/proc/process.cpp),
  [proc/thread.cpp](../../../src/kernel/proc/thread.cpp)). The SMP plan is built
  against the dynamic registry shape.
- [docs/ARCHITECTURE.md](../../ARCHITECTURE.md) and
  [GOALS.md](../../../GOALS.md) describe the BSP timer as PIT. Source prefers an
  HPET-calibrated LAPIC timer ([core/kernel_main.cpp:216-260](../../../src/kernel/core/kernel_main.cpp#L216-L260))
  and falls back to PIT only when HPET is absent. The phased plan above
  exploits the LAPIC timer being per-CPU.
- [docs/kernel/smp-synchronization.md](../../kernel/smp-synchronization.md)
  predates the additional spinlocks that have since landed in `kmem`,
  `dma`, `irq_route_registry`, device-binding registry, BAR claim registry,
  `arp_cache`, and `event_ring`. Those subsystems are already SMP-safe in
  source; the contract should be amended after Phase 0 to reflect that.

## 8. Open questions

These are not blockers for Phase 0–2 but should be resolved before Phase 7:

- **PIT-only fallback policy.** Refuse SMP scheduling? Broadcast PIT ticks via
  IPI? Recommendation: refuse, log, run BSP-only. Cheap and predictable.
- **Affinity ABI.** Expose to user space at all, or kernel-only initially?
  Recommendation: kernel-only until the descriptor model lands.
- **Shootdown granularity.** Full CR3 reload vs `invlpg` per page? Phase 7
  starts with full CR3 reload because `reap_process` destroys the whole
  address space; finer shootdown waits for `mmap`/`munmap`.
- **AP halt path under `OS1_KERNEL_EVENT_KMEM_CORRUPTION`.** Today the BSP
  halts. Under SMP, must broadcast NMI or `IPI_HALT`. Add to Phase 0 if it's
  cheap; otherwise tackle in Phase 5 alongside other cross-CPU semantics.
