# Input polling and EHCI recovery

This change addresses input failures from two normal-boot hardware runs.
The final local suite passes after storage scheduling and controller ownership corrections.
Both mice delivered movement, but neither run passed all input checks.

```text
normal input polling -> memory-only input provider
log export -> separate kernel worker -> filesystem admission -> bounded files
EHCI split error -> bounded endpoint retry -> same input identity OR retained retirement evidence
```

## Hardware evidence

The [evidence record](2026-11-10-normal-input-evidence.json) preserves identities, archive hashes, counters, and analysis limits.
The source image remains in `build/hardware-input-normal-2026-11-10/data.ext2`.
Its filesystem check passed, both storage probes matched, and both capture archives validated.
Both runs used revision `c4cc470` and build `0c88f3040d131f34f84f54427f9fefc69a90774f6fd314a43ceba9c5aa4eb1d8`.
The user kept both mice connected throughout each test.

| Machine | PASS / FAIL / SKIP | Observations |
| --- | --- | --- |
| Desktop | 19 / 4 / 3 | Logitech delivered 1,331 movement events and 14 button changes. All four subscriptions reported loss. |
| Laptop | 20 / 2 / 1 | SIGMACHIP delivered 208 movement events and 15 button changes before removal. Touchpad delivered 130 movement events. |

Desktop subscriptions share one global ring. Their loss counters overlap and must not be added.
Quiet keyboard subscriptions do not establish lost keystrokes.
The laptop retained two EHCI failures with token `0x80040141` on endpoint `0x81`.
This token identifies a halted split transfer with exhausted retries and four untransferred bytes.
The second failure occurred after the original observation window.
The physical cause of the downstream transaction error remains unconfirmed.

The desktop `lshw` capture contains 72,218 bytes. The laptop capture contains 65,944 bytes.
Both captures are complete, which confirms the earlier capture-limit fix.
The separate laptop Bluetooth reference mismatch remains unresolved.

Retained input IPC durations reach 1.433 seconds on the desktop and 1.805 seconds on the laptop.
Normal-mode logs contain repeated input polls, including empty reads classified as warnings.
Export timestamps align with long completions, and source inspection shows automatic export on the shared provider worker.
These observations support the contention hypothesis but do not attribute every delay to export.
Memory-log and input-event timestamps use different clocks; their absolute values must not be subtracted.

## Requirements and impact

Relevant requirements are GOALS: Simplicity first, Real hardware enablement, Native system interface, and Testability and observability.
The Native API index classifies input, memory logging, and log export as implemented profiles.
Architecture responsibilities change in Input services and attachment changes, and Memory logging and command correlation.

| Area | Assessment |
| --- | --- |
| Scope | Keep native input independent from automatic persistence. Preserve visible failure results. |
| Boot and platform | Test BIOS and UEFI. Add one kernel worker; preserve boot records and mappings. |
| Memory and isolation | Keep bounded queues, retained Operations, DMA ownership, and descriptor checks. Measure image and worker costs. |
| Concurrency | Separate export dispatch; narrow filesystem admission; protect exported status snapshots. Preserve scheduler wakeup ordering. |
| Interfaces | Preserve syscall and input record layouts. Add explicit runner opt-outs; retain existing opt-in spellings. |
| Runtime resources | Reduce routine log traffic. Bound EHCI retries and retain the original input identity during recovery. |
| Validation and documents | Add host regressions, run the complete local suite, compare baseline artifacts, and obtain independent review. |

Network protocols, process authority, filesystem formats, and user memory permissions do not change.
The full ASD-STE100 dictionary is unavailable. Technical prose receives manual review against the available project rules.

## Implementation order

1. Suppress successful native-input polling and expected empty reads in normal logging.
2. Separate automatic export from native-provider dispatch and narrow filesystem admission.
3. Retry eligible EHCI split errors before retiring a still-connected input device.
4. Enable input observation and USB-mouse expectations by default in `run-tests`.
5. Run regression checks, the complete local suite, and independent review.

True input errors and attachment changes remain observable.
Diagnostic mode retains detailed IPC tracing.
`--no-input` disables the supervised observation window.
`--no-expect-usb-mouse` permits machines without a USB mouse.
Existing `--input` and `--expect-usb-mouse` options remain accepted.
Automatic diagnostic boot uses the physical defaults unless the explicit unattended deployment setting is present.

