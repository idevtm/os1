# Live input device discovery

This change exposes connected input devices as children of `os.input`.
Each child has an `open` method; discovery does not open devices.

```text
boot / hotplug -> input registry -> os.input.keyboardN / mouseN
context / Tab -> passive descriptions -> explicit open -> retained input object
removal -> child disappears; existing objects retain disconnected state
```

## Requirements and impact

| Area | Requirement and effect |
| --- | --- |
| Scope | GOALS, Hardware drivers and Milestone D: make existing native input accessible through live discovery. |
| Boot and platform | ARCHITECTURE, Input services and attachment changes: use existing boot and hotplug registration on both firmware paths. |
| Memory and isolation | Input ABI, Authority and discovery: preserve input rights and bounded identities. Observation cannot allocate input objects. |
| Concurrency | Input ABI, Broadcast, loss, and lifetime: serialize discovery with attachment changes. Cancel stale page cursors. |
| Interfaces | Replace shell numeric acquisition with device-local `open`. Preserve the version-1 IPC selector for native clients. |
| Runtime resources | Measure images, context latency, and boot resources against `9ae4285`. Keep discovery bounded to 64 attachment lifetimes. |
| Validation and documents | Test repeated discovery, pagination, removal, replacement, authority, fields, and callable methods. Run the full local suite and independent review. |

Storage, DMA ownership, scheduler policy, and input report decoding do not change.
The discovery transport uses the existing passive observation boundary.

## Implementation sequence

1. Reproduce enumeration of disconnected records in the existing native list.
2. Add passive, generation-checked input descriptions with connected children only.
3. Expose stable per-kind names and device-local acquisition through context and Tab.
4. Test repeated enumeration and attachment changes without identity reuse.
5. Measure resource effects and run all local checks.
6. Obtain an independent review of the complete change.

Names use `keyboardN` and `mouseN`, with separate counters for each kind.
Removed names are not reused during the boot.
The touchpad uses the relative-pointer interface and receives a `mouseN` name.
Each child exposes its descriptive name and hardware catalog identity.
Existing disconnected handles remain available for state inspection and event draining.

## Hardware report: 2026-11-10

The user verified keyboard Left Shift state and touchpad movement and buttons.
The user reported increasing enumeration results and `input closed` errors during acquisition or subscription.
USB mouse acceptance remains pending because enumeration obstructed the test.
The report does not identify which device produced each closed result.

The existing native list includes retired attachments; its enumeration does not register devices.
This explains stale entries but does not prove the cause of every reported identity increase.
Repeated live discovery and hotplug tests will check for growth without attachment changes.

## Evidence

Baseline revision: `9ae4285fc2c72d7a56f3a4cd71be57a6418c11cf`.
Baseline artifacts: `build/input-discovery/baseline/`.
User-provided SMBIOS captures remain outside this change; SMBIOS remains deferred.

### Regression and focused checks

`InputService.ListExcludesRetiredAttachments` failed before the fix.
It returned removed identity 1 instead of connected replacement identity 2.
The failure remains in `build/input-discovery/reproduction.log`.
After correction, all 21 focused input and discovery tests passed in 0.70 seconds.
An intermediate build failure identified an incorrect string-helper name; the correction uses the existing freestanding helper.

The BIOS and UEFI native-input checks passed in 7.69 and 7.04 seconds, respectively.
These checks use real QMP mouse attachment, movement, removal, and replacement.
They repeat passive discovery 12 times, check unchanged results, and invoke the device-local method through Tab completion.
Evidence: `build/input-discovery/input-smokes.log` and isolated guest transcripts under `build/artifacts/test-runs/`.

```sh
ctest --test-dir build-host-tests -R 'InputService|Discovery' --output-on-failure
ctest --test-dir build --parallel 2 -R 'os1_smoke_native_input_(bios|uefi)$' --output-on-failure
./scripts/check-local.sh
```

### Review

The first independent review found one obsolete README example that used removed shell commands.
The correction replaces numeric acquisition with connected children and device-local `open`.
The reviewer found no concrete code blocker in the complete task diff.
The README correction passed a second independent review.
No executable source changed after the complete local suite started.
Final independent review covered the complete diff, new documents, validation logs, measurements, and resolved finding.
It found no remaining blocker.
Physical USB mouse behavior remains outside that review's evidence.

