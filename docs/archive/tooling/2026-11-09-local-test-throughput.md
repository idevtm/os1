# Local test throughput

This work reduces complete local validation time while retaining all test assertions and storage transports.
The baseline normal-image suite passed 79 checks in 616.92 seconds at `5813863`.

```text
private test files -> two concurrent guests -> repeated complete runs
                    -> readiness and parsing changes -> separate storage profile
```

## Requirements and impact

| Area | Assessment |
| --- | --- |
| Scope | GOALS: Testability and observability, Performance stance. Keep complete local validation and simple test ownership. |
| Boot/platform | Architecture: CI and Boot profile selection. Retain BIOS, UEFI, normal, and diagnostic images. |
| Memory/isolation | Give each invocation private mutable images, firmware variables, logs, and sockets. Retain failed fixtures. |
| Concurrency | Replace blanket serialization with bounded two-test scheduling. Audit shared inputs before removing locks. |
| Interfaces | Preserve native ABI profiles, guest commands, expected markers, and failure assertions. |
| Resources | Measure elapsed time, preparation cost, child CPU time, and storage phases. Two guests increase concurrent host resource use. |
| Validation/docs | Run complete matrices repeatedly, preserve every failure, add host contracts, and obtain independent review. |

## Ordered plan

1. Add a common invocation wrapper with private files and retained evidence.
2. Run two CTest jobs and repeat the entire matrix before changing pacing or parsing.
3. Replace command pacing with prompt readiness and reduce repeated parsing work.
4. Profile the six EHCI tests separately, including image preparation, boot, commands, and inspection.
5. Run final full local validation and review corrections independently.

Do not substitute xHCI for EHCI in this task.
The earlier transport experiment failed UEFI event-retention assertions.
Do not remove countdown, timeout, or background-drain coverage to improve timing.
Historical intermittent recursive-copy and rescue log-export failures remain separate investigation items.

## Evidence

The isolation stage passed all 80 checks twice with two CTest jobs.
Elapsed times were 316.78 seconds and 313.69 seconds.
The mean was 315.24 seconds, 301.69 seconds below the 616.92-second serial baseline.
This is a 48.90 percent elapsed-time reduction.
The new isolation contract adds one check to the baseline's 79 checks.
No observed failure required a serial exception.

Both runs used unchanged guest code and the existing storage transports.
CTest changed test order based on its recorded costs.
These two passes do not resolve historical intermittent guest failures.
The evidence does not qualify more than two concurrent tests.

| Paired BIOS workload | Before mean | After mean | Absolute difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| Observe elapsed | 10.031 s | 7.894 s | -2.137 s | -21.30% |
| Spawn elapsed | 20.693 s | 18.315 s | -2.378 s | -11.49% |
| Observe parsing CPU | 1.214 s | 0.271 s | -0.943 s | -77.63% |
| Spawn parsing CPU | 0.651 s | 0.110 s | -0.541 s | -83.10% |
| Observe output chunks | 6598 | 974 | -5624 | -85.24% |
| Spawn output chunks | 2537 | 451.5 | -2085.5 | -82.20% |

Each row uses two alternating before/after pairs with identical guest images and marker assertions.
Elapsed time includes the private-input wrapper.
Parsing CPU measures normalization and compaction through the same measurement wrapper in both versions.
The old runner requested 2.4 seconds of observe pauses and 4.6 seconds of spawn pauses.
The new runner requested zero command pauses and checked prompt readiness instead.

QEMU CPU time averaged 8.344 to 7.805 seconds for observe and 21.171 to 22.155 seconds for spawn.
Faster elapsed time does not imply lower guest CPU use for every workload.

The targeted readiness suite passed all 11 BIOS, UEFI, and contract checks in 39.80 seconds.
The smoke-runner contract passed 19 cases, including three new readiness cases.
Those cases cover stale prompts, fragmented prompts, command batches, and absent prompts.
Existing rejection, EOF, UTF-8, and final-settling checks remain enabled.

## EHCI storage profile

All six checks passed in 186.56 seconds during one separate serial run.
The historical baseline was 182.31 seconds; this run was 4.25 seconds higher, or 2.33 percent.
This single-run comparison does not establish a storage performance regression or improvement.
No storage transport, guest implementation, timeout, or assertion changed.

| Test | Execution | Startup | Prelude | Workload | Host remainder |
| --- | ---: | ---: | ---: | ---: | ---: |
| Data BIOS | 47.662 s | 19.510 s | 2.656 s | 23.555 s | 1.929 s |
| Data UEFI | 39.162 s | 12.352 s | 0.237 s | 25.101 s | 1.457 s |
| Failure BIOS | 22.859 s | 3.172 s | 3.473 s | 15.833 s | 0.369 s |
| Failure UEFI | 31.444 s | 12.273 s | 0.246 s | 18.540 s | 0.366 s |
| Storage BIOS | 20.705 s | 1.598 s | 2.167 s | 16.544 s | 0.390 s |
| Storage UEFI | 24.495 s | 5.868 s | 0.119 s | 18.094 s | 0.406 s |

Execution totals 186.326 seconds inside the wrappers.
Startup totals 54.774 seconds, or 29.40 percent of execution.
Workload totals 117.667 seconds, or 63.15 percent.
Prelude totals 8.898 seconds; cleanup totals 0.070 seconds.
Host preparation and inspection total 4.916 seconds, or 2.64 percent.
Wrapper startup and CTest overhead explain the remaining difference from 186.56 seconds.

Child CPU time totals 243.387 seconds across these six tests.
Guest CPU work can exceed elapsed time because each guest has four virtual CPUs.

