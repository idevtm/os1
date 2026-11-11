# Remediation plan for the 2026-11-11 project review

This plan sets the order and acceptance criteria for the findings from the 2026-11-11 project review.
The local review file is `docs/reviews/2026-11-11-review.md`.
The review examined `main` at `f1dbfb5eb99085fcfdc69ba6484dfaa6ddcaf489`.
The R and P identifiers in this plan refer to that review.
The first implementation PR protects kernel entry.
The next PR corrects process exit, wait, and reap as one lifecycle.

| Order | Work | Findings |
| --- | --- | --- |
| 1 | [Kernel entry](#kernel-entry) | R1, with R36 and R38 checks |
| 2 | [Process lifetime](#process-lifetime) | R2–R4, then P4 and P5 |
| 3 | [Observation and quotas](#observation-and-quotas) | R7–R9, R22, R25, R48, R49 |
| In parallel | [Deployment and build tools](#deployment-and-build-tools) | R17, R20, R21, R41, R42, R45–R47, R52 |
| After triage | [Subsystem follow-ups](#subsystem-follow-ups) | Remaining R and P findings |

## Kernel entry

The review observed an unprivileged guest stop on the UEFI path.
The [entry assembly](../../src/kernel/arch/x86_64/interrupt/irqhandler.asm) and [scheduler restore](../../src/kernel/arch/x86_64/asm/multitask.asm) use GS for CPU state.
The corrected kernel must maintain this state across privilege changes on both boot paths.

- Define GS ownership for BSP and AP initialization, syscall entry and return, and interrupt entry and return.
- Define GS ownership for scheduler restores and NMI, double fault, and machine check on IST stacks.
- Review the entry and return transitions where an IST event can interrupt a change in GS ownership.
- Keep the current syscall ABI and the existing fast and slow return behavior.
- Check the shared trap-frame contract for R36 and R38 before changing it.

Check the assembled paths for valid CPU state before each GS-relative access.
Verify that supported user segment states preserve kernel CPU state during syscall and asynchronous entry.
Verify normal user and kernel returns, scheduling, timer interrupts, and fault handling.
Run the targeted checks on UEFI and BIOS with one CPU and with SMP.
Record syscall and interrupt latency, kernel size, and available BIOS image space.

[CPU, Interrupt, And Trap Architecture](../ARCHITECTURE.md#cpu-interrupt-and-trap-architecture)

## Process lifetime

The [wait path](../../src/kernel/syscall/wait.cpp) uses the thread registry to decide when a child can be collected.
The [reaper](../../src/kernel/proc/reaper.cpp) still uses the child process record after it removes the last thread.
The wake path also needs to respect the CPU that owns the waiting thread's stack.

- Define who owns the process record until final cleanup and who can collect it after cleanup.
- Protect parent references, child status collection, and reaper work with the documented lock order and lifetime rules.
- Preserve same-CPU completion before handoff. Defer completion while the waiter is current on a different CPU.
- Give deferred waiters a retry path after handoff, even if no other child exits.
- Reclaim an orphaned zombie once. Preserve exit status for a live parent and preserve retry after a failed status copy.

Add deterministic host tests for both CPU handoff cases, cleanup before collection, orphaning, and status-copy failure.
Verify one release per process and return of resource counts to baseline.
Run repeated spawn, exit, and wait workloads with SMP on UEFI and BIOS.
Record wait latency and retained pages. Measure P4 and P5 separately from the correctness change.

[Scheduler Model](../ARCHITECTURE.md#scheduler-model) · [Process Fault Handling And Teardown](../ARCHITECTURE.md#process-fault-handling-and-teardown) · [SMP Locking Order](../kernel/smp-synchronization.md#locking-order)

## Observation and quotas

Review R7, R8, R22, and R25 as one snapshot workstream.
Use bounded kernel-owned storage and keep each snapshot valid until the user copy finishes.
Maintain the caller length, record layout, count, and return semantics of the implemented observation ABI.
Keep user copies outside registry locks.
Check the arithmetic that converts record counts to byte lengths.
Check allocation failure and compare snapshot latency and temporary memory use.

Test registry growth and removal, concurrent event queries, and DMA and IRQ record changes.
Verify the returned header and record counts. Verify that the user copy respects its supplied length.
Investigate the separate R22 capacity check and append under concurrent DMA allocation.
If the race occurs, verify that each append has a valid reservation and that failed allocations release it.
Review R9 and R48 as quota and authority changes.
Test admission with competing callers under the approved policy.
Verify that final release makes each slot available again under its documented lifetime rules.
Review R49 as a disclosure policy decision before changing public behavior.

[Observability ABI](../ARCHITECTURE.md#observability-abi) · [Kernel console IPC ABI](../native-api/abi/kernel-console-abi.md) · [Memory log ABI](../native-api/abi/memory-log-abi.md)

## Deployment and build tools

Give R17 a separate PR because a mistaken selection can destroy user data.
Require explicit device selection and confirmation tied to the selected device.
Test cancellation, queued input, and device changes with simulated disk commands.
Verify that these cases issue zero erase or write commands.

Correct header dependency tracking and formatter scope in separate changes for R20 and R21.
Check incremental ELF builds, the ACPICA override headers, and the exclusion of upstream files.
Build `os1_usb_image` in a separate directory with the three diagnostic options set to `OFF`.
Set `OS1_QEMU_DEBUGCON=OFF`, `OS1_BOOT_DIAGNOSTICS=OFF`, and `OS1_USB_KEYBOARD_DIAGNOSTICS=OFF`.
Boot the generated USB image in QEMU and verify shell startup.

| Finding | Acceptance check |
| --- | --- |
| R41 | Compare host and production memory-layout constants. |
| R42 | Check each affected Python test registration. Run the suite with `PYTHONOPTIMIZE=1`. Verify that a failing assertion still fails its test. |
| R45 | Build from a source archive without Git metadata. Check the reported source identity. |
| R46 | Check that user program entry declarations agree with `crt0` and produce no LTO type mismatch. |
| R47 | Compile each UAPI header as C11 and C++. Check that the binary layouts stay compatible. |
| R52 | Pin action and compiler versions in CI. Verify the versions in the CI log. Check kernel size with that toolchain. |

[USB image workflow](../hardware/usb-image-workflow.md) · [Source formatting](../../AGENTS.md#source-formatting) · [Testing](../testing.md)

## Subsystem follow-ups

Compare each finding with the latest `main` before implementation.
Check R12, P11, P12, and overlapping EHCI changes against the framebuffer branch before assigning work.
The review reproduced R1 in UEFI.
Host and build checks reproduced R10, R21, R28, and R45–R47.
R2, R3, R7, R8, and the R22 read-side race have source evidence.
The review reports hypotheses for R13–R15, R27, R34, R50, and R53.
Record the evidence for each remaining finding when its implementation PR starts.
Require physical evidence for a physical-hardware claim.

| Area | Findings | Next check |
| --- | --- | --- |
| ACPI and IRQ routing | R5, R6, R26, R27, R37, R40 | Check table capacity, hardware limits, PCI IRQ flags, x2APIC, spurious interrupts, and BSP identity. Collect physical firmware evidence where required. |
| Boot and power | R14, R15, R39 | Test delayed AP startup and validate shim ranges against the firmware memory map. Check reset and shutdown when firmware lacks a reset register. |
| Keyboard input | R10, R33 | Test PS/2 scan-code translation and USB rollover without false console input. |
| xHCI recovery and timing | R11, R13, R34, P11 | Test HID halt recovery and delayed command completion. Check event-ring DMA ordering. Measure xHCI waits and worker delays. |
| EHCI deadline and scheduling | R12, P11, P12 | Test a delayed bulk transfer with a connected endpoint. Verify input remains usable. Measure worker delays and periodic schedule stops. |
| USB hardware qualification | R53 | Check DMA address width, reset timing, and periodic NAK handling. Qualify the changes on the [reference desktop and laptop](../hardware/reference-captures/README.md). |
| Block transport | R16, R32 | Verify read-only mounts and continued reads. Check virtio capability bounds with invalid transport metadata. |
| Filesystem and log export | R28–R31, P1–P3, P13, P14 | Check ext2 dirty state, move failures, reference lifetime, and final log drain. Measure block operations, export latency, and IRQ work before tuning. |
| Init and shell | R18, R19, R43, R44 | Check rescue startup, parser behavior, and file-save failure with separate tests. |
| Allocator and loader | R23, R24, R50, P10 | Check allocator accounting and ELF overlap cleanup. Measure stack and contiguous allocation use before changing limits. |
| Mapping and copies | R35, R51, P6, P9 | Check Buffer restoration after allocation failure and page-table walks at large leaves. Measure user-copy and TLB retirement costs. |
| IRQ and IPC costs | P7, P8 | Measure hot-path cost before changing dispatch. |

Compare the host ext2 I/O counts for P1 and P2 before and after any optimization.
Measure syscall latency during concurrent log export. Repeat the measurement after changes to ext2 block I/O.

Check the untested module loader, virtqueue, terminal, syscall dispatch, shell handles, and file helpers with their subsystem owners.
Add focused tests for the contracts changed by each follow-up.
Check each document conflict with its subsystem owner.
Update architecture when data flow or ownership changes.
Update implemented ABI profiles when a contract changes.
Keep proposals and historical evidence identified in the [Native API index](../native-api/README.md).

## Impact assessment and validation

| Area | Impact of planned implementation |
| --- | --- |
| Project scope | Keep fixes small and preserve runnable boot and userland paths. See [Primary objectives](../../GOALS.md#primary-objectives). |
| Boot and platform | Verify both firmware paths, shared entry code, `BootInfo`, higher-half mappings, and device assumptions for each affected change. |
| Memory and isolation | Verify per-CPU state, record ownership, user bounds, allocation failures, and cleanup. See [Kernel and userland separation](../../GOALS.md#kernel-and-userland-separation). |
| Concurrency | Verify ordering, lock tiers, IRQ context, wakeups, and record lifetime. See [Process and scheduling model](../../GOALS.md#process-and-scheduling-model). |
| Interfaces | Preserve published syscall and record layouts. Record any approved change to authority, quotas, or error behavior in its implemented profile. |
| Runtime resources | Compare latency, CPU time, retained memory, image size, and disk I/O where the change affects them. |
| Validation and documents | Use focused tests, the full local suite, and an independent review for each implementation PR. Record physical evidence separately. |

For each implementation PR, record the baseline and candidate revisions, toolchain, and build options.
Record the firmware, QEMU machine, CPU count, and memory limits.
Use the same workload and repeat count for both revisions.
Record units, logs, absolute and relative differences, and resource tradeoffs.
If the baseline is zero, mark the relative difference as undefined.
Keep existing failures separate from regressions.

Run the focused checks for the change.
Run `./scripts/check-local.sh` on the final executable source tree.
Retain the first failure evidence.
Use [Local validation and GitHub CI](../testing.md) for commands, log locations, and profile coverage.

This plan changes documentation only. Its checks cover links, source references, and command accuracy.
The review reports 681 host tests and 92 normal-image checks that passed on its recorded revision.
The diagnostic-image suite and physical hardware checks are unmeasured in that review.
The implementation PRs must establish their own baselines.
