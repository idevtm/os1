# Exception stacks, smoke settling, and measurement clock

This phase closes two correctness gaps and separates performance measurement from scheduler ticks.
USB keyboard issue 27 remains outside this phase.

```text
IST allocation gate -> complete smoke settling -> high-resolution measurement -> repeated IPC batches
```

## Plan and acceptance

1. Require an exception stack before loading the kernel IDT on every CPU.
2. Stop a CPU safely when stack allocation fails; never publish that AP as online.
3. Reject smoke processes that exit before the requested settling interval finishes.
4. Test early EOF, delayed failure, missing markers, and successful settling.
5. Add a measurement API using a validated 64-bit HPET counter, with explicit unavailability.
6. Keep the 1000 Hz scheduling tick and existing IPC deadline clock unchanged.
7. Measure timestamp overhead and repeated copied/chunked-copy versus Buffer workloads at several payload sizes.
8. Run host tests, both boot paths, comparable resource measurements, and independent review.

HPET supplies one counter across CPUs and avoids unverified TSC synchronization assumptions.
The existing firmware TSC fallback counts queries and is unsuitable for measurement.
TSC optimization remains deferred until frequency stability, cross-CPU consistency, migration, ordering, and calibration have evidence.
BIOS currently disables HPET for hardware compatibility; measurement reports unavailable there.
No periodic interrupt increase or timer-deadline semantic change is planned.

## Impact assessment

| Area | Requirements and effects |
| --- | --- |
| Scope | GOALS: simplicity, testability, performance stance. Fix existing boundaries before adding USB support. |
| Boot | Architecture: Modern Platform Support. BSP and AP initialization must reject missing exception stacks on both boot paths. |
| Memory | Architecture: Virtual Memory. Allocate one existing-purpose stack page per CPU; handle exhaustion without an invalid IDT. |
| Concurrency | Architecture: Scheduler Model. Failed APs stay offline; measurement serializes shared-counter samples across CPUs. |
| Interfaces | IPC ABI: preserve deadline clock; add a separate versioned measurement record and explicit unavailable status. |
| Resources | Record image growth, stack allocation behavior, whole-workload durations, timestamp overhead, and transport batch timings. |
| Validation | Architecture: Test And CI Architecture. Add boundary regressions, retain all guest workloads, and reject incomplete settling. |

Device protocols, BootInfo, storage, credentials, and Buffer ownership contracts remain unchanged.
The benchmark may need initrd space; preserve staging bounds and all existing regression programs.
Physical qualification remains separate from QEMU and host evidence.
The comparison baseline is `7e330ea`; evidence resides in `build/reliability-clock-evidence/`.
The full ASD-STE100 standard remains unavailable; full language compliance cannot be claimed.


## Implemented behavior

BSP and AP setup use the same exception-stack allocator.
Allocation failure halts that CPU before kernel IDT loading; a failed AP never publishes its online flag.
The IDT loader independently rejects a missing stack in release builds.
The early BSP descriptor setup remains separate and does not install the kernel IDT.
Host tests cover exhausted allocation for both CPU records, distinct stacks, and repeated initialization without another allocation.
Privileged IDT installation and the failure halt are source-reviewed; forced guest allocation failure remains untested.

The smoke runner records the observation end before terminating its child.
Three previously successful early-EOF cases now fail: immediate exit, delayed exit, and closed output with a live process.
Seven script tests also cover successful settling, forbidden output, missing markers, and zero-length settling.

The [measurement ABI](measurement-clock-abi.md) adds operation 27 without changing existing operation numbers.
A 64-bit HPET counter supplies the timestamp; unsuitable sources produce an explicit unavailable result.
Host tests cover invalid capabilities, stalled startup, fractional conversion, rollback, overflow, output faults, and deadline-clock independence.
QEMU UEFI exercises the real MMIO source; BIOS exercises explicit unavailability.

## Validation

| Check | Result |
| --- | --- |
| Host suite | 317/317 pass |
| Root integration | 15/15 pass in 55.24 seconds |
| Smoke-settle regressions | 7/7 pass; three failed before the fix |
| IST regression | Exhaustion failed with the extracted old behavior; both final stack tests pass |
| Repeated observation workload | Six final passes, three per boot path |
| Image checks | Kernel staging, initrd size, execution ranges, and firmware/disk bounds pass |

The root matrix includes 12 QEMU smokes and three script contracts.
Local Unix sockets require sandbox escalation for monitor and no-UART checks.
No test or image-bound assertion was disabled.
The before/after logs remain in the evidence directory.
No physical-device tests were run.

```sh
cmake --build build-host-tests -j4
ctest --test-dir build-host-tests --output-on-failure
python3 tests/scripts/test_smoke_settle.py
cmake --build build/buffer-validation -j4
ctest --test-dir build/buffer-validation --output-on-failure
ctest --test-dir build/buffer-validation -R '^os1_smoke_observe(_bios)?$' --output-on-failure
```

## Comparable measurements

Both revisions use Debug, kernel `-Os`, allocator debugging, debugcon, q35, qemu64, TCG, four CPUs, and 128 MiB.
GCC 11.2.0, QEMU 11.1.3, CMake 4.4.5, and the firmware files are unchanged.