### Full local validation

`./scripts/check-local.sh` passed on the final executable tree based on `9ae4285`.
The working changes comprise passive input discovery, shell acquisition, and regression tests.

| Profile | Result | Duration |
| --- | --- | --- |
| Host tests | 648/648 passed | 21.19 seconds |
| Unfiltered normal images | 90/90 passed | 422.53 seconds |
| Diagnostic images | 3/3 passed | 5.86 seconds |

Full output: `build/input-discovery/full-local.log`.
The first failing regression and intermediate build output remain preserved.
No full-suite failure was retried or omitted.
Scoped formatting, Python syntax, whitespace, local document links, and current command examples passed checks.
The diff scan found no credentials.
The complete ASD-STE100 dictionary was unavailable; prose received manual review.

### Resource comparison

The [measurement record](2026-11-10-input-discovery-measurements.json) retains all 12 runs and artifact hashes.
The baseline source/build hash matches `9ae4285`; its saved pre-commit binary still embeds revision `80d754d`.
The candidate uses the final executable working tree and its recorded source/build hash.
Measurements ran alone after the full suite, with three alternating repeats per firmware and revision.

The host used macOS 26.6.2, Apple M4 Max, 16 logical CPUs, and 64 GiB RAM.
The toolchain used GCC 11.2.0, CMake 4.4.5, and QEMU 11.1.3.
Each guest used Q35, TCG, four CPUs, 128 MiB RAM, and virtio program storage.
Build settings were Debug, kernel allocator diagnostics enabled, debugcon enabled, and normal boot diagnostics disabled.
BIOS used bundled SeaBIOS; UEFI used the Homebrew EDK2 code and variables recorded in the JSON.

Each workload describes the first keyboard 20 times, with memory and system observations before and after.
The baseline calls `os.input.list`; the candidate calls `context os.input.keyboard1`.
These user operations provide comparable device information but have different output and execution paths.
The candidate includes method metadata and path resolution; these measurements do not isolate kernel query latency.

| Firmware | Metric, seconds | Baseline median | Candidate median | Absolute change | Relative change |
| --- | --- | --- | --- | --- | --- |
| BIOS | boot_seconds | 0.4651 | 0.4620 | -0.0031 | -0.67% |
| BIOS | workload_seconds | 1.7137 | 2.6404 | +0.9267 | +54.08% |
| BIOS | qemu_cpu_seconds | 2.4450 | 3.3823 | +0.9372 | +38.33% |
| UEFI | boot_seconds | 2.0134 | 2.0293 | +0.0159 | +0.79% |
| UEFI | workload_seconds | 0.7621 | 1.2342 | +0.4721 | +61.94% |
| UEFI | qemu_cpu_seconds | 2.9032 | 3.4431 | +0.5399 | +18.60% |

The new descriptive workflow takes longer in this workload; no latency improvement is claimed.
Measured free-page counts match the baseline before and after each workload.
The input registry adds one eight-byte generation counter; its existing allocation still uses the same page count.
Discovery adds no device or subscription allocation.

| Artifact | Baseline bytes | Candidate bytes | Absolute change | Relative change |
| --- | --- | --- | --- | --- |
| Kernel ELF | 612056 | 613848 | +1792 | +0.293% |
| Initrd | 7680 | 7680 | 0 | 0% |
| BIOS image | 1474560 | 1474560 | 0 | 0% |
| UEFI image | 4345856 | 4347904 | +2048 | +0.047% |
| Program disk capacity | 34603008 | 34603008 | 0 | 0% |

The functional and bounded-resource requirements passed automated checks.
Physical USB mouse behavior and spontaneous hardware reconnects remain unmeasured after this change.
Retest repeated `context os.input`, device-local acquisition, removal, and reconnect on each physical device.
Update both the kernel and disk programs before that retest.

The measurement driver is `build/input-discovery/measure_input.py`.
It copies `tests/scripts/measure_hardware_catalog.py` and changes the repeated command and expected marker.
The JSON records those substitutions and the driver hash.
Raw logs remain under `build/input-discovery/measurements/`.
