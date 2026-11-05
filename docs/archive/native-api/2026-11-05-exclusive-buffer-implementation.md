# Exclusive Buffer implementation record

This record describes the first exclusive Buffer implementation and its validation.
The final build passes 303 host tests, all 14 root integration checks, and repeated workloads on both boot paths.

```text
Create and map -> submit LOAN -> request escrow -> provider access
      ^                                              |
      +------------ retire and restore <-------------+

Provider output -> MOVE -> reply escrow -> one successful result claim
```

The [ABI](../../native-api/abi/buffer-loans-abi.md) defines the current records and limits.
The [accepted plan](2026-11-05-exclusive-buffer-plan.md) defines requirements.
The [impact assessment](#impact-assessment) identifies affected contracts.
The [validation](#validation) separates tests from unverified behavior.

## Scope and implementation

The kernel owns dedicated, zero-initialized backing pages.
Each Buffer permits one active owner or borrower.
A loan revokes all origin mappings before provider access begins.
Completion retires provider mappings before restoring the caller's reserved addresses.

IPC retains input loans through timeout and Operation descriptor closure.
Application errors restore access but do not undo changes to bytes.
Distinct outputs stay in reply escrow until one successful result claim.
Failed descriptor installation or control copies preserve unclaimed output for retry.

The memory module owns backing, budgets, mapping records, and ownership generations.
IPC owns admission, transition reservations, receipts, completion, and result consumption.
Private providers can use resource operations 11–14 for Buffer control.
Their other syscall restrictions remain in force.

`LowercaseBuffer` changes the input pages and returns the same Buffer identity.
`UppercaseBuffer` writes its result directly into a separate Buffer.
Both methods use a 24-byte attachment reference with an offset and length.
`intf /bin/textprocessor` reads the bounded `.os.buffers` metadata without provider execution.

## Impact assessment

| Area | Requirement source | Effect |
| --- | --- | --- |
| Project scope | GOALS: Simplicity first; Runnable vertical slices | One exclusive mode supports two complete method examples. |
| Boot and platform | GOALS: Boot strategy; Architecture: Build outputs and boot artifacts | Both boot paths load changed kernel and initrd images. Existing image bounds apply. |
| Memory and isolation | Architecture: Virtual memory; User address spaces | Dedicated pages, non-executable mappings, retained budgets, and restoration change page lifetime. |
| Concurrency | Architecture: Scheduler model; Buffer plan: Required implementation boundaries | The copy gate and acknowledged retirement serialize access across CPUs. A scheduler correction preserves a waiter awakened during reaping. |
| Interfaces | Copied IPC ABI: Endpoints, Operations, and calls | New operations preserve existing records and numeric values. |
| Runtime resources | GOALS: Performance stance | Backing pages, mapping records, metadata, and retirement add bounded costs. |
| Validation and documents | Architecture: Test and CI architecture | Host fault injection and both guest boot paths cover the changed contracts. |

`BootInfo`, firmware discovery, and higher-half addresses do not change.
Storage, network, and device protocols do not change.
Shared VM and user-copy changes can affect those subsystems indirectly.
The complete smoke matrix checks their existing guest workloads.

## Corrections during integration

The first guest Buffer call returned an application error on both boot paths.
The private-provider syscall filter denied Buffer resource operations before dispatch.
The filter now admits the four new Buffer operations.
The guest method test exercises this complete syscall path.

Independent review found two repeated-result defects.
A failed repeated result copy could revoke an already claimed output.
A successful repeated read could erase a later loan's reserved mapping.
Commit and rollback now require an active, unconsumed claim.
Host and guest regressions exercise repeated reads after successful claims.

Independent review also found delayed cleanup for zombie processes.
Last-thread teardown now removes Buffer mappings before the parent calls `waitpid`.
Tests replace and destroy the old address space before later loan completion.
These tests check that completion cannot restore stale mappings.

The guest descriptor-exhaustion test initially used an alias without DELEGATE.
It now duplicates an authorized alias until the descriptor table is full.
Its expected LIMIT result remains unchanged.

## Validation

The isolated build directory is `build/buffer-validation/`.
It has separate guest disks and firmware variable copies.
No existing user QEMU process was stopped.
The baseline remains unchanged in `build/buffer-baseline/`.

```sh
cmake -S . -B build/buffer-validation -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-elf.cmake \
  -DCMAKE_BUILD_TYPE=Debug -DOS1_KMEM_DEBUG=ON \
  -DOS1_QEMU_DEBUGCON=ON -DOS1_REQUIRE_UEFI_SMOKE=ON
cmake --build build/buffer-validation -j4
cmake --build build-host-tests -j4
ctest --test-dir build-host-tests --output-on-failure --no-tests=error
ctest --test-dir build/buffer-validation --output-on-failure --no-tests=error
```

The final host suite passed 303 tests.
The complete root integration matrix passed 14 tests after the scheduler correction.
The image build passed its existing layout checks.
The initrd contains 195584 bytes, with 1024 bytes below its unchanged bound.

Both spawn smokes now require all Buffer success markers.
The guest tests exercise copied IPC, same-buffer calls, distinct output, timeout, provider stop, endpoint closure, and result retry.
They also execute actual revoked reads, revoked writes, and execution attempts.
Successful runs record page-fault errors `0x4`, `0x6`, and `0x15`, followed by caller and shell recovery.

The earlier intermittent stall came from a child-exit wakeup race in the existing scheduler.
A failed guest snapshot showed the caller marked Running, with no CPU or run queue.
Its saved return value already contained the completed child's PID.
All retirement acknowledgements had completed, and the copy gate was unlocked.

Reaping can complete the current thread's child-exit wait before a CPU handoff.
The scheduler now keeps that awakened thread runnable.
A deterministic host regression failed before correction and passed afterward.
Repeated guest workloads provide the additional evidence below.
Monitor and no-UART tests require local Unix socket access outside the restricted sandbox.
The monitor contract passed with that permission.

## Measurements

The comparison baseline is revision `b729b24b03bba8d2bddc36de8c3aac3c958d2cf4`.
The final source is the working-tree diff from that revision.
Both configurations use Debug, kernel `-Os`, allocator debugging, and debugcon.
QEMU uses `q35`, `qemu64`, four CPUs, 128 MiB, and TCG.

The baseline environment record contains compiler, linker, assembler, QEMU, CMake, and firmware versions and hashes.
Final measurements use the same tools and firmware.
The frozen comparison copies the baseline test definitions and scripts into `build/buffer-comparison/`.
Only image paths and isolated output paths change.

The baseline BIOS median is 7.692146 seconds from three successful samples.
The baseline UEFI successful median is 8.450257 seconds from two successful samples.
Its third UEFI sample timed out during `paramcheck` before implementation began.
That failure remains separate from the new fault-recovery stall.
All six final frozen-workload samples completed.
The following medians include boot, CTest setup, workload commands, and settling.
They do not measure IPC latency.

| Boot path | Baseline seconds | Final seconds | Difference seconds | Difference percent |
| --- | ---: | ---: | ---: | ---: |
| UEFI | 8.450257 | 8.461815 | +0.011559 | +0.137% |
| BIOS | 7.692146 | 7.767498 | +0.075352 | +0.980% |

| Artifact | Baseline bytes | Final bytes | Difference bytes | Difference percent |
| --- | ---: | ---: | ---: | ---: |
| `kernel.elf` | 444248 | 455712 | +11464 | +2.581% |
| `kernel_bios.elf` | 332288 | 340480 | +8192 | +2.465% |
| `initrd.cpio` | 183808 | 195584 | +11776 | +6.407% |
| `os1.iso` | 4354048 | 4378624 | +24576 | +0.564% |
| `os1.raw` | 1474560 | 1474560 | 0 | 0% |
| `kernel_limine.elf` | 41368 | 41368 | 0 | 0% |

Artifact sizes use one build per revision.
The BIOS kernel has 28160 bytes below its 368640-byte staging bound.
The initrd has 1024 bytes below its 196608-byte bound.
All existing layout checks pass; no bound or regression workload was removed.

The observe workload uses one sample per boot path at matching command positions.
It measures 4096-byte pages and allocator objects.

| Metric | Baseline | Final | Difference | Difference percent |
| --- | ---: | ---: | ---: | ---: |
| UEFI free pages | 19634 | 19628 | -6 | -0.03056% |
| BIOS free pages | 31994 | 31991 | -3 | -0.00938% |
| Slab pages, either boot | 160 | 160 | 0 | 0% |
| Live objects, either boot | 2331 | 2332 | +1 | +0.04290% |
| Free objects, either boot | 569 | 568 | -1 | -0.17575% |
| Allocation failures, either boot | 0 | 0 | 0 | Undefined |

Total managed pages remain 24685 for UEFI and 32732 for BIOS.
These observations do not measure the peak Buffer workload.

The Buffer host test compares physical addresses before loan, during borrowing, and after restoration.
The addresses remain equal while modifications remain visible after restoration.
The transport changes mapping authority and copies control records, with no payload-copy loop.
The provider algorithm reads and writes Buffer pages directly.
This evidence supports payload-copy avoidance; it does not imply zero runtime overhead.

The guest timing loop performs 64 calls with 252-byte logical payloads for each transport.
It includes submission, waiting, result delivery, and cleanup.
Its clock has millisecond resolution.
All six extended spawn samples completed, with three samples per boot path.
Each sample includes the real fault and recovery checks.
The timing comparison uses the final binary for both transports.
The Buffer timing workload retains its 4097-byte object, which occupies two backing pages.

| Boot path | Copied median ns | Buffer median ns | Difference ns | Difference percent |
| --- | ---: | ---: | ---: | ---: |
| UEFI | 6000000 | 13000000 | +7000000 | +116.667% |
| BIOS | 5000000 | 11000000 | +6000000 | +120.000% |

These aggregate times cover 64 calls, not one call.
The small-payload Buffer path costs more in this workload.
It avoids transport payload copies while adding mapping, descriptor, and CPU-retirement work.
No speed improvement is claimed.

A separate host workload measures three 65536-byte Buffer lifecycle cycles.
It uses a 32 MiB synthetic arena, one simulated CPU, Debug, and allocator debugging.
One origin mapping and one borrower mapping share 16 backing pages.

| Cycle | Free pages before | Peak pages used | Peak percent of starting free pages | Free pages after | Retained pages |
| --- | ---: | ---: | ---: | ---: | ---: |
| Cold | 7876 | 21 | 0.266633% | 7871 | 5 |
| Warm 1 | 7871 | 16 | 0.203278% | 7871 | 0 |
| Warm 2 | 7871 | 16 | 0.203278% | 7871 | 0 |

The cold cycle retains five infrastructure pages while both address spaces remain alive.
That retained difference equals 0.063484% of the starting free-page count.
Every cycle releases all 16 Buffer backing pages.
Both warm cycles return to their exact free-page baseline.
Host page measurements do not establish guest peak usage or physical-hardware behavior.

The evidence directory contains `measurements.json`, `environment.json`, and exact final test logs.
Frozen samples reside in `build/buffer-comparison/timings.json` with each serial and CTest log.
Extended samples reside in `build/buffer-validation/evidence/extended-timings.json` with each serial and CTest log.
The unchanged baseline retains its original failure and resource logs.
Image hashes identify the tested final binaries.
Before-correction images and failed CPU captures remain in the evidence directory.

## Physical-device follow-up

The user reported three successful `buffercheck` runs on the Asus G74SX on November 5.
The supplied photograph shows these aggregate measurements:

| Run | Calls | Logical bytes per call | Copied ns | Buffer ns |
| --- | ---: | ---: | ---: | ---: |
| 1 | 64 | 252 | 1000000 | 2000000 |
| 2 | 64 | 252 | 1000000 | 1000000 |
| 3 | 64 | 252 | 1000000 | 2000000 |

All three runs show successful identity, restoration, distinct-output, fault-recovery, and provider-failure checks.
Each command returns status zero and restores the shell prompt.
This evidence does not establish sustained operation or complete hardware qualification.

The prepared UEFI candidate preserves the known G74SX USB layout and replaces its kernel and initrd.
Its image SHA-256 is `eef3fb6a42257a99e5e1949d00a7c88e520d2cc9b6e854b46d5d4fa835c6df14`.
Preparation records and QEMU logs remain under `local-evidence/g74sx/2026-11-05-buffer-usb/`.
The photograph does not verify the written image hash or USB readback.
The candidate uses Debug, kernel `-Os`, allocator debugging, and disabled debugcon and boot diagnostics.
Its configuration differs from the QEMU comparison above; no cross-machine speed ratio is claimed.

`OS1_IPC_CLOCK` currently reads a counter advanced by the 1000 Hz BSP scheduler tick.
Its unit is nanoseconds, but its resolution is one millisecond.
These values count tick boundaries across 64 calls; they do not measure individual call latency.
A shorter batch can report zero when no tick boundary occurs.
IPC wakeups can schedule work between periodic ticks, so this resolution is not a minimum scheduling or IPC latency.

## Next steps

The next work should close existing correctness gaps and improve measurement before adding sharing modes.
This sequence records proposed work; it does not change the current ABI or implement a new clock.

| Priority | Work | Completion evidence |
| --- | --- | --- |
| 1 | Make ordinary ACPI observation passive. | Snapshot tests prove no power transitions; any separate diagnostics require explicit authority and driver coordination. |
| 2 | Recheck the remaining review findings. | Close IST allocation failure and smoke-settle gaps with regressions; preserve unexplained historical failures separately. |
| 3 | Establish useful IPC timing and larger-payload comparisons. | Validate clock resolution and monotonicity, then collect repeated measurements with explicit workload and configuration records. |
| 4 | Complete exact-image hardware qualification. | Record image hashes, CPU observations, sustained operation, and recovery results on the physical device. |
| 5 | Select the next native-object or storage slice. | Specify live-reader publication before implementation, or define a read-only block-backed filesystem contract. |

The [project review](../../reviews/2026-11-03-review.md#7-recommended-next-milestones) retains the original correctness and platform priorities.
Descriptors, console, startup arguments, initrd VFS, hosted IPC, sessions, rich parameters, and exclusive Buffer loans now have implementation records.
Follow-up: the [passive ACPI correction](../kernel/2026-11-05-passive-acpi-observation.md) completes priority 1 by removing runtime firmware calls.
The Buffer scheduler correction does not explain every historical BIOS or baseline timeout.
Before adding programs, resolve the initrd's remaining 1024-byte headroom without removing regression coverage.

### Clock and benchmark plan

The immediate performance task is trustworthy measurement, followed by analysis of the measured costs.

```text
Validate clock -> measure overhead -> repeat comparable batches -> profile costs
```

1. Define a monotonic measurement clock with reported resolution and availability.
2. Evaluate TSC frequency stability, calibration, cross-CPU consistency, migration, and instruction ordering.
3. Provide a validated fallback or report high-resolution timing as unavailable.
4. Measure timestamp overhead and use longer batches that exceed the clock resolution.
5. Repeat warm and cold workloads across payload sizes and CPU configurations.
6. Report total batch time, call count, distributions, and derived per-call estimates separately.
7. Profile mapping changes, TLB retirement, descriptor work, and wakeups before optimizing them.

The existing firmware TSC helper needs review before reuse.
Its calibration-failure fallback advances on each query; that value does not measure elapsed time.
Keep measurement changes separate from IPC deadline and timer-delivery semantics unless a later contract explicitly changes both.
Compare copied and Buffer calls directly within the copied payload limit.
For larger payloads, define equivalent chunked-copy work and disclose its additional calls.
Report Buffer size, page count, initialization, reuse, allocation, and cleanup costs separately from the method's logical payload.

### Follow-up documentation assessment

This addition follows GOALS sections Performance stance and Testability and observability, plus Architecture section Test And CI Architecture.
It clarifies measurement claims and the Buffer ABI's validation limits without changing runtime behavior.
Boot, memory, concurrency, interfaces, and resource use are unaffected by this documentation addition.
Validation checks source claims, local links, and recorded measurements; no kernel rebuild is required for this addition.
The existing complete implementation reviews remain applicable.
A separate independent review also approved this documentation addition.

## Limits and review status

The exclusive mode does not provide simultaneous readers, writable sharing, page adoption, snapshots, or request-side permanent moves.
A failed online CPU can stall retirement indefinitely.
The protocol assumes no PCID and no global user mappings.
Host acknowledgement tests simulate the hardware boundary.
QEMU results do not qualify physical hardware.

Host CPU use, peak RSS, energy, and physical-device I/O remain unmeasured.
Future measurement needs host resource instrumentation and a defined hardware workload.
Independent review covers the complete source diff, correction interactions, documents, and recorded evidence.
Both final independent reviews report no blocking findings.
They verified the corrections and their interactions with the complete change.

The official ASD-STE100 site returned HTTP 403 during this task.
The complete Issue 9 standard and dictionary were unavailable.
The prose follows available project rules and glossary terms.
Full language compliance remains unverified.
