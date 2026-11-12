# Remediation status for the 2026-11-11 project review — 2026-11-12

This document records an independent verification of the corrections for the [2026-11-11 project review](2026-11-11-review.md).
The main result: `main` contains a correction for each of the 53 defects, and the critical defect R1 no longer reproduces in QEMU.
Ten corrections concern hardware behavior and still need physical evidence.
Four of the 14 performance items are partly corrected.
Verified source: `dc43625` on `main`, the merge of PR 112. Reviewed baseline: `f1dbfb5`.

```text
Group         Total  Fixed,  Fixed  Fixed,         Partly  Open
                     run            hardware open  fixed
Critical         1      1      0        0             0      0
High             2      0      2        0             0      0
Medium          19      0     13        6             0      0
Low             31      5     22        4             0      0
Performance     14      1      9        0             4      0
```

| Read next | Purpose |
| --- | --- |
| [Scope and method](#scope-and-method) | What this verification examined and how |
| [Status labels](#status-labels) | Meaning of each status |
| [Critical and high defects](#critical-and-high-defects) | R1 to R3 with evidence |
| [Medium defects](#medium-defects) | R4 to R22 |
| [Low defects](#low-defects) | R23 to R53 |
| [Performance items](#performance-items) | P1 to P14 with measurements |
| [Remaining work](#remaining-work) | Residual risks and new observations |
| [Other review tables](#other-review-tables) | Test gaps, document conflicts, and maintainability |
| [Evidence and limits](#validation-evidence-and-limits) | Checks run, results, and limitations |

## Scope and method

This verification compares each finding in the review with the source on `main` at `dc43625`.
It does not change runtime code, tests, or build settings.
The [remediation plan](2026-11-11-remediation-plan.md) gives the intended order and acceptance checks.

```text
review finding (f1dbfb5)
   -> find the correcting commit (PR 56 to PR 112)
   -> read the corrected source path
   -> find the regression test
   -> run the complete local suite and focused experiments (dc43625)
   -> assign a status
```

The verifier did these steps for each finding:

1. Read the finding and its recommended correction.
2. Read the current source at the locations that the finding names.
3. Trace the corrected path, and look for the same defect in adjacent paths.
4. Identify the project test that covers the correction.
5. Run `./scripts/check-local.sh` on a clean clone of `dc43625`.
6. Repeat the experiments from the review where a repeat was possible.

The verifier did not review the 22 commits as complete changes.
New code that no finding names had no line-by-line review.
An independent reviewer then examined this document against the same revision.

## Status labels

| Status | Meaning |
| --- | --- |
| Fixed, run | The source contains the correction, and a run in this verification shows the corrected behavior |
| Fixed | The source contains the correction, the verifier traced it, and the local suite passes |
| Fixed, hardware open | As "Fixed", but the finding concerns hardware or firmware behavior that no physical test shows |
| Partly fixed | The source corrects a part of the finding; the table states the remaining part |
| Open | The source does not correct the finding |

QEMU and host results do not prove physical-hardware behavior.
A "Fixed" status means that the reported code path is corrected.
It does not mean that the subsystem has no other defect.

## Critical and high defects

### R1: User code controls the GS base that every kernel entry uses

**Status:** Fixed, run. **Correction:** PR 56 (`aa42697`), with later changes in PR 93.

The kernel now exchanges the GS base on each change between ring 3 and ring 0.

```text
ring 3                                   ring 0
IA32_GS_BASE = 0 (flat user base)        IA32_GS_BASE = per-CPU record
        SYSCALL, interrupt, exception  ->  swapgs before the first gs: access
        SYSRET, IRETQ to ring 3        <-  swapgs as the last step before return
kernel exception (vector < 32) in ring 0: read IA32_GS_BASE, swapgs only if the base is low
```

| Item | Evidence |
| --- | --- |
| Syscall entry | [`irqhandler.asm`](../../src/kernel/arch/x86_64/interrupt/irqhandler.asm): `swapgs` and `lfence` are the first two instructions of `syscall_entry` |
| Trap entry | `trap_entry_common` tests the saved CS, and does `swapgs` for ring 3 before the first `gs:` access |
| IST events | For kernel exceptions, the entry reads `IA32_GS_BASE` and exchanges only a low base; this covers NMI, `#DF`, and `#MC` in the transition window |
| Return paths | `swapgs` is immediately before `sysret` and before the user `iretq` in [`multitask.asm`](../../src/kernel/arch/x86_64/asm/multitask.asm); both paths keep RSP on a kernel stack until then |
| Non-canonical return | The SYSRET path uses `iretq` when the return address is outside the lower half |
| MSR setup | [`cpu.cpp`](../../src/kernel/arch/x86_64/cpu/cpu.cpp) writes `IA32_GS_BASE` and sets `IA32_KERNEL_GS_BASE` to 0 on each CPU; it clears `CR4.FSGSBASE` |
| Project tests | `os1_kernel_entry_assembly_contract` checks the linked instructions; `os1_smoke_kernel_entry_{bios,uefi}_{1,4}cpu` run a user probe |

The review experiment was repeated with a stronger program.
A scratch clone of `dc43625` changed only `src/user/programs/fault.cpp`.
The changed program does these steps in sequence:

1. It loads selector `0x1b` into GS and calls `write`.
2. It loads `0x1b` into FS and runs a busy loop across timer interrupts.
3. It calls `getpid` with RSP set to the kernel address `0xFFFF800000200000`.
4. It writes to address `0x1234`, as the unchanged program does.

`ctest -R '^os1_smoke_spawn(_bios)?$'` passed on UEFI in 9.72 s and on BIOS in 6.44 s, each with 4 CPUs.
Both guest logs show the same sequence:

```text
[user/fault] boom
[user/fault] gs-syscall-survived
[user/fault] gs-irq-survived
[user/fault] kernel-rsp-syscall-survived
user trap vector 14 pid 5 ... cr2 0x1234 error 0x6
user page fault killed pid 5
command failed: fault status=14
```

The kernel stayed in operation, stopped only the faulting process, and completed the remaining test steps.
At `f1dbfb5`, the first step stopped the machine after the `boom` line.
Step 3 was a hypothesis in the review; the kernel-address RSP causes no kernel fault now.

Notes:

- The project QEMU probe checks the default FS and GS bases. It does not load a user selector. The scratch experiment covers that case, but no project test does.
- The kernel still does not enable SMAP or SMEP. The review listed this as context, not as a required correction.

### R2: A child exit can wake a parent that another CPU still runs

**Status:** Fixed. **Correction:** PR 57 (`487a9c0`), then PR 66 (`024eea3`).

`wake_child_waiters` in [`wait.cpp`](../../src/kernel/syscall/wait.cpp) now tests each waiter with `current_on_remote_cpu`.
It puts such a waiter back in the queue and requests another reaper pass.
The BSP tick wakes the reaper worker for the retry, so the waiter does not depend on a second child exit.
Host test: `TaskRegistry.BspReaperTickRetriesPinnedThreadsAndRemoteChildWaiters`.

The rule is still local to this path.
`wake_blocked_thread` compares only with the current CPU, so each new wake path must add its own remote-CPU check.
No SMP stress run reproduced the original race or its absence.

### R3: The reaper uses a process that `waitpid` on another CPU can free

**Status:** Fixed. **Correction:** PR 57 (`487a9c0`), then PR 65 (`0928c6f`).

`waitpid` now collects a child only when it is `Zombie` and `exit_cleanup_complete` is set.
Only the reaper sets that flag, under `g_process_table_lock`, after it completes its work on the record.
`waitpid` sets `wait_claimed` under the same lock, so only one collector releases the record.
The reaper reads `state` and `parent` under the lock.
Host test: `TaskRegistry.WaitpidRequiresCleanupPublicationAndPreservesStatusOnCopyFailure`.

## Medium defects

| ID | Status | Correction and evidence | Remaining part |
| --- | --- | --- | --- |
| R4 | Fixed | The reaper in [`reaper.cpp`](../../src/kernel/proc/reaper.cpp) scans for a `Zombie` with complete cleanup and no parent, and reaps it. `orphan_children` requests a reaper pass. Test: `TaskRegistry.ReaperCollectsPublishedOrphanAfterParentWaitpid`. | No QEMU check of `process_count` after the provider smokes |
| R5 | Fixed | SSDT inventory capacity is 15, and one slot stays free for the DSDT. Discovery omits extra SSDTs with a log line. Test: `AcpicaIntegration.BoundsSsdtInventoryAndKeepsTablesAvailableToAcpica`. | None |
| R6 | Fixed, hardware open | GSI-sourced `_PRT` entries return level-triggered, active-low flags. The kernel calls `\_PIC(1)` before route resolution and accepts `AE_NOT_FOUND`. Tests: `AcpicaIntegration.SelectsApicModeAndKeepsDirectPciRoutesLevelLow`, `ReportsFailureFromPresentPicMethod`. | No run with an INTx-only device. See [observation N1](#new-observations) |
| R7 | Fixed | Each `observe` kind now fills a bounded kernel snapshot under the registry lock, and copies it to the user with no lock held. Test: `UsbInputObserve.RegistryGrowthAfterSizingLeavesUserBufferUntouched`. | See [observation N2](#new-observations) |
| R8 | Fixed | `observe(EVENTS)` uses a snapshot buffer for each call. The global buffer is gone. Tests: `ObserveEvents.CallerOwnedSnapshotsSurviveConcurrentPublishing`, `UsbInputObserve.EventSyscallKeepsCapturedRecordsDuringOverlappingRead`. | No SMP QEMU test with two observers |
| R9 | Fixed | A creator process can hold 8 of the 16 kernel bindings (`OS1_IPC_KERNEL_BINDING_PROCESS_LIMIT`). The [kernel console ABI](../native-api/abi/kernel-console-abi.md) records the limit. Test: `KernelConsole.BindingAdmissionLimitsEachCreatorAndTheSystem`. | Two processes can still use all 16 slots |
| R10 | Fixed | [`ps2_console.cpp`](../../src/kernel/drivers/input/ps2_console.cpp) returns bytes only for Latin and letter keysyms, and maps keypad keys explicitly. The terminal-switch hook consumes its hotkeys. Test: `Ps2Modifiers.ConsoleScancodesEmitOnlyIntentionalControlBytes`. | None |
| R11 | Fixed, hardware open | A halting completion code queues recovery for the worker. The worker does Reset Endpoint, Set TR Dequeue Pointer, and `CLEAR_FEATURE`, and retires the input if recovery fails. Tests: `XhciController.HaltedHidCompletionQueuesOneWorkerRecovery`, `HidClearHaltProtocolStallRestoresEp0AndRetiresOnlyInput`. | No physical halt test |
| R12 | Fixed, hardware open | A transfer deadline with a connected root port now retires only the device. The bulk deadline is 2 s. Test: `Ehci.ConnectedBulkTimeoutAllowsPeriodicScheduleToContinue`. | No device test with long NAK |
| R13 | Fixed, hardware open | Command, control, bulk, and port waits use `tsc_clock_now_100ns` deadlines of 1 s to 2 s. A command timeout aborts the command ring before the driver retires the controller. | Controller stop, reset, and run still count 1,000,000 loop iterations. The wait duration is not measured on hardware |
| R14 | Fixed, hardware open | Each AP finds its record through a table indexed by APIC ID, so a late AP cannot read the record of another CPU. A compare-and-swap decides between `Online` and `TimedOut`. The scheduler, IPI, and observe paths use only `Online` CPUs. The wait uses the TSC. | The BSP does not send INIT to an AP that timed out; that AP parks itself. No injected-delay run |
| R15 | Fixed, hardware open | The shim checks each `PT_LOAD` destination, the CPU page, `BootInfo`, the boot stack, and the frame bitmap against the Limine memory map. It stops with a message if a range is not usable. Tests: `BootInfo.UsableDestinationRejectsOccupiedAndOverflowingRanges`, `UsableDestinationCanSpanAdjacentMemoryRegions`. | Tested with OVMF only |
| R16 | Fixed | `BlockDevice` and `ext2::Io` have a `read_only` flag. Mutations return `OS1_ERR_ACCESS` and do not set `failed`. Log export skips the volume. Tests: `Ext2.ReadOnlyMountKeepsReadsAndSkipsDeviceFlush`, `LogExport.AutomaticExportSkipsReadOnlyMount`, and a read-only case in `test_ext2_persistence.py`. | None |
| R17 | Fixed | [`write_usb.py`](../../scripts/write_usb.py) flushes queued input, and the user must type the disk identifier before a write. Enter alone cancels. Test: `test_usb_menu.py`. | The verifier did not execute the tool against a disk |
| R18 | Fixed | Init counts shell exits within 1 s, waits between restarts, and starts rescue after 3 rapid exits. Test: `test_shell_bootstrap.py` with `tests/fixtures/faulting_shell.cpp`. | None |
| R19 | Fixed | `handles()` returns false for an `echo` line with a top-level command operator, before the parenthesis test. Tests: additions in `tests/host/common/shell_tests.cpp` and `test_typed_shell.py`. | None |
| R20 | Fixed | User programs compile with `-MD -MF` and `DEPFILE` in [`src/user/CMakeLists.txt`](../../src/user/CMakeLists.txt). Test: `test_user_elf_dependencies.py`. | None |
| R21 | Fixed | [`autoformat.sh`](../../autoformat.sh) accepts only explicit tracked paths under `src/` or `tests/`. The ACPICA override directory has its own `.clang-format`. Test: `test_source_formatting.py`. | The verifier did not repeat the format-and-compile probe |
| R22 | Fixed | The DMA registry is a linked list. Allocation appends, and `visit_dma_allocations` reads, under one lock hold each. No function returns a registry pointer after unlock. Test: `DmaBuffer.ConcurrentPublishersHaveNoSharedSnapshotCapacity`. | None |

## Low defects

| ID | Status | Correction and evidence | Remaining part |
| --- | --- | --- | --- |
| R23 | Fixed | `g_kmem_stats_lock` protects the global counters. A counter underflow clamps to 0 and does not halt. Test: `Kmem.ConcurrentLargeAllocationStatsStayConsistent`. | None |
| R24 | Fixed | `validate_user_elf_image` rejects load segments that share a page, before any mapping. Test: `UserElfPolicy.RejectsSegmentsThatShareAMappedPage`. | None |
| R25 | Fixed | `visit_irq_routes` reads the registry under its lock. `observe` copies the records into its snapshot. | No test changes routes during an observation |
| R26 | Fixed | The CPU table, the PCI function table, and both memory-map builders now cap their input and log the omission. The memory-map builders merge adjacent ranges. Tests: `AcpicaIntegration.CapsMadtCpuInventoryAndKeepsLateBsp`, `PciEnumeration.CapsDiscoveredFunctionsAndKeepsInventory`, `BootMemoryMap.*`. | More than 8 IOAPICs or 32 overrides still stop the boot; the review did not name these limits |
| R27 | Fixed, hardware open | `platform_discover` and AP startup stop with a clear message when `IA32_APIC_BASE.EXTD` is set. MADT x2APIC entries are counted and omitted. Test: `AcpicaIntegration.MadtX2ApicRecordsRequireAnXapicBspEntry`. | x2APIC operation is not supported, as the review permitted. No x2APIC run |
| R28 | Fixed, run | `begin_write` writes `s_state = 0`. The repeated host experiment reads 0 from the image after the first mutation; the baseline value was 2. | None |
| R29 | Fixed | A cross-directory move writes and flushes the destination block before it changes the source block. Test: `Ext2.CrossDirectoryMoveKeepsSourceUntilDestinationFlush`. | None |
| R30 | Fixed | Shutdown and reboot wait up to 250 ms for export admission before the final drain. Test: `LogExport.ShutdownDrainsAfterAdmissionClearsAndKeepsItThroughStorageFreeze`. | After 250 ms the transition fails with `log-drain-admission busy` |
| R31 | Fixed | `DiskFs::references` uses `__atomic_add_fetch`. | None |
| R32 | Fixed | Queue setup computes the notify offset in 64 bits and checks it against the capability length. A present ISR capability with length 0 is rejected. Tests: `VirtioPciTransportTest.RejectsWrappedNotificationRegion`, `RejectsZeroLengthAndOutOfBarIsr`, `RejectsQueueRegisterOutsideNotificationRegionBeforeEnabling`. | None |
| R33 | Fixed | An error report returns before any state update. Test: `HidKeyboard.ErrorReportsPreserveHeldKeysAndModifiers`. | None |
| R34 | Fixed, hardware open | The event reader loads the control word with acquire semantics, checks the cycle bit, and only then reads the other fields. | No memory-model check |
| R35 | Fixed | `map_alias` checks each mapping result and detaches a partial alias. Tests: `BufferMemory.FailedLoanRestoreRemovesPartialAliasWithoutOverwritingConflict`, `FailedLoanRestoreReleasesReservationWhenPageTablesAreMissing`. | None |
| R36 | Fixed | Trap entry stores the hardware RSP and SS for each privilege level. The kernel return builds the 5-word frame. `os1_kernel_entry_assembly_contract` checks both sequences. | None |
| R37 | Fixed | The spurious vector is `0xFF`. Trap dispatch returns without an EOI. Test: `TrapDispatch.LapicSpuriousVectorSkipsOrdinaryIrqAndFaultDispatch`. | None |
| R38 | Fixed | `exception_owner` gives NMI and `#MC` to the platform at each privilege level. Test: `ExceptionOwnership.NonMaskableInterruptAndMachineCheckBelongToPlatformAtEitherPrivilege`. | The kernel halts on each NMI or `#MC` |
| R39 | Fixed, hardware open | Reboot falls back to port `0xCF9` and to the keyboard controller. The transition parks the other CPUs first, and shutdown waits after `SLP_EN`. Tests: `Ipi.PowerParkRequiresEveryOnlinePeerAndClearsTimedOutRequest`, `AcpicaIntegration.WaitsAfterPm1aEnableEvenIfPm1bEnableFails`. | No run with a FADT that has no reset register |
| R40 | Fixed | MSI-X and the IOAPIC both use `cpu_boot_apic_id()`. Test: `PciMsi.UsesBootstrapApicIdWhenMadtListsAnotherCpuFirst`. | None |
| R41 | Fixed | [`cmake/kernel_layout.cmake`](../../cmake/kernel_layout.cmake) holds the layout constants for the production build and the host tests. The old bitmap base is gone from `memory_layout.inc`. | None |
| R42 | Fixed, run | Each Python test command uses `-E` or `-I`. `PYTHONOPTIMIZE=1 python3 -E -c "assert False"` raises `AssertionError`. | The suite was not run as a whole with `PYTHONOPTIMIZE=1` |
| R43 | Fixed | The shell reads and discards the here-document body after an early failure. Test: additions in `test_interactive_files.py`. | None |
| R44 | Fixed | `edit` writes a temporary file and moves it over the target. It removes the temporary file on failure. Test: additions in `test_interactive_files.py`. | None |
| R45 | Fixed, run | `log_build_identity.py` returns `unknown`, or `--archive-revision`, when no Git metadata exists. It adds `-dirty` for uncommitted changes. The script ran with exit status 0 on a `git archive` export. Test: `test_source_identity.py`. | The verifier did not run the complete kernel build from an archive |
| R46 | Fixed, run | `crt0` and all programs use `int main(int, char**)`. The kernel build log has no `-Wlto-type-mismatch` warning. | None |
| R47 | Fixed, run | `clang -std=c11 -fsyntax-only` reports 0 errors for each header in `src/uapi/os1/`. Test: `test_uapi_headers.py`. | None |
| R48 | Fixed | `OS1_LOG_CONTEXT_OPEN` needs `OS1_RIGHT_LOG_READ` on the root descriptor. Test: `LogIpc.HistoryAdmissionRequiresLogReadOnTheRootDescriptor`. | None |
| R49 | Fixed | `observe` reports `cr3` as 0. Event records show fault addresses only for user faults of the caller. Tests: `UsbInputObserve.ProcessSnapshotReservesCr3AsZeroForEveryRow`, `EventSnapshotDisclosesOnlyOwnUserFaultAddresses`. | None |
| R50 | Fixed | Kernel stacks are mapped above an unmapped guard page. The startup record is on the heap. Tests: `TaskRegistry.GuardedStacks*`, `Resources.LegacyStartupRecordReleasesBackingAndHandlesExhaustion`. | The stack high-water mark is not measured |
| R51 | Fixed | Small-page walks reject a present entry with the `PS` bit. Test: `VirtualMemory.SmallPageOperationsRejectLargeLeaves`. | None |
| R52 | Fixed | CI uses commit hashes for actions, builds the cross toolchain from `scripts/ci-cross-toolchain.sh`, and prints tool versions. | The verifier did not examine a CI run log |
| R53 | Fixed, hardware open | xHCI records `AC64` and limits DMA addresses. The reset path has the 1 ms Intel delay and the 10 ms reset recovery. EHCI periodic queue heads use NAK reload 0. | No physical tests on the recorded machines |

## Performance items

The table gives the status of each performance item.
Counts for P1 and P2 come from the host experiment of the review, repeated on both revisions.

| ID | Status | Correction and evidence | Remaining part |
| --- | --- | --- | --- |
| P1 | Partly fixed | The descriptor path gives ext2 block-sized units for calls larger than 256 bytes. User tools use 4 KiB buffers. See [ext2 I/O counts](#ext2-io-counts). | A 4 KiB append still costs about 7 reads and 8 writes. Small appends keep the 104× cost. Inode and counter writes are not deferred |
| P2 | Fixed, run | A multi-block read loads the indirect block one time. A 1 MiB single read used 257 reads; the baseline was 500. Test: `Ext2.IndirectMappingIsReadOncePerMultiBlockRead`. | Each separate block-sized call still reads the indirect block again |
| P3 | Partly fixed | Disk export writes block-sized units. Read, write, and exit check admission after they retain the file. Test: `LogExport.ConsoleWriteContinuesWhileFileIoDefers`. | Syscall latency during export is not measured. Admission stays global |
| P4 | Fixed | The reaper destroys the address space before it publishes the exit (PR 65). | None |
| P5 | Fixed | A BSP-pinned worker thread does teardown from a list of dying threads (PR 66). The schedule and IRQ paths no longer scan. Test: `TaskRegistry.SchedulerLeavesCleanupAndChildWakeToBspWorker`. | None |
| P6 | Fixed | Each address space has its own copy gate. `copy_user_string` walks the page tables one time per page. Tests: `BufferMemory.AnUnrelatedAddressSpaceCanCopyBeforeRetirementAcknowledgment`, `StringCopiesStopAtNulBeforeCheckingTheNextPage`. | None |
| P7 | Fixed | `timer_source.cpp` compares the vector with the recorded LAPIC timer vector. Test: `TimerSource.LapicModeMatchesOnlyTheCalibratedVector`. | None |
| P8 | Partly fixed | `ipc_drain_wakeups` returns at once when the `signalled_pending` flag is clear. Test: `Ipc.CancelLastSignalledWaiterThenPublishAnotherWake`. | `signal_locked` still scans all waiters under the global lock |
| P9 | Fixed | `revoke` sends the TLB IPI only when it detached a present alias. Test: `BufferMemory.ProcessRevocationRetiresOnlyPresentMatchingAliases`. | None |
| P10 | Fixed | The contiguous search skips full bitmap words and reserves the run with word masks (PR 64). Test: `PageFrameContainer.FailedRunSearchLeavesFragmentedBitmapAndTailUnchanged`. | None |
| P11 | Partly fixed | Runtime reconnect and hub debounce waits of 100 ms are now deadlines that the worker checks on later passes. Tests: `Ehci.ReconnectWaitRestartsAndCancelsOnNewConnectionStatus`, `RuntimeHubPowerAndDebounceAreSeparateFromReset`. | Shorter waits remain under `SchedulingGuard`. The verifier did not trace each one |
| P12 | Fixed | The worker links the next report transfer to the queue tail while the periodic schedule runs. Test: `Ehci.PeriodicInputAlternatesTransfersAndLinksShortReportsToTheTail`. | Recovery of a halted queue head still stops the schedule |
| P13 | Fixed | The BSP tick wakes a VFS worker thread, and the worker does the I/O in thread context. Test: `LogExport.VfsTickWakesWorkerWithoutReadingStorage`. | None |
| P14 | Fixed | `cat` and copy use 4 KiB buffers for regular files. The script reader reads ahead for a privately opened file. | A reader on shared standard input still reads one byte per call, by design |

### Ext2 I/O counts

The experiment links the real `src/kernel/fs/ext2.cpp` with a counting block device over `ext2-fixture.raw`.
It runs on the host, so the values are I/O counts, not device latency.
Each count is deterministic; one run for each revision and unit size.

| Workload | `f1dbfb5` | `dc43625` | Difference |
| --- | --- | --- | --- |
| 1 MiB append in 256 B units | 17,013 reads, 9,721 writes (104.4×) | 17,013 reads, 9,721 writes (104.4×) | 0 (0 %) |
| 1 MiB append in 4 KiB units | 2,013 reads, 2,041 writes (15.8×) | 1,757 reads, 2,041 writes (14.8×) | −256 reads (−12.7 % of reads) |
| 4 KiB overwrite in one 4 KiB unit | 2 reads, 2 writes | 1 read, 2 writes | −1 read (−50 %) |
| 1 MiB read in 256 B units | 8,000 reads | 8,000 reads | 0 (0 %) |
| 1 MiB read in 4 KiB units | 500 reads | 500 reads | 0 (0 %) |
| 1 MiB read in one call | 500 reads | 257 reads | −243 reads (−48.6 %) |
| `s_state` after the first mutation | 2 | 0 | R28 corrected |

The kernel descriptor path selects the unit size.
At `f1dbfb5` it always used 256 B units.
At `dc43625` it uses 4 KiB units when the caller gives more than 256 bytes.
For such a caller, a 1 MiB append changes from 104.4× to 14.8× device bytes, a decrease of 85.8 %.
For a caller that writes 256 bytes or less per call, the cost does not change.

## Remaining work

### Residual parts of the findings

1. P1: decrease the metadata cost for each appended block, and decide on a policy for small appends.
2. P3: measure syscall latency during log export in QEMU.
3. P8: decrease the `signal_locked` scan.
4. P11: examine the remaining waits that the USB worker does under `SchedulingGuard`.
5. R13: measure or replace the iteration-count waits for xHCI stop, reset, and run.
6. R9: decide if two processes that together use all 16 bindings are acceptable.
7. R2: move the remote-CPU rule into `wake_blocked_thread`, as the review suggested.
8. R1: add a project QEMU test that loads a user selector into GS and FS. The test must then do a syscall and wait for an interrupt.

### Hardware evidence

These findings have code corrections and host tests, but no physical evidence: R6, R11, R12, R13, R14, R15, R27, R34, R39, and R53.
Collect that evidence on the [reference desktop and laptop](../hardware/reference-captures/README.md).
The remediation plan also asks for targeted QEMU runs that do not exist yet:

- an INTx-only device (R6);
- an injected AP startup delay (R14);
- an x2APIC configuration (R27);
- a FADT with no reset register (R39).

### New observations

These items are not findings of the review. The verifier saw them while it traced the corrections.

| ID | Observation | Status |
| --- | --- | --- |
| N1 | If `\_PIC` exists and its evaluation fails, namespace initialization fails. Firmware with a defective `\_PIC` method then cannot boot, although PIC-mode routing was possible before. `acpica_integration.cpp`, APIC mode selection. | Hypothesis; no such firmware is known |
| N2 | `observe` sizes its snapshot from a count, and then captures under the lock. If the registry grows between the two steps, the call returns −1, also when the user buffer is large enough. The caller must try again. `observe.cpp`, `snapshot_capacity`. | Traced; not reproduced |
| N3 | `OS1_BSP_ONLY` is still on `g_irq_route_registry`, the PCI BAR and device-binding registries, and `kmem` objects that have their own locks and that other CPUs use. | Traced |

## Other review tables

### Test coverage gaps

| Gap in the review | Status on `dc43625` |
| --- | --- |
| Trap and syscall entry (R1) | Assembly contract test and four QEMU probes exist. No test loads a user selector |
| Process lifetime (R2 to R4) | Host tests exist for all three. No SMP stress run |
| Observe ABI (R7, R8, R22, R25) | Host tests exist for R7, R8, and R22. None for R25 |
| IPC quotas (R9, R48) | Host tests exist |
| ACPI and IRQ routing (R5, R6, R40) | Host tests exist. No INTx-only QEMU run |
| SMP boot (R14) | Startup-state host tests exist. No slow-AP run |
| USB input (R11, R12, R13, R33) | Host tests exist for R11, R12, and R33. The verifier found no delayed-command test for R13 |
| PS/2 input (R10) | Host test over the scancodes exists |
| Storage (R16, R29, R30) | Host tests exist; R16 also has a QEMU case |
| Init and shell (R18, R19, R43) | QEMU and host tests exist |
| Build (R20, R41, R45) | `test_user_elf_dependencies.py`, the shared layout file, and `test_source_identity.py` exist |
| Formatting (R21) | `test_source_formatting.py` exists |
| Production configuration | Open. `check-local.sh` still does not boot the image with `OS1_QEMU_DEBUGCON=OFF` and `OS1_BOOT_DIAGNOSTICS=OFF` |
| Units without host tests | Virtio PCI transport and `commands::handles` now have tests. The verifier did not examine the other units |

### Conflicts between documents and implementation

| Row in the review | Status on `dc43625` |
| --- | --- |
| GS base is never protected | Corrected. [ARCHITECTURE.md](../ARCHITECTURE.md#cpu-interrupt-and-trap-architecture) describes the `SWAPGS` rules |
| Phase 6 function names | Open. `LoadUserProgramImage` and `WriteCr3` are still in the Phase 6 diagram |
| "Retry `INT 0x80`" | Corrected. The text now describes the two-byte rewind for SYSCALL |
| Observe snapshots are not consistent | Corrected in code (R7) |
| Orphaned zombies | Corrected in code (R4) |
| 64-byte key queue omission | Not changed |
| Module profile and USB hotplug ambiguity | Not changed |
| xHCI removal in IRQ context | Corrected in code. `xhci_irq` records the port change, and the worker does the removal |
| 16 retained device slots and failed enumerations | Not examined |
| Ext2 unclean state | Corrected in code and in [ext2.md](../filesystems/ext2.md) |
| Read-only virtio behavior | Corrected. [ext2.md](../filesystems/ext2.md) has a read-only row |
| USB storage transport after malformed status | Not examined |
| Storage README limits | Not changed. The README still states one block device |
| Final drain during shutdown | Corrected. [log-export-abi.md](../native-api/abi/log-export-abi.md) states the 250 ms deadline |
| `OS1_BSP_ONLY` labels | Partly corrected. See observation N3 |
| `autoformat.sh` scope | Corrected in [AGENTS.md](../../AGENTS.md#source-formatting) and in the script |
| Per-process binding accounting | Corrected. The console ABI states 8 bindings per creating process |

### Maintainability suggestions

The verifier examined only the suggestions that a finding depends on.

| Suggestion | Status on `dc43625` |
| --- | --- |
| Centralize the "current on any CPU" wake rule | Open. A fifth local copy now exists in `wait.cpp` |
| One copy-out helper for registry snapshots | Done. `ObserveSnapshot::collect` and the `visit_*` functions |
| Correct `OS1_BSP_ONLY` labels | Partly done |
| Capability flags on `BlockDevice` | Partly done. `read_only` exists |
| Layout constants in one CMake file | Done |
| Dependency files for user programs | Done. `-Werror` is not enabled |
| Stale comments | The `INT 0x80` comment in `trap_dispatch.cpp` is corrected |
| Other suggestions | Not examined |

## Validation evidence and limits

### Environment

| Property | Value |
| --- | --- |
| Verified revision | `dc43625` (`main`), clean working tree |
| Baseline revision | `f1dbfb5eb99085fcfdc69ba6484dfaa6ddcaf489` |
| Source tree | A local `git clone --shared` of each revision in a scratch directory |
| Submodules | `third_party/acpica` `232ff3f8`, `third_party/googletest` `52eb8108` (same as the baseline) |
| Host | macOS on Apple M4 Max, 16 CPUs, 64 GiB |
| Host compiler | AppleClang 21.0.0 |
| Cross compiler | x86_64-elf-gcc 11.2.0 |
| Emulator | QEMU 11.1.3 |
| Build options | Those in [`scripts/check-local.sh`](../../scripts/check-local.sh) |

### Checks run

All checks ran on `dc43625`. One run for each check.

| Check | Command | Result |
| --- | --- | --- |
| Complete local validation | `./scripts/check-local.sh` | Exit status 0; 441 s in total |
| Host unit tests | Part of `check-local.sh` | 792 of 792 passed; 27.66 s test time. Baseline: 681 of 681 |
| Normal-image QEMU suite | Part of `check-local.sh`, `--parallel 2` | 104 of 104 passed; 379.69 s. Baseline: 92 of 92 in 384.77 s |
| Diagnostic-image checks | Part of `check-local.sh`, `-R '^os1_smoke_usb_keyboard_'` | 3 of 3 passed; 9.71 s. Not run at the baseline |
| Kernel build warnings | Build log of the normal-image profile | 3 project warnings, all `-Wextra` enum warnings that the baseline also had; the R46 warning is gone |
| GS experiment (R1) | Scratch clone with a changed `fault.cpp`, then `ctest -R '^os1_smoke_spawn(_bios)?$'` | 2 of 2 passed; 16.17 s. Baseline: UEFI failed after 60.11 s |
| Ext2 I/O count (P1, P2, R28) | Real `fs/ext2.cpp` of each revision with a counting block device | Values in [Ext2 I/O counts](#ext2-io-counts) |
| UAPI as C11 (R47) | `clang -std=c11 -fsyntax-only -Wall` on each header in `src/uapi/os1/` | 0 errors for each header. Baseline: 9 errors in `ipc.h` |
| Build identity from an archive (R45) | `git archive dc43625`, then `log_build_identity.py --root <export>` | Exit status 0; revision `unknown`. Baseline: `fatal: not a git repository` |
| Python assertion mode (R42) | `PYTHONOPTIMIZE=1 python3 -E -c "assert False"`; inspection of each test command | `AssertionError`; each test command has `-E` or `-I` |

The logs are in the scratch directory of the verification session and are not part of the repository.
No check failed, so no first-failure evidence exists.

### Image sizes

The corrections add code, so the images are larger. The BIOS floppy image keeps its fixed size.

| Image | `f1dbfb5` (bytes) | `dc43625` (bytes) | Difference |
| --- | --- | --- | --- |
| `kernel.elf` | 621,160 | 653,480 | +32,320 (+5.20 %) |
| `kernel_bios.elf` | 462,216 | 483,912 | +21,696 (+4.69 %) |
| `os1.iso` | 4,358,144 | 4,399,104 | +40,960 (+0.94 %) |
| `os1.raw` | 1,474,560 | 1,474,560 | 0 (0 %) |
| `os1-usb.img` | 35,651,584 | 35,651,584 | 0 (0 %) |

Both builds used the options of the normal-image profile, the same toolchain, and the same machine.
Syscall latency, interrupt latency, and retained memory are unmeasured in this verification.
The implementation PRs record their own measurements.

### Limitations

- This verification did not test physical hardware.
- No run reproduced the R2, R3, R7, R8, or R22 races on either revision. Their status depends on the code trace and the host tests.
- The verifier read the corrected paths. It did not review all 14,321 added lines of the 22 commits.
- The verifier identified the project tests by name and by the passing suite. It did not check that each test fails without its correction.
- The R1 experiment ran in QEMU with 4 CPUs. It is not hardware evidence.
- The rows marked "Not examined" in [Other review tables](#other-review-tables) have no status.
- The complete ASD-STE100 Issue 9 dictionary was not available. This text follows the rules in [AGENTS.md](../../AGENTS.md#mandatory-documentation-standard); full compliance is not established.

## Documentation change impact

This change adds one status document and one row in the [review index](README.md).
It does not change source, ABI, build settings, or tests.
It does not replace the whole-project review, so the [archive rule](../README.md#organization-rules) does not apply to it.

| Area | Impact |
| --- | --- |
| Project scope | Records the status of each finding; no scope change |
| Boot and platform | Confirms corrections for R1, R5, R6, R14, R15, R26, R27, and R36 to R40; no code change |
| Memory and isolation | Confirms corrections for R1, R3, R4, R22 to R24, R35, R50, and R51; no code change |
| Concurrency | Confirms corrections for R2, R3, R7, R8, R14, R22, R23, R25, R31, and R34; no code change |
| Interfaces | Confirms corrections for R7 to R9 and R47 to R49; no ABI change |
| Runtime resources | Records image sizes and ext2 I/O counts for both revisions; no runtime change |
| Validation and documents | Records the checks, results, and limits of this verification; no test change |