The data checks boot the combined USB image four times.
Their startup cost includes loading the operating system through the tested USB path.
The plain storage checks boot twice and perform explicit filesystem consistency checks.
Failure checks retain removal, reconnect, read-only rejection, controller timeout, and keyboard coexistence coverage.
Command durations include native IPC, filesystem work, scheduling, and command synchronization.
They do not measure raw USB bandwidth.

Keep these workloads intact for correctness coverage.
A future storage optimization should measure `diskutil` phases and EHCI transfer latency before changing transport behavior.
Boot-loading costs require separate analysis from ordinary file operations.

## Final validation

`./scripts/check-local.sh` returned status zero on the final implementation.
All 590 host tests, 80 normal-image checks, and three diagnostic-image checks passed.

| Profile | Serial baseline | Final elapsed | Absolute difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| Host | 19.68 s | 19.14 s | -0.54 s | -2.74% |
| Normal images | 616.92 s | 309.13 s | -307.79 s | -49.89% |
| Diagnostic images | 10.42 s | 5.79 s | -4.63 s | -44.43% |

These times exclude configuration and compilation.
The host workload remains unchanged; its small difference does not establish an optimization.
The complete normal matrix passed twice before readiness changes and once after all implementation changes.
The final normal result is one run; longer-term timing stability remains unmeasured.
All six strategic CI tests remain registered with the `ci` label.
No assertion, timeout coverage, or negative test was removed.

The normal and diagnostic tests cover BIOS and UEFI, including existing four-CPU SMP workloads.
The two isolation contracts check private inputs, argument preservation, original-file retention, and failure evidence.
The local-runner contract verifies two-job scheduling for both QEMU profiles.
Shell syntax, Python syntax, local document link targets, and `git diff --check` passed.
No project-owned C or C++ source changed; no source formatting was required.

Commands:

```sh
# Repeat twice before readiness changes.
ctest --test-dir build --parallel 2 --output-on-failure --no-tests=error

# Check the smoke-runner boundary.
python3 -I -B tests/scripts/test_smoke_settle.py

# Profile storage without competing guests.
ctest --test-dir build --parallel 1 --output-on-failure --no-tests=error \
  -R '^os1_smoke_ehci_(storage|data|failure)_(bios|uefi)$'

# Validate the final implementation.
./scripts/check-local.sh
```

All observed guest and contract runs passed.
The negative contract cases intentionally exercised child failure without failing their containing test.
Physical-device behavior and hosted GitHub duration remain unmeasured because this task changes host test tooling.
The next hosted run can check the unchanged six-test selection.

## Configuration and resource tradeoffs

Base revision: `581386333d9fef10d4cfca6ea1c23e49ba3adc1e`, plus this task's working-tree changes.
Host: Apple M4 Max, sixteen physical cores, 64 GiB RAM, macOS 26.6.2.
Tools: CMake 4.4.5, Python 3.15.2, QEMU 11.1.3, and GCC 11.2.0.
Guests retain q35, four virtual CPUs, TCG, and 128 MiB RAM.
Two concurrent guests require up to eight guest CPU threads and 256 MiB configured guest RAM, plus emulator overhead.

Normal images enable allocator checks and debugcon; diagnostic capture disables debugcon.
UEFI uses the Homebrew `edk2-x86_64-code.fd` and `edk2-i386-vars.fd` files.
Initial firmware and image hashes are recorded in `implementation-baseline.json` in the evidence directory.
Final hashes are recorded in `final-artifacts.json`.

No guest behavior, native ABI profiles, disk layouts, or storage driver code change.
Rebuilding updates the embedded log build identity, which changes kernel and image hashes.
The normal ISO remains 4,311,040 bytes; the BIOS image remains 1,474,560 bytes.
The 34,603,008-byte program fixture and firmware files retain their initial hashes.
Source-file hashes are retained in `implementation-source.json`.

Private fixture copies add temporary disk use and preparation I/O.
Successful runs remove their binary fixtures; failed runs retain them.
Logs and timing records accumulate until explicitly removed.
The completed normal test directories occupied approximately 15 MiB after fixture cleanup.
Host peak resident memory and peak temporary disk use were not sampled.
Measure those peaks before qualifying additional concurrent guests.

The readiness buffer retains at most 1,048,576 characters per generic smoke runner.
Unicode storage and temporary string copies require additional host memory.
Serial batching adds at most 64 KiB and a five-millisecond partial-line collection interval.
The matcher retains its existing 1,048,576-character history and rejection rules.
No physical device or hosted CI performance was measured for this host-tooling change.

## Evidence locations and review

Evidence is under `build/local-duration-analysis/`:

- `isolation-full-1.log` and `isolation-full-2.log`: complete parallel runs.
- `isolation-full-1-detail.log` and `isolation-full-2-detail.log`: invocation evidence locations.
- `readiness-smokes.log`: targeted BIOS and UEFI checks.
- `readiness-comparison.json` and `parser-profile.json`: paired parser measurements.
- `ehci-profile.log` and `ehci-profile.json`: separate serial EHCI run and phase totals.
- `final-local.log`: complete final validation command output.
- `implementation-source.json`: source-file hashes for the validated implementation.

Final CTest summaries remain under `build/local-validation/`.
Per-invocation files remain under `build/artifacts/test-runs/`.
The first independent review found obsolete serial-execution and fixed-log-path documentation.
The README, testing guide, and architecture now describe private evidence and two-test scheduling.
A second independent review found no implementation defects.
The closing review verified the final source hashes, test logs, timing calculations, and documentation correction.
It found no blocking defects.
The reviewer did not run tests or edit files.

The complete ASD-STE100 dictionary is unavailable; full language compliance remains unverified.
