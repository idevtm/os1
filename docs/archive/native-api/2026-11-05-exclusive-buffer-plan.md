# Exclusive Buffer implementation plan

This plan defines the first exclusive Buffer implementation and its required validation.
The approved scope covers Operation loans, same-buffer processing, and distinct outputs. The [implementation record](2026-11-05-exclusive-buffer-implementation.md) reports current results.

```text
Create -> map -> fill -> submit -> request escrow -> receive -> process
                         |                              |
                         |                              v
                         +<-- restore <- retire <- reply or provider failure

Distinct output -> reply escrow -> one result claim -> caller mapping
```

The [scope](#scope-and-contract) defines ownership and compatibility.
The [impact assessment](#impact-assessment) identifies affected requirements.
The [acceptance criteria](#acceptance-criteria) define the release conditions.
The [measurement plan](#measurement-plan) defines comparable evidence.

## Scope and contract

The kernel allocates dedicated, zero-initialized Buffer pages. Programs fill those pages through explicit mappings.
IPC transfers access to those pages without copying payload bytes. Small control records still use normal kernel copies.
The first mode gives one process exclusive access. All aliases lose access before another process receives access.
The receiver can inspect the entire Buffer. Method offsets and lengths do not create a security boundary.

Submission lends input Buffers to one Operation. The Operation retains the input pages until terminal handling.
A timeout or closed Operation descriptor does not cancel accepted work.
Terminal handling revokes provider access before restoring caller ownership and signaling completion.
An application error can leave modified input bytes. Provider failure after delivery reports an unknown outcome.

Original mappings remain reserved during the loan. Completion restores them at their original addresses.
An explicit unmap can abandon a reservation. Exec and process exit discard old address-space reservations.
Ownership uses a stable process incarnation. Mapping restoration also checks address-space identity and generation.
Closing a descriptor alone does not remove mappings.

A reply can name the input Buffer or transfer a distinct provider-owned Buffer.
The first case preserves identity. The second case holds output ownership in reply escrow until one successful result claim.
A failed result descriptor installation or control copy preserves the output for retry.
Every input loan returns independently of the selected output.

Existing byte messages and copied capability records retain their layouts and meanings.
New resource operations occupy values 11–14. New Buffer IPC operations occupy values 23–26.
Legacy capability attachment operations reject Buffers. They cannot bypass ownership checks.
The separate Buffer interface uses object kind 9 and interface bit 256.

The accepted limits are 65,536 bytes per Buffer, eight allocated objects per originating process, and 128 globally allocated pages.
Each Buffer permits four mapping records. Each process permits 32 mapping records.
Each process permits eight outbound loan objects and eight borrowed objects.
An IPC batch permits four objects. Duplicate objects and contradictory directions fail before any ownership change.
Retained origin budgets charge physical storage until final release. Transfers and aliases do not reset these charges.

Live readers, shared writable mappings, arbitrary page adoption, snapshots, and permanent request moves remain deferred.
The [ownership proposal](../../native-api/buffer-ownership.md) contains those broader design options.
This plan accepts only its exclusive Operation-loan phase.

## Impact assessment

| Area | Affected requirements | Assessment |
| --- | --- | --- |
| Project scope | [GOALS: Simplicity first](../../../GOALS.md#3-simplicity-first); [Runnable vertical slices](../../../GOALS.md#4-runnable-vertical-slices) | One Buffer mode and two TextProcessor methods demonstrate a complete ownership flow. General shared-memory frameworks remain deferred. |
| Boot and platform | [GOALS: Boot strategy](../../../GOALS.md#boot-strategy); [Architecture: Early physical layout](../../ARCHITECTURE.md#early-physical-layout-shared-by-boot-paths) | Both boot paths load the changed kernel and initrd. Existing staging limits remain mandatory. |
| Memory and isolation | [Architecture: Virtual memory](../../ARCHITECTURE.md#virtual-memory); [User address spaces](../../ARCHITECTURE.md#user-address-spaces) | Dedicated pages, explicit mappings, retained budgets, non-executable permissions, and ownership checks change VM lifetime rules. |
| Concurrency | [Architecture: Scheduler model](../../ARCHITECTURE.md#scheduler-model); [Client sessions: Lock order](../../native-api/abi/client-sessions-abi.md#lock-order-memory-and-checks) | Acknowledged TLB retirement, remote user-copy admission, migration, and terminal IPC transitions require coordinated serialization. |
| Interfaces | [GOALS: Native system interface](../../../GOALS.md#native-system-interface); [Copied IPC ABI](../../native-api/abi/copied-ipc-abi.md); [Client sessions API](../../native-api/abi/client-sessions-abi.md) | Append operations and records. Preserve copied IPC, descriptor ceilings, retained Operation capacity, and session accounting. |
| Runtime resources | [GOALS: Performance stance](../../../GOALS.md#performance-stance); [Architecture: Build outputs](../../ARCHITECTURE.md#build-outputs-and-boot-artifacts) | Buffer pages and mapping records add bounded memory use. Retirement adds CPU coordination and latency. Kernel and initrd grow. |
| Validation and documents | [GOALS: Testability and observability](../../../GOALS.md#testability-and-observability); [Architecture: Test architecture](../../ARCHITECTURE.md#test-and-ci-architecture) | Add host boundary tests and guest ownership tests. Compare both boot paths and SMP behavior against preserved baseline evidence. |

`BootInfo` layouts, firmware discovery, and higher-half addresses do not change.
Network, storage, and device protocols do not change. They remain indirect regression targets through shared VM and user-copy helpers.
No physical-hardware qualification follows from host or QEMU results.

## Required implementation boundaries

The Buffer module owns backing pages, mapping records, ownership generations, retained budgets, and transfer batches.
The IPC module owns admission reservations, Operation states, reply receipts, completion visibility, and one-time result claims.
IPC reserves a transition under its lock. It performs Buffer retirement outside that lock.
IPC publishes readiness or completion only after the required memory transition completes.

VM teardown must detach Buffer mappings before freeing anonymous pages.
Exec must discard reservations for the old CR3 before replacing the process address space.
The existing Process record survives exec, so a process identifier alone cannot authorize restoration.

Kernel user copies translate user addresses and then access the direct map.
Their admitted access must complete before revocation. Remote console and VFS wake paths also perform these copies.
Clearing a PTE's Present bit alone is insufficient for existing translation checks.
Use zero PTEs, or require both translation and access validation to reject nonpresent leaves.

TLB retirement must acknowledge invalidation on every relevant CPU before ownership publication or frame reuse.
The protocol must cover migration and address-space switching.
A TLB interrupt must reach its acknowledgement path before general IPC wake processing.
Syscall entry clears interrupts. Concurrent retirement initiators therefore need a safe service protocol while waiting.
No retirement wait can hold IPC, scheduler, process-table, or descriptor locks required by another participant.

## Acceptance criteria

| Boundary | Required evidence |
| --- | --- |
| Creation and mapping | Zero initialization includes padding. Size, overflow, rights, mode, and execute checks reject invalid access. |
| Ownership | Identity remains stable. Every origin alias loses access during a loan. Stale borrower references cannot map after return. |
| Retirement | A delayed acknowledgement delays ownership publication. Simultaneous initiators progress with interrupts disabled. |
| Kernel copies | A delayed admitted copy prevents retirement. New copies fail or wait during revocation. |
| Admission | Object, page, mapping, descriptor, queue, and loan exhaustion leave all source mappings and ownership unchanged. |
| Receive | Failed descriptor installation and control output preserve the request for retry. |
| Reply | Failed preparation preserves reply authority. Application errors still return every input loan. |
| Same-buffer output | The caller recovers original addresses and identity without a second payload allocation or transport copy. |
| Distinct output | The input returns separately. Output claims succeed once across aliases and remain retryable after delivery failure. |
| Process lifetime | Caller exit, caller exec, provider death, and live-provider endpoint closure cannot restore stale mappings. |
| Operation lifetime | Timeouts and closed Operation aliases cannot reclaim accepted loans prematurely. |
| Compatibility | Existing copied IPC, activation, session, descriptor, spawn, and exec checks retain their contracts. |
| Metadata | TextProcessor advertises explicit Buffer mode and direction. Existing methods retain their signatures and behavior. |
| Runtime | Both TextProcessor examples pass on UEFI and BIOS with SMP. All artifact bounds remain unchanged. |

Add regression tests for confirmed defects found during independent review.
After corrections, repeat affected checks and obtain another independent review.
Do not declare completion while blocking findings remain.

## Measurement plan

The baseline revision is `b729b24b03bba8d2bddc36de8c3aac3c958d2cf4`.
The preserved baseline uses Debug configuration, kernel `-Os`, allocator debugging, and debugcon.
The guest uses QEMU `q35`, `qemu64`, four CPUs, 128 MiB RAM, and TCG.
The baseline evidence directory is `build/buffer-baseline/`.
Its environment record contains toolchain versions and firmware hashes.

| Artifact | Baseline bytes | Relevant bound | Remaining bytes |
| --- | ---: | ---: | ---: |
| `kernel_bios.elf` | 332288 | 368640 | 36352 |
| `initrd.cpio` | 183808 | 196608 | 12800 |
| `kernel.elf` | 444248 | Separate boot-contract checks | Not derived from this file size |

The BIOS staging window is 360 KiB. The initrd window is 192 KiB.
The baseline leaves limited room for new code and test programs.
Measure artifact sizes early. Preserve loader bounds and existing tests.

Repeat the frozen spawn workload three times per boot path.
Keep new Buffer workloads separate from this comparison.
Record elapsed seconds, success or failure, absolute difference, and relative difference.
The baseline BIOS median is 7.692146 seconds from three successful runs.
The baseline UEFI successful median is 8.450257 seconds from two successful runs.
Its third UEFI run timed out after 30.090499 seconds during `paramcheck`.
Preserve that pre-existing failure separately from implementation regressions.
These durations include boot and test setup. They are not IPC latency measurements.

Repeat the same observe workload on both final boot paths.
Compare free pages, slab pages, live allocator objects, and allocation failures at matching workload positions.
Measure Buffer handoff latency separately for comparable payload sizes and repeat counts.
Measure the retained and peak Buffer page charges with bounded workloads.
Record zero transport payload copies from implementation paths and focused instrumentation or tests.
Do not infer copy avoidance from latency alone.

Use bytes for images, 4096-byte pages for page counts, and seconds or nanoseconds for durations.
Report final minus baseline values and relative percentages.
When the baseline is zero, report the relative difference as undefined.
Mark unavailable measurements as unmeasured and identify the next required check.

## Original validation status and limitations

This section records the plan status before implementation.
The [implementation record](2026-11-05-exclusive-buffer-implementation.md) supersedes this original status.
The implementation, runtime measurements, and independent implementation review remain pending.
The parent task owns build registration, integration checks, and final evidence reports.
Changed C and C++ files require scoped formatting with the root configuration.

The official ASD-STE100 website returned HTTP 403 during this task.
The complete Issue 9 standard and dictionary were unavailable.
The prose follows available project rules and glossary terms. Full language compliance remains unverified.
