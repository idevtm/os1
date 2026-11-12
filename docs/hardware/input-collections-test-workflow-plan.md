# Compact input collections and hardware acceptance

This change separates live collection positions from retained attachment identities.
It also adds reference comparison and automatic diagnostic testing.

```text
attachment identity -> compact typed collection -> open -> retained device handle
Linux captures -> versioned profile -> offline comparison <- captured OS1 results
diagnostic boot -> first shell startup -> run-tests -> sync -> interactive prompt
```

## Accepted contract

`os.input.keyboards` and `os.input.pointers` expose connected devices and a read-only `count`.
Zero-based indexing selects the current position: `os.input.pointers[0].open`.
The one-based `keyboardN` and `mouseN` aliases remain available for short commands.
Removal compacts both views; reconnect assigns a new attachment identity.
An open handle never changes its attachment when an index or alias changes.
Disconnected handles remain valid for state inspection until released.
Existing registry admission and USB DMA retention limits remain unchanged.

A generation protects each enumeration; changes cancel stale pages.
Discovery does not instantiate a device or acquire input.
The initial shell supports literal decimal indexes; computed indexes remain future language work.
Future game controllers use `os.input.controllers`; cameras use `os.media.cameras`.
These future collections do not claim implemented drivers.

## Impact assessment

| Area | Requirements and effect |
| --- | --- |
| Scope | GOALS, Platform discovery and Testability and observability: deliver compact discovery and repeatable physical acceptance. |
| Boot/platform | ARCHITECTURE, Input services and attachment changes: preserve both firmware paths. Diagnostic startup runs once per boot. |
| Memory/isolation | Input ABI, Authority and discovery: retain identity-bound handles and current limits. Bound offline input and archive processing. |
| Concurrency | Protect collection counts and positions with the input lock. Preserve generation cancellation and detached handle semantics. |
| Interfaces | Extend passive input node identifiers and indexed shell paths. Preserve native input acquisition and event layouts. |
| Runtime resources | Measure common workloads, image sizes, diagnostic startup cost, and captured bytes against revision c2fbfd1. |
| Validation/documents | Cover host parsing, compaction, held handles, hotplug, offline mismatches, and diagnostic startup on BIOS and UEFI. |

`BootInfo`, page ownership, driver DMA reclamation, hardware decoding, filesystem layout, and device authority remain unchanged.
SMBIOS remains deferred; reference matching uses captured numeric hardware identity.

## Implementation sequence

1. Add typed collections, counts, compact aliases, indexed paths, and Tab navigation.
2. Add structured live hardware capture for machine-readable comparison.
3. Normalize the existing Linux references into reviewed machine profiles.
4. Add an offline comparison command with machine selection, mismatches, unsupported devices, and unexpected-device reports.
5. Start `run-tests` once during the first diagnostic shell startup.
6. Test failure handling, shell restart, normal boot, and preserved results.
7. Run the complete local suite, compare resources, and obtain independent review.

## Reference comparison

Keep Linux source captures immutable and record their hashes in each profile.
Match PCI vendor, device, class, and location; identify USB devices by vendor and product identifiers.
Do not compare Linux virtual root hubs, loop disks, driver names, or event-node counts as physical devices.
USB controller routing can differ between operating systems.
Profiles distinguish required hardware, optional attachments, required functions, and known unsupported functions.
Ambiguous or unknown machine matches remain inconclusive.
Record the profile hash, capture build identity, comparison version, and test outcomes in the report.
Do not infer functionality from enumeration alone.

The current runner checks input APIs and storage operations.
Shell test groups and supervised arbitrary verification programs remain follow-up work.
The comparison report must include existing test failures rather than only hardware matches.

## Diagnostic startup

The diagnostic boot selection starts the noninteractive test group before the first shell prompt.
Normal boot, nested shells, and restarted top-level shells do not start another automatic run.
Failure returns to an interactive prompt and preserves incomplete evidence.
Unavailable disk programs use the existing rescue path; they cannot claim a completed test run.
Physical stimulus observation is enabled by default; `run-tests --no-input` disables it.
Diagnostic watchdog deadlines remain a limit on unattended execution.

