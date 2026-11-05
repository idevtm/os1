# SMP Enablement Implementation Review - 2026-05-05

Source checked against [2026-05-05-phased-smp-enablement.md](../kernel/2026-05-05-phased-smp-enablement.md)
using the recommended per-CPU run-queue/load-balancer option.

> Status update: all findings in this post-implementation review were addressed
> by the follow-up work called out in [2026-05-05-review-2.md](2026-05-05-review-2.md).
> Keep this file as the historical punch-list that drove commit `61bbb4f` and
> the subsequent documentation refresh.

## Findings

1. **BIOS balance smoke is not deterministic.**

   `smoke_balance_bios` can time out after spawning all 16 balance workers without
   ever printing `[user/balancecheck] runq delta=` or the shell completion marker.
   The smoke program waits for every worker to complete before printing its
   balance marker, and each worker originally yielded 65,536 times. Even after
   shortening to 8,192 yields, forced repeated smoke runs still reproduced BIOS
   timeouts after all workers had been spawned.

   Fix: shorten the worker loop further and print the balance observation marker
   as soon as run queues converge. The program still waits for children before
   returning when run outside the smoke runner, but smoke coverage no longer
   depends on long post-observation child runtime.

2. **AP timer smoke evidence is incomplete.**

   The plan requires smoke coverage for AP timer ticks. The implementation has
   `OS1_KERNEL_EVENT_AP_TICK`, but it is emitted only every 1000 AP-local ticks,
   and the observe smokes do not assert it. Current observe logs show AP online,
   run-queue depth, kernel ping, and reschedule IPI events, but not AP tick events
   before the event-ring dump.

   Fix: emit the first AP tick per CPU immediately, keep the 1000-tick
   rate-limit afterward, and add `event ap-tick seq=` to UEFI/BIOS observe smoke
   markers.

3. **Console-read waiters still use a registry walk instead of a wait queue.**

   Phase 5 calls for console read waits to use a console-owned `WaitQueue`.
   `block_current_thread_on_console_read` still blocks without a queue, and
   `wake_console_readers` still finds waiters through
   `first_blocked_thread(ThreadWaitReason::ConsoleRead)`. This keeps a
   scheduler hot path tied to global thread-registry iteration.

   Fix: add a console-read `WaitQueue` owned by `console_input`, enqueue console
   readers there, and wake from that queue.

4. **`observe processes` violates the documented lock order.**

   `sys_observe_processes` takes `g_thread_registry_lock` before
   `g_process_table_lock`, while the SMP contract orders process table before
   thread registry. The count pass also reads `entry->process` while holding only
   the thread lock.

   Fix: take process table then thread registry in both passes.

5. **Some named SMP smoke targets from the plan are consolidated rather than
   present as standalone targets.**

   The current `smoke_observe` and `smoke_balance` targets cover AP online, AP
   idle, kernel ping, user work, and balance behavior, but the plan's individual
   `smoke_smp_*` target names are not present. This is acceptable for now if the
   consolidated smokes assert the same observables; AP tick was the missing
   observable from that set.

6. **QEMU smoke tests are not safe under CTest parallelism.**

   Reproduced with `ctest --test-dir build --output-on-failure -j 11`: 10 of 11
   smoke tests failed immediately because QEMU could not take write locks on
   shared images (`os1.raw` and `virtio-test-disk.raw`). The UEFI runner also
   reused one mutable `smoke-ovmf-vars.fd` across all UEFI smokes. A plain serial
   `smoke_all` run can pass, but CI or `act` can inherit `CTEST_PARALLEL_LEVEL`
   and make the same target flaky.

   Fix: mark all QEMU smoke tests `RUN_SERIAL` with a shared CTest resource lock,
   and give each UEFI smoke its own OVMF vars copy derived from its log file.

7. **Smoke markers can be split by unsynchronized console output.**

   Reproduced in `os1_smoke_spawn_bios`: the log contained the yield program's
   `tick 0` output, but a kernel `user thread ready` debug line interleaved with
   the marker text, so the smoke runner never saw the exact marker. The console
   stream was mirroring syscall writes byte-by-byte without an output lock, and
   the per-spawn debug line was high-volume enough to split user markers.

   Fix: serialize console writes with a console-output spinlock, remove the noisy
   per-spawn `user thread ready` debug line, and keep spawn/exec smoke assertions
   on `tick 2` plus shell completion rather than the more fragile first tick.

8. **Observe smoke waits on `smpcheck` child cleanup instead of the SMP evidence.**

   Reproduced on pass 5 of repeated `smoke_all` under `CTEST_PARALLEL_LEVEL=11`:
   `os1_smoke_observe_bios` reached the `smpcheck` command, but timed out before
   `[user/smpcheck] observed pids` and shell completion. Like balance, `smpcheck`
   waited for long-running helper children before printing its observation
   marker, so the smoke depended on cleanup runtime rather than the SMP condition.

   Fix: print the `smpcheck` observation marker as soon as the two helper
   processes are observed on different CPUs, shorten the helper loop, remove the
   shell-completion marker from observe smokes, and reject explicit smpcheck or
   balancecheck failure markers.

## Plan

1. Convert console-read blocking/wakeup to the console-owned wait queue.
2. Correct `sys_observe_processes` lock ordering.
3. Emit and assert early AP tick events.
4. Shorten the balance worker loop so the BIOS balance smoke completes within
   the existing timeout.
5. Serialize QEMU smoke tests and stop sharing UEFI vars state between smoke
   cases.
6. Serialize console writes and remove fragile first-tick smoke markers.
7. Make observe smokes assert the `smpcheck` observation instead of child cleanup.
8. Re-run host tests and `smoke_all`; iterate until both pass.