The baseline reuses the completed passive ACPI phase's final artifacts and three timing samples per boot path.
Original records are `build/acpi-passive-evidence/final-artifacts.json` and `build/acpi-passive-evidence/final-times.json`.
The baseline environment record identifies this reuse and preserves the prior comparison's revision label separately.
It contains firmware hashes and host details.
Artifact manifests record the final image hashes.

Whole-workload values are medians of three successful samples per boot path for each revision.
It includes boot, CTest, shell commands, and settling; it does not isolate scheduler or syscall latency.

| Boot path | Baseline seconds | Final seconds | Difference seconds | Difference percent |
| --- | ---: | ---: | ---: | ---: |
| UEFI | 6.448746 | 6.517760 | +0.069014 | +1.070% |
| BIOS | 6.601938 | 6.608297 | +0.006359 | +0.096% |

| Artifact | Baseline bytes | Final bytes | Difference bytes | Difference percent |
| --- | ---: | ---: | ---: | ---: |
| Kernel ELF | 455744 | 455928 | +184 | +0.04037% |
| BIOS kernel ELF | 340480 | 340480 | 0 | 0% |
| Initrd | 195584 | 197120 | +1536 | +0.78534% |

Each artifact uses one build per revision.
The expanded benchmark exceeded the old initrd bound by 512 bytes.
The staging boundary moves from `0x6A000` to `0x69000`, transferring 4096 bytes from the kernel slot to the initrd slot.
The new kernel slot has 712 sectors; the initrd slot has 392 sectors.
Kernel LBAs are 65–776; initrd LBAs are 777–1168.
The firmware ceiling remains `0x9A000`, and the boot-log locations remain unchanged.

The BIOS kernel retains 24064 bytes of slot capacity; the initrd retains 3584 bytes.
No regression program was removed.
Stack allocation remains one page per initialized CPU; only failure handling changes.
The measurement clock adds fixed state and one IRQ-safe lock, with no dynamic allocation per query.
Host CPU use, peak RSS, and isolated boot-lookup costs remain unmeasured.
Dedicated resource instrumentation is required to measure them.

### IPC benchmark results

The UEFI guest reports a nominal HPET resolution of 10 ns.
The minimum elapsed time across 32 back-to-back timestamp pairs is 1000 ns.
This estimate includes syscall and virtualization overhead; it does not establish 10 ns measurement accuracy.
Each row below is the median of three samples in one guest run.

| Logical bytes | Repetitions | Copied calls | Buffer calls | Copied total ns | Buffer total ns | Difference ns | Difference percent |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 252 | 256 | 256 | 256 | 21398000 | 55958000 | +34560000 | +161.510% |
| 4096 | 64 | 1088 | 64 | 91830000 | 15912000 | -75918000 | -82.672% |
| 65536 | 8 | 2088 | 8 | 176604000 | 4290000 | -172314000 | -97.571% |

A reusable 65536-byte Buffer occupies 16 pages for every row, including the small logical payload.
Copied transport assembles and returns equivalent byte ranges in 252-byte chunks.
The larger rows compare many copied calls against one loan per repetition; they do not isolate payload-copy cost.
The benchmark alternates transport order and verifies output bytes outside the timed interval.
It excludes initial allocation, initial mapping, input initialization, and final verification.
It includes request assembly, synchronous calls, result delivery, and descriptor cleanup.

BIOS reports high-resolution timing unavailable and still runs all Buffer correctness checks.
The benchmark does not substitute tick timestamps or the firmware TSC pseudo-clock.
Cold allocation cost, asynchronous throughput, energy, and physical-device latency remain unmeasured.

## Independent review

The first review found obsolete BIOS sector ranges in Architecture; these ranges are corrected.
No functional defect was confirmed in that review.
Final independent review approved the corrections and measurement report; no blocking findings remain.


## Combined commit validation

The final check also includes the subsequent USB workflow changes and launcher rename to `os1.sh`.
The earlier 15-case integration result above remains historical evidence for the reliability phase.

| Check | Final result |
| --- | --- |
| Host tests | 317/317 passed |
| Root integration | 16/16 passed in 55.56 seconds: 12 QEMU smokes and four script contracts |
| USB helper cases | 18/18 passed within the USB menu contract |
| Build and whitespace | Cross-build current; `git diff --check` passed |
| Secret scan | Only password-prompt text, parser terminology, and recorded artifact hashes matched |

The final run uses the same toolchain and debugcon-enabled configuration as the earlier reliability checks.
No physical USB write was performed for validation.
The user subsequently reported that the sudo prompt presentation looks correct; this does not establish image-write or hardware-boot success.

Formatter comparison covered all 13 changed or new C/C++ files.
Three files retain pre-existing formatting violations outside the edited code: `cpu.cpp`, `hpet.cpp`, and `cpu_record_tests.cpp`.
No new formatter violations were found; unrelated formatting was left unchanged.

Integrated review identified stale script-contract totals in README and Architecture; these are corrected to four contracts.
Independent review approved the integrated changes and corrected documentation; no blocking findings remain.
