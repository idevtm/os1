# Pager merge-blocker investigation

PR 38 CI found a kernel page fault during a UEFI file-copy check.
This plan investigates the failure before the authorized merge and timestamp implementation.

```text
Outgoing thread -> Ready queue -> leave stack and CR3 -> publish successor
                        |
                  Remote migration must wait
```

## Requirements and impact

The affected requirements are GOALS “Process and scheduling model,” “Boot strategy,” and “Testability and observability.”
ARCHITECTURE “Scheduler Model” and “Unified TrapFrame” define the affected ownership boundary.
The Native API records remain unchanged; this change affects internal thread scheduling.

| Area | Assessment |
| --- | --- |
| Scope | Correct a reproduced scheduling defect before merging the pager. |
| Boot and platform | Shared SMP scheduler affects BIOS and UEFI; preserve assembly handoff order. |
| Memory and isolation | A CPU must leave a thread's stack before another CPU can use it. |
| Concurrency | Check current execution under the donor queue lock before migration. |
| Interfaces | No syscall, BootInfo, IPC, or binary record changes. |
| Runtime resources | Compare image sizes and repeated file-copy workload timings. Measure both firmware paths. |
| Validation and documents | Add a deterministic host regression; run host, SMP, firmware, and CI checks. Obtain independent review. |

## Plan

1. Retain the CI fault and local reproduction evidence.
2. Reproduce migration during the donor's handoff window in a host test.
3. Exclude the still-current donor thread from migration under the queue lock.
4. Verify migration becomes possible after the donor publishes its successor.
5. Run affected checks and measurements against the unchanged pager build.
6. Obtain an independent correction review before publication.

CI run 42415401825 passed 486 host tests and 55 of 56 integration checks.
The failing UEFI traversal check reported a supervisor write fault during `cp`.
A local repetition failed after nine successful checks, but reported a hang instead of a page fault.
These failures do not establish identical causes.
The local transcript is `build/pager-ci-evidence/baseline-hang.log`.

Static review found that the balancer can steal a queued thread while its donor still uses the thread's stack.
The original independent review classifies this pre-existing defect as P1.
The exact CI instruction cannot be resolved against the different local compiler output.
Keep that attribution limit separate from the deterministic migration regression.

## Correction and review

The host regression fails before the correction: the receiver steals the still-current donor thread.
The corrected migration predicate checks the donor's current thread under the donor queue lock.
The same regression then passes and verifies that migration succeeds after successor publication.
All 487 host checks pass in 11.04 seconds.
An independent correction review found no remaining defects in locking, handoff ordering, wakeups, or the regression.

Further firmware checks, runtime comparisons, and final CI results are recorded in [PR 38](https://github.com/idevtm/os1/pull/38).
Physical-hardware validation remains pending.
The complete ASD-STE100 dictionary was unavailable; manual prose review does not certify full compliance.

## Additional request-lifetime investigation

Longer UEFI repetitions still hang after the scheduler correction.
A separate run leaves an invalid ext2 inode block after successful command completion.
These results keep PR 38 blocked.

```text
IRQ holds queue lock -> publish done -> finish request wait-queue access -> release queue lock
Caller observes done ----------------------------------------------------> may release request
```

The captured hang has two CPUs blocked on locks.
The IRQ holds the virtio queue lock and waits for a request-owned wait-queue lock.
The caller has reused that request's stack storage and waits for the virtio queue lock.
`completion_signal` publishes completion before its final access to the request's wait queue.
The driver's unlocked completion check lets the caller return before that final access.

The additional correction must check completion under the virtio queue lock.
Keep the existing timeout and DMA quarantine behavior.
Add a concurrent host regression for callback retirement before request release.
Repeat both firmware checks and persistent image validation before merge.

This extends the memory and concurrency impact to stack-owned block requests and cross-CPU IRQ completion.
Storage interfaces and disk formats remain unchanged.
The captured state is `build/pager-ci-evidence/repro/capture2.first.memory` with its corresponding register log.
The original CI write address equals its invalid thread pointer plus the offset of `Thread::wait_link`.
That offset supports the request-reuse diagnosis without assuming identical compiler output.

The concurrent host regression fails with the old unlocked completion check.
It passes when the poller acquires the queue lock before accepting completion.
All 488 host tests pass in 11.16 seconds.
EHCI and xHCI complete their stack-owned block requests synchronously before returning.
Their interrupt handlers do not retain these request pointers.