## Evidence

Baseline revision: `c2fbfd1`.
Saved baseline artifacts: `build/input-collections/baseline/`.
The full ASD-STE100 dictionary is unavailable; new prose receives manual technical review.

## Operator workflow

Use these commands to inspect the current input collections:

```lua
inspect os.input.keyboards
os.input.keyboards.count
inspect os.input.pointers
p = os.input.pointers[0].open
p.get_state
s = p.subscribe
s.try_read
s.unsubscribe
drop s
drop p
```

Type `os.input.pointers.` to display collection children.
Use Up and Down to select a child.
Press Tab to insert its indexed path and continue discovery.
Reconnect can change positions; retain an open object when attachment identity must remain fixed.

Select `os1 diagnostics` to run the default acceptance group before the first prompt.
The runner records results under `/disk/tests` and returns control to the shell.
Use `run-tests` for an additional, supervised physical-input window.
The automatic group does not establish that a person moved each pointer or pressed each key.

After copying the ext2 data partition, run the offline comparison:

```sh
python3 tools/compare_hardware.py --image build/returned-data.ext2 --output build/hardware-comparison.json
```

For an extracted results directory, use this command:

```sh
python3 tools/compare_hardware.py build/returned-tests --output build/hardware-comparison.json
```

The command does not write to the input image.
Image inspection requires `debugfs`; direct directory inspection uses Python's standard library.
The image must contain an ext2 filesystem, not the full boot disk layout.
Version 1 runner archives have no `catalog.bin`; their hardware comparison remains inconclusive.
Existing failed summaries remain failures even when the catalog is missing.
Use a new capture to compare those machines with the current profiles.

An explicit `--machine laptop` or `--machine desktop` selects a reference without automatic matching.
This override does not suppress hardware mismatches.

| Outcome | Meaning |
| --- | --- |
| PASS | A required comparison or implemented API test passed. |
| FAIL | Required hardware, classification, API checks, or result persistence failed. |
| REVIEW | A device is unexpected, or its PCI location changed. |
| SKIP | An optional reference attachment is absent. |
| UNSUPPORTED | Detection has no bound driver, or the reference function has no implemented test. |
| INCONCLUSIVE | Evidence is incomplete, invalid, unknown, or ambiguous. |

Exit status 0 means all runs have overall PASS status.
Exit status 1 means at least one run failed.
Exit status 2 means review, inconclusive evidence, or an input error.
A PASS does not qualify functions marked UNSUPPORTED.
Internal storage models remain reference information; current captures do not establish their functional identity.
CPU and memory records are captured, but this comparison version does not validate them against Linux inventory.

## Structured catalog format

The archive contains `catalog.bin` alongside human-readable reports.
All integer fields use little-endian encoding.
The file contains one 32-byte header followed by at most 160 records.

| Header offset | Bytes | Field |
| --- | ---: | --- |
| 0 | 8 | `OS1CAT1` followed by a zero byte |
| 8 | 4 | Format version, currently 1 |
| 12 | 4 | Record size, currently 360 |
| 16 | 4 | Record count |
| 20 | 4 | Complete flag; 1 only after all queries succeed |
| 24 | 8 | Hardware catalog generation |

| Record offset | Bytes | Field |
| --- | ---: | --- |
| 0 | 16 | Node identity, physical parent, source kind, and backing identity; four `u32` values |
| 16 | 8 | Known-field mask; original hardware field positions |
| 24 | 176 | Hardware fields 8 through 29; 22 `u64` values |
| 200 | 96 | Zero-terminated name |
| 296 | 32 | Zero-terminated state |
| 328 | 32 | Zero-terminated driver name |

The maximum file size is 57,632 bytes, within the current 65,536-byte temporary-file limit.
The capture includes platform, PCI, USB, block, partition, filesystem, and mount records.
It excludes the full ACPI namespace to keep the file bounded.
All queries share one generation; a topology change makes the capture incomplete.
The offline reader rejects incomplete files, duplicate identities, invalid layouts, and unterminated strings.
It reads bounded regular tar members without extracting archive paths or links.

## Remaining limits

