# Passive ACPI observation

This change removes firmware execution from ordinary ACPI observation.
It addresses review finding R2 without adding a user-facing power-control interface.

```text
Boot discovery -> bounded route cache -> observe snapshot -> acpiprobe
Power control  -> existing kernel platform operations only
```

## Contract and plan

1. Reproduce observation-triggered power changes with real ACPICA methods in a host regression.
2. Remove power-method selection and execution from the snapshot path.
3. Complete missing enumerated-device routes during boot, then read only the published cache.
4. Preserve record layout and return zero in both legacy power counters.
5. Report unavailable cached routes without treating inventory absence as a command failure.
6. Require passive output in both observation smokes.
7. Run host checks, both boot paths, comparable measurements, and independent review.

The route counters describe cached lookup attempts and matches for enumerated PCI interrupt pins.
A missing cached route does not authorize firmware evaluation.
The power counters remain reserved-zero fields with their existing names and offsets.
No ABI version or record size changes.
Clients must rebuild to receive the revised human-readable output.

Explicit power diagnostics remain deferred.
They require separate authority, driver quiescence, serialization, and recovery after a failed resume.
Existing kernel suspend and resume functions retain their current behavior.

## Impact assessment

| Area | Requirement and effect |
| --- | --- |
| Project scope | GOALS: Security model; simplicity. Observation loses an unintended control operation. |
| Boot and platform | Architecture: Modern Platform Support. Boot resolves missing enumerated-device routes before user execution; runtime snapshots read cached data on both boot paths. |
| Memory and isolation | Architecture: Observability ABI. Existing bounded user-copy validation and record layout remain unchanged. |
| Concurrency | Architecture: Scheduler Model. Read-only cached lookup removes runtime AML calls; boot publishes these records before user execution. |
| Interfaces | Observe UAPI and Architecture: Observability ABI. Legacy power counters become reserved zero; command output explicitly identifies passive behavior. |
| Runtime resources | GOALS: Performance stance. Measure comparable observation workloads and artifact sizes; snapshots avoid firmware work and allocate no new state. |
| Validation and documents | GOALS: Testability and observability. Real ACPICA regression, boundary tests, dual-boot smokes, and independent review cover the change. |

BootInfo, page ownership, scheduler policy, descriptors, IPC, storage protocols, and network protocols do not change.
The shared kernel and user program change, so both boot paths require validation.
No physical power transition is needed to reproduce the defect.

## Corrections and regression evidence

The original snapshot changed a synthetic device's AML `PWST` value from zero to one.
This occurred for bound devices, unbound fallback devices, and a buffer whose header fit but whose record copy failed.
Four initial regressions failed before correction and passed afterward.
The tests call the real syscall, user-copy implementation, and ACPICA interpreter.
They do not replace power methods with mocks.

The first guest checks exposed an empty boot route cache on both QEMU boot paths.
The former snapshot obtained routes through the live resolver, so simply removing that fallback lost the required route output.
The boot path now resolves missing routes for enumerated PCI interrupt pins before publishing initialized platform state.
It appends exact-function records within the existing 64-entry bound and skips unresolved routes.
Existing cached matches remain unchanged; repeated population does not duplicate them.

Full caches remain bounded, and unavailable routes remain valid inventory results.
The snapshot never fills this cache or invokes firmware.

The observation smokes retain their required `route ok` marker.
They now additionally require `passive snapshot power control disabled`.
This checks passive behavior without accepting missing routes on the known QEMU firmware.

## Validation and measurements

The host suite passes 311 tests, including eight new observation and cache tests.
The original four failing regressions are retained in `before-regression.log`.
Coverage includes repeated reads, bound and unbound devices, cache misses, invalid user buffers, empty inventory, route matching, and cache bounds.
Both kernel and initrd image builds pass their existing layout checks.

All 14 integration checks pass, including both observation smokes, SMP, and no-UART coverage.
The full integration run took 52.93 seconds.
Six final observation repeats pass, with three runs per boot path.
Both boot paths report four cached route matches and disabled power control.

Independent review found sentence-length and paragraph-length issues in the README; all were corrected.
Final independent review approved the corrections and measurement report; no blocking findings remain.
The comparison baseline is `7774cae172c281506b2c82e537f36dfc028e7933`.
Evidence resides in `build/acpi-passive-evidence/`.
The complete ASD-STE100 standard and dictionary remain unavailable; full language compliance is unverified.

### Reproduction

```sh
cmake --build build-host-tests -j4
ctest --test-dir build-host-tests --output-on-failure
cmake --build build/buffer-validation -j4
ctest --test-dir build/buffer-validation --output-on-failure
ctest --test-dir build/buffer-validation -R '^os1_smoke_observe(_bios)?$' --output-on-failure
```

The regression log uses `build-host-tests/os1_host_tests --gtest_filter='AcpiObservation.*'`.
The monitor and no-UART checks require local Unix socket access outside the restricted sandbox.
No kernel or firmware assertion was disabled.
The initial cache-related guest failures remain in `initial-integration-tests.log`.

### Comparable resource measurements

Both revisions use Debug, kernel `-Os`, allocator debugging, and debugcon.
QEMU uses `q35`, `qemu64`, TCG, four CPUs, and 128 MiB.
The compiler is GCC 11.2.0; QEMU is 11.1.3; CMake is 4.4.5.
Both revisions use the same firmware files and build directory.
`environment.json` records the host, settings, firmware hashes, and tool versions.
`guest-test-definitions.json` records the exact final test commands.

Each timing value is the median of three successful whole-workload runs.
The workload includes boot, shell commands, CTest overhead, and settling.
It does not isolate syscall latency, firmware execution time, or CPU use.
These small differences do not establish a performance improvement.

| Boot path | Baseline seconds | Final seconds | Difference seconds | Difference percent |
| --- | ---: | ---: | ---: | ---: |
| UEFI | 6.506324 | 6.448746 | -0.057578 | -0.885% |
| BIOS | 6.692856 | 6.601938 | -0.090918 | -1.358% |

| Artifact | Baseline bytes | Final bytes | Difference bytes | Difference percent |
| --- | ---: | ---: | ---: | ---: |
| `kernel.elf` | 455712 | 455744 | +32 | +0.00702% |
| `kernel_bios.elf` | 340480 | 340480 | 0 | 0% |
| `initrd.cpio` | 195584 | 195584 | 0 | 0% |

Artifact sizes use one build per revision; image manifests record SHA-256 hashes.
The unchanged initrd bound leaves 1024 bytes available.
The change reuses the existing bounded route array and adds no persistent cache allocation.
Boot can perform extra route lookups; snapshots no longer perform those lookups or device power transitions.

Guest peak memory, host CPU use, and isolated observation latency remain unmeasured.
A dedicated instrumented workload is required to measure those properties.
No physical-device tests were performed.
The regression uses synthetic firmware; it does not control physical devices.
The tests do not establish complete ACPICA concurrency or power-management correctness.

## Remaining work

The next correctness items are IST allocation failure and smoke-runner settle validation.
The high-resolution clock and IPC measurement plan remains recorded in the [Buffer follow-up](2026-11-05-exclusive-buffer-implementation.md#clock-and-benchmark-plan).
Power diagnostics require a separate design; the current change does not expose them to callers.