## Retest

Update the kernel and disk programs with the preserving USB operation.
Boot normal `os1` on each machine.
Run `run-tests --machine laptop` or `run-tests --machine desktop`.
Keep each mouse connected during the observation window.
Move each pointer and operate its buttons during that window.
Use `shutdown` before returning the stick.
Physical recovery and loss-free operation remain pending until this retest passes.


## Implementation details

Successful input polls, empty reads, and successful descriptor close/query calls are omitted from normal-mode tracing.
Failures and diagnostic-mode traces remain available.
The I/O worker dispatches file, directory, lifecycle, and export requests separately from memory-only services.
Basic IPC and descriptor operations bypass filesystem admission. Program loading still requires admission.
Export status uses a locked snapshot of completed work.

EHCI recovery permits three attempts per ten-second window, with 20/40/80-millisecond delays.
The driver keeps halted queue-head slots reserved during backoff.
It verifies the connection path and quiesces the periodic schedule before descriptor reuse.
The endpoint keeps its input identity and hardware data toggle.
The [input ABI](../native-api/abi/input-abi.md#bounded-ehci-endpoint-recovery) defines eligibility and retained evidence.


## Independent review

The separate reviewer checked the complete implementation, contracts, evidence, and regression tests without editing files.
The first review found one behavioral defect and two documentation errors.
Expected timer expirations still produced warning records during normal polling.
The correction suppresses expected WAIT_UNTIL and WAIT_MANY expirations while preserving diagnostic traces and unexpected errors.
A regression exercises 2,000 expected expirations plus error and diagnostic cases.

The review also corrected the IPC_CREATE description and the default observation duration.
IPC_CREATE creates an in-memory endpoint pair; program loading uses launch, spawn, or exec.
The default observation lasts 20 seconds; `--no-input` uses one second.
The second source review found no remaining defects.
Physical EHCI recovery remains outside host-test and QEMU evidence.


## BIOS startup failure found during validation

The reviewed full suite found a BIOS startup timeout in `os1_smoke_ehci_failure_bios`.
The first failure remains in `build/artifacts/test-runs/os1_smoke_ehci_failure_bios-c32dnzf3/`.
Its disk image, transcript, test arguments, and result record remain unchanged.
Forty isolated boot trials and forty post-boot synchronization trials did not reproduce the timeout.
These passing trials did not resolve the blocker.

Source review identified an unregistered halt after a `Busy` block submission.
The new export worker can run on CPU1, where the BIOS profile disables periodic timer interrupts.
Previously, automatic export ran on CPU0, which receives PIT ticks.
A Busy request has no completion producer, so a later controller release does not necessarily wake that AP.
The exporter can consequently retain filesystem admission indefinitely.

Controlled QEMU contention reproduced this deadlock without changing the kernel image.
CPU1 halted at `relax_block_wait`, with filesystem admission equal to `UINT32_MAX`.
After the injected controller busy flag cleared, CPU1 remained halted and admission remained locked.
Registers and guest memory remain in `build/input-recovery/boot-investigation/forced-before/`.
This confirms the deadlock path; the original timeout has no live CPU snapshot.

Kernel workers register a one-millisecond scheduler timer before retrying an unaccepted Busy request.
Early boot and user syscall frames use processor polling instead of an unregistered halt.
Read, write, and flush requests use the same Busy retry rule.
Accepted requests retain their existing completion path. Permanent errors remain final.
The change affects generic storage scheduling, so both firmware paths and the complete storage suite require validation.


Host regressions verify timed wakeup registration and Busy retries for read, write, and flush operations.
They also verify that permanent submission errors remain final.
Controlled post-fix contention releases admission and completes `diskutil sync` after the busy flag clears.
The corresponding registers and memory remain in `build/input-recovery/boot-investigation/forced-after/`.

Independent review found that user syscall frames cannot park through the kernel-thread scheduling path.
The correction limits timer parking to kernel workers. A separate host regression checks the user-frame fallback.
The quiet contention comparison prevents serial input from supplying an incidental wakeup.
Before correction, admission remained locked after the controller became available.
After correction, admission cleared without serial input.
Evidence remains in `build/input-recovery/boot-investigation/quiet-before/` and `quiet-after/`.

A further review found deferred storage I/O inside interrupt handlers.
Those frames must not enter the timer parking path, even when a kernel worker is current.
The retry helper now requires enabled interrupts and a kernel thread before it can park.
A host regression verifies that interrupt-disabled kernel frames preserve their thread state.
The interrupted validation run remains in `build/input-recovery/full-local-complete.log`.
It is not a final validation result.

The final correction review found no remaining defects.
It covered IRQ entry, all retry-helper callers, timer wakeup interactions, fixture configuration, and the regression assertions.
The final controlled run is `build/input-recovery/boot-investigation/quiet-irq-final/`.
Filesystem admission cleared after the busy flag cleared, without serial input.
The subsequent `diskutil sync` completed successfully.

### Automated test duration

The physical defaults must not add activity waits to automated QEMU checks.
Private QEMU disks set the explicit [unattended boot policy](device-test-runner-plan.md#unattended-automatic-tests).
The dedicated pointer-activity test retains its existing 20-second window.
This preserves default-option coverage without adding that window to other tests.

### Controller-owner scheduling dependency

The next full suite found one startup failure in `os1_smoke_observe_bios`.
That suite passed 678 host tests, 91 normal-image tests, and all three diagnostic checks.
This failed suite remains in `build/input-recovery/failed-observe-suite/`.
The original observation failure remains in `build/artifacts/test-runs/os1_smoke_observe_bios-k6r2qrkn/`.

Repeated unchanged-image boots captured the same failure with live CPU and memory state.
Evidence remains in `build/input-recovery/observe-stress/boot-178/`.
CPU2 executed init's storage syscall with interrupts disabled and the EHCI controller busy flag set.
The USB worker that owned the controller was ready in CPU2's run queue.
The other CPUs were idle. Filesystem admission contained one foreground caller.
The syscall could not yield to its preempted controller owner.

Controller ownership now defers scheduling while preserving interrupt service.
A scoped guard covers EHCI and xHCI polling and storage submission.
The guard starts before busy acquisition and ends after busy release.
IRQ dispatch services hardware and timers, but defers scheduling and deferred filesystem work during the scope.
The outermost guard requests a pending reschedule when ownership ends, including on BIOS APs without periodic ticks.

The scope cannot sleep or yield. Existing synchronous controller timeouts still bound its duration.
Other tasks on that CPU can wait during topology or storage operations.
Interrupt handling remains available; this avoids disabling interrupts throughout long USB delays.
A host regression covers nested ownership, preserved interrupts, IRQ deferral, and the final reschedule IPI.

The ownership-guard image completed 200 repeated BIOS boots without a startup stall.
This intermediate image precedes the reschedule-volume correction below.
The failing workload used two concurrent QEMU guests, four virtual CPUs per guest, and 128 MiB per guest.
The unchanged-image investigation captured one stall among 201 started boots; the other 200 completed.
These counts establish regression evidence, not a statistical reliability guarantee.
Passing-run transcripts remain; duplicate generated disk copies can be removed after analysis.
The failed guest's disk, CPU registers, and memory remain preserved.

Future asynchronous controller operations can reduce the scheduling delay of these synchronous ownership scopes.
That conversion requires separate request lifetime and cancellation work.
It is not part of this correction.

### Reschedule event volume

A later observation check completed all commands but lost four early boot records from the bounded event ring.
The failure remains in `build/artifacts/test-runs/os1_smoke_observe_bios-yjoftpn5/`.
Controller-scope exits requested reschedules even when no other task could run.
The event snapshot contained 122 reschedule records from the boot-sequence thread alone.
The interrupted validation log is `build/input-recovery/full-local-owner-final.log`.

Scope exit now requests a reschedule only when the local run queue contains work.
It clears the pending request before sending, which combines repeated requests inside interrupt-disabled syscalls.
A failed IPI send restores the pending request.
Regressions cover 1,000 empty-queue scopes and 1,000 scopes inside a syscall.
The boot-event assertions remain unchanged.

The corrected observation checks passed three consecutive BIOS runs and three consecutive UEFI runs.
All original boot-event assertions passed. The combined check took 24.29 seconds.
Independent review found no lost wakeup between the run-queue check, remote enqueue, and IPI publication.

## Validation record

The final source tree must pass the complete local command before push.
Intermediate runs do not replace that requirement.

| Log under `build/input-recovery/` | Result and scope |
| --- | --- |
| `full-local-first.log` | Intermediate pass; source changed between profile builds. This is not final qualification. |
| `full-local-final.log` | Failed BIOS storage startup; original guest evidence retained. |
| `full-local-complete.log` | Stopped after review identified IRQ-context parking. |
| `full-local-reviewed.log` | Failed BIOS observation startup; live reproduction confirmed controller-owner preemption. |
| `full-local-owner-final.log` | Stopped after boot events were evicted by redundant reschedule requests. |
| `owner-coalesce-final-host-tests.log` | Seven focused regressions passed in 0.45 seconds. |
| `observe-coalesce-tests.log` | Three BIOS and three UEFI observation runs passed in 24.29 seconds. |

No assertions were removed or weakened. Diagnostic reruns preserve the earlier failure evidence.
Physical recovery remains unmeasured until the user repeats the hardware tests.

### Final qualification

`./scripts/check-local.sh` passed on revision `9b7f1fb` plus the complete task changes.
The final source-diff hash and new-file hashes are recorded with the measurements.
No executable or validation changes followed this run.

| Profile | Result | Duration |
| --- | --- | --- |
| Host tests | 681 / 681 passed | 21.93 seconds |
| Unfiltered normal-image suite | 92 / 92 passed | 379.87 seconds |
| Diagnostic-image checks | 3 / 3 passed | 9.81 seconds |

The final command log is `build/input-recovery/full-local-final-qualified.log`.
Profile logs are preserved in `build/input-recovery/final-suite/`.
The BIOS EHCI failure check passed in 25.90 seconds. BIOS observation passed in 8.12 seconds.
The diagnostic fixtures use explicit unattended mode; their assertions reject the 20-second activity prompt.
The dedicated input-activity test retains its existing window and verifies the physical defaults.

Formatting, local document links, JSON syntax, and credential-pattern checks passed.
The independent reviewer found no remaining blocking defects after the final correction.
Review coverage includes IRQ ordering, controller ownership, request lifetime, admission, wakeups, tracing, and fixture behavior.
The recorded review limitations remain: no physical recovery claim and no complete ASD-STE100 dictionary verification.

### Resource comparison

The [measurement record](2026-11-10-input-recovery-measurements.json) contains hashes, source profiles, firmware identities, samples, and failure-capture references.
The comparison uses QEMU 11.1.3, GCC 11.2.0, kernel `-Os`, Debug configuration, and two concurrent TCG guests.
Each q35 guest has four CPUs and 128 MiB of memory.
The baseline artifact embeds `b350fac`; its reconstructed source/profile hash matches recorded HEAD `9b7f1fb` inputs.

| Measurement | Baseline | Final | Difference |
| --- | --- | --- | --- |
| `kernel.elf` | 617,408 bytes | 619,992 bytes | +2,584 bytes; +0.42% |
| `run-tests.elf` | 44,672 bytes | 44,832 bytes | +160 bytes; +0.36% |
| EHCI static pool | 38,304 bytes | 40,352 bytes | +2,048 bytes; +5.35% |
| Export status and lock | 0 bytes | 112 bytes | +112 bytes; relative difference undefined |
| `Thread` object | 664 bytes | 664 bytes | 0 bytes; 0% |
| BIOS unattended command median | 1.789 seconds | 1.894 seconds | +0.105 seconds; +5.85% |
| UEFI unattended command median | 1.593 seconds | 1.665 seconds | +0.072 seconds; +4.50% |

Each command median contains two samples from one isolated BIOS/UEFI pair per tree.
The workload uses baseline defaults and final explicit unattended opt-outs, with the same one-second observation.
These small samples do not establish a performance trend. They show no added 20-second activity wait.
The final pair passed in 36.55 seconds; logs remain in `build/input-recovery/candidate-pair.log`.

The separate I/O worker adds one 16,384-byte kernel stack and a Thread allocation.
Aggregate allocator overhead and physical input latency remain unmeasured.
EHCI recovery adds bounded retry state and at most three configured backoff delays per window.
The user must confirm physical recovery, latency, and loss behavior on both devices.