Compact names do not remove the 64-attachment admission limit or reclaim retained USB DMA resources.
Detached registry entries remain until reboot; open descriptors remain detached until their owners release them.
Device generations protect enumeration, but separate count and open commands do not form one transaction.
Diagnostic startup adds test latency before the prompt and creates another persistent run directory.
Storage exhaustion remains a reported failure; test results are not silently overwritten.

## Failure evidence and corrections

| Evidence | Finding and correction |
| --- | --- |
| `build/input-collections/smoke-second.log` | Diagnostic startup compared a returned byte count with zero. The caller now checks the expected record size. |
| First independent review | USB interface associations used field 35 instead of field 37. A real USB archive check now verifies backing identities. |
| First independent review | Right-aligned Linux storage sizes broke column slicing. Reference tests now assert the physical disk models and sizes. |
| First independent review | Profiles and input records accepted incomplete or invalid data. Required hashes, types, identities, states, and duplicates now receive checks. |
| Second independent review | Named pipes could block offline reads. Nonblocking opens and regular-file checks prevent this failure. |
| `build/input-collections/first-suite/` | The full run passed 657 host checks and three diagnostic checks. Three of 92 normal checks failed. |
| Observation failures | Constructor metadata reads added startup traffic before the first prompt. Init now supplies the known empty shell constructor directly. |
| Native-input failure | A driver readiness message interrupted parsed shell output. The test now consumes that message before inspecting fields. |

The first full run took 20.59 seconds for host checks and 488.65 seconds for normal-image checks.
Diagnostic checks took 9.91 seconds.
Source corrections overlapped this initial run; it does not establish final-source validation.
The original observation logs retain the missing boot-event markers.
The original native-input log retains the interleaved readiness line.

A second full run exposed the same output race in `os1_smoke_usb_failure_bios`.
The test timed out because the storage-ready message split the shell prompt.
The log also contains expected file and synchronization failures from the deliberate device removal.
Its evidence remains in `build/artifacts/test-runs/os1_smoke_usb_failure_bios-54vv1ih2/`.
All three hotplug test programs now consume the relevant readiness line before parsing shell output.
Their acquisition, state, persistence, and detached-handle assertions remain unchanged.
The kernel still permits asynchronous diagnostic text on the serial stream.
The tests synchronize with that known output instead of assuming an uninterrupted prompt during attachment.

## Resource comparison

The [measurement record](2026-11-10-input-collections-measurements.json) contains samples, hashes, commands, and the measurement script.
Baseline source revision is `c2fbfd131065299f5fa26d174466885b64f92a0a`.
Saved baseline binaries still embed revision `8903070`; their source/build hash matches the baseline source.
The baseline hash is `d9271e6cf2f68603dbab39abb9d485202990cbefd6ed45aae9caf23c615ffd13`.
The candidate hash is `ffbeb80f2b64e0fe8ba80e2b88b5858e83f42511fa2979a50049a06b49ca0b2b`.

Measurements ran without concurrent QEMU tests, with three alternating baseline/candidate repeats per firmware.
The host used macOS 26.6.2, an Apple M4 Max, 16 logical CPUs, and 64 GiB RAM.
Tools were GCC 11.2.0, CMake 4.4.5, and QEMU 11.1.3.
Guests used Q35, TCG, four CPUs, 128 MiB RAM, and virtio storage.
Builds used Debug, allocator diagnostics, debugcon, and the normal boot profile.
Firmware was bundled SeaBIOS or the recorded Homebrew EDK2 code and variables.

The common workload executes 20 `context os.input.keyboard1` commands and one default `run-tests` command.
Memory observations surround that workload.
A separate UEFI workload selects the diagnostic menu entry and measures time to the first prompt.
These measurements include shell dispatch, serial output, and framebuffer updates.
They do not isolate driver or IPC latency.

| Firmware | Metric, seconds | Baseline median | Candidate median | Absolute change | Relative change |
| --- | --- | ---: | ---: | ---: | ---: |
| BIOS | First prompt | 0.4587 | 0.4646 | +0.0059 | +1.28% |
| BIOS | 20 context commands | 1.9833 | 1.9738 | -0.0095 | -0.48% |
| BIOS | Default test command | 1.9653 | 1.9951 | +0.0298 | +1.51% |
| BIOS | QEMU CPU, complete workload | 4.4301 | 4.5071 | +0.0770 | +1.74% |
| UEFI | First prompt | 1.9549 | 1.9616 | +0.0067 | +0.34% |
| UEFI | 20 context commands | 1.0654 | 1.0470 | -0.0184 | -1.73% |
| UEFI | Default test command | 1.8516 | 1.9072 | +0.0556 | +3.00% |
| UEFI | QEMU CPU, complete workload | 4.6190 | 4.6141 | -0.0049 | -0.11% |
| UEFI diagnostics | First prompt, including automatic tests | 1.9824 | 3.6895 | +1.7071 | +86.11% |

No performance improvement is claimed from three repeats.
The diagnostic delay includes the new automatic run; the baseline does not run tests before its prompt.
Before the common workload, candidate guests use three additional 4096-byte pages, or 12,288 bytes.
Used-page increases are 0.260% on BIOS and 0.054% on UEFI.
The workload retains 37–38 pages across result metadata, logs, and allocator capacity in both versions.
Peak process memory remains unmeasured because the current API does not expose a peak counter.

| Artifact | Baseline bytes | Candidate bytes | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| Kernel ELF | 614440 | 614952 | +512 | +0.083% |
| Initrd | 7680 | 10240 | +2560 | +33.33% |
| BIOS image | 1474560 | 1474560 | 0 | 0% |
| UEFI image | 4349952 | 4352000 | +2048 | +0.047% |
| Program disk capacity | 34603008 | 34603008 | 0 | 0% |

The runner's text, data, and BSS total 110057 bytes, up from 109049 bytes: +1008 bytes, or +0.924%.
This count excludes stack and temporary-file allocations.
The initrd increase carries the initial shell argument envelope; the constructor metadata parser is not required for that launch.
Both image layouts remain within their checked size limits.


A separate UEFI comparison measured one fresh default run per revision with the same machine label.
Saved file bytes increased from 110162 to 115868: +5706 bytes, or +5.18%.
The new catalog contained 14 records; its padded archive member accounts for 5632 added bytes.
Manifest and progress fields account for the remaining 74 bytes.
Each version creates five files; these sizes exclude ext2 allocation overhead.

## Final validation and review

`./scripts/check-local.sh` passed on the final executable and test tree.
The validated tree contains baseline revision `c2fbfd1` plus this task's changes.

| Profile | Result | Duration |
| --- | --- | ---: |
| Host suite | 657/657 passed, including 14 offline comparison cases | 19.96 seconds |
| Unfiltered normal-image suite | 92/92 passed | 458.17 seconds |
| Diagnostic-image suite | 3/3 passed | 9.76 seconds |

The command output is `build/input-collections/full-local-verified.log`.
Copied test logs are in `build/input-collections/verified-suite/`.
The full runs include BIOS, UEFI, four-CPU guests, hotplug, retained handles, actual archives, and failed diagnostic startup.
No checks were disabled or assertions removed.
The six focused regressions also passed in 79.46 seconds before the final readiness correction.

Independent read-only reviews checked the complete diff and each correction.
Six confirmed capture and input-boundary findings received fixes and regression coverage.
The runtime corrections retained all functional assertions and received a separate review.
A documentation correction distinguished expected removal errors from the test's prompt-parsing timeout.
Review found no remaining source blockers.

Document file links, Python syntax, scoped formatting, whitespace, and credential signature checks passed.
The credential review treated build hashes and numeric hardware identities as expected data.
The complete ASD-STE100 dictionary was unavailable; technical prose received manual review.
The current hardware result is unmeasured on both physical devices.
The next validation step is diagnostic boot on each device, followed by returned-stick comparison and optional input stimulus.

## Returned hardware follow-up

The [hardware correction record](input-hardware-fixes-plan.md) records the laptop and desktop runs from November 10, 2026.
Both PCI comparisons pass, but the runs expose report truncation and separate USB pointer failures.
Normal-mode captures do not establish diagnostic autorun or physical stimulus delivery.
