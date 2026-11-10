# Persistent device test results

`run-tests` collects repeatable checks and hardware evidence on the running machine.
Each run creates separate files on ext2 without replacing previous results.

```text
kernel build + machine fingerprint + boot ID + exclusive run directory
    -> manifest and progress journal -> hardware / input / storage checks
    -> saved logs and summary -> sync -> offline inspection
```

## Requirements and impact

| Area | Requirement and effect |
| --- | --- |
| Scope | GOALS, Hardware drivers and Milestone D: support repeatable physical acceptance and returned-stick analysis. |
| Boot and platform | ARCHITECTURE, Input services and attachment changes: preserve both boot paths and existing device activation. |
| Memory and isolation | Input ABI, Authority and discovery: require existing grants; bound snapshots, result files, and run admission. |
| Concurrency | Check catalog generations during pagination. Treat input enumeration as a bounded observation, not an atomic topology snapshot. |
| Interfaces | Add a disk utility through the existing launch and native APIs. Preserve public record layouts. |
| Runtime resources | Measure image size, memory, duration, and output bytes. Keep raw key contents out of captured results. |
| Validation and documents | Test tracing, exclusive output creation, blocked result storage, repeated runs, and reboot persistence. Run all local checks and independent review. |

Storage layout, firmware policy, input decoding, and driver activation remain unchanged.
Add bounded input attachment/removal records and EHCI transfer-failure details to the existing memory log.
Do not infer a mouse-driver correction from incomplete hardware evidence.

## Traceability

Use a machine fingerprint derived from CPU identity and stable PCI identity fields.
Exclude removable USB devices from this fingerprint.
Allow an explicit machine label for identical hardware configurations.
Record the fingerprint inputs; this fingerprint is not a guaranteed hardware serial number.

Record the full kernel revision and build-content hash from validated log export metadata.
Include boot ID, wall time when available, monotonic start/end times, and runner version.
Create each run directory exclusively; never truncate or reuse an existing result file.
Preserve incomplete runs and report their last completed stage.

## First implementation

1. Capture platform, PCI, USB, storage, input, and kernel log evidence.
2. Check input discovery, acquisition, state, independent subscriptions, and unsubscribe.
3. Check temporary and persistent file writes with deterministic readback.
4. Save explicit PASS, FAIL, and SKIP results with stage progress and final sync.
5. Provide an optional input observation interval for physical movement and button tests.
6. Preserve previous results; require explicit cleanup when the bounded run namespace or disk is full.

Idle input cannot prove physical keyboard or pointer function.
Default checks must not report that physical stimulus passed when no stimulus occurred.
The first runner does not execute destructive fault programs or claim complete driver qualification.

## Commands and saved files

The utility resides at `/disk/bin/run-tests`.
Deploy the updated programs through the preserving USB update menu before testing.

```text
run-tests --machine laptop --input --expect-usb-mouse
run-tests --machine desktop --input --expect-usb-mouse
```

During the 20-second observation, move each pointer and press and release its buttons.
Press and release Left Shift on each keyboard.
Keep the devices connected during this test.
The default run observes input for one second without requiring user action.
Use `--expect-usb-mouse` only when a USB mouse must be available.

```text
/disk/tests/
  m-<optional-label-><machine-fingerprint>/
    k-<first-16-build-hash-characters>/
      b<boot-id>-r<run-number>/
        manifest.txt       identity and test options
        progress.txt       stages, outcomes, times, and input counters
        summary.txt        final counts and completion marker
        storage-probe.bin  deterministic 4096-byte disk readback sample
        captures.tar       hardware reports and binary logs
```

The machine label accepts 1–24 ASCII letters, digits, underscores, or hyphens.
The fingerprint uses FNV-1a over CPU identity and ordered PCI location, vendor, and device identifiers.
It excludes PCI resource addresses and USB attachments.
The hardware reports retain the source identity fields.
Identical hardware can share a fingerprint; use distinct labels when necessary.
This identifier is not a security identity.

The manifest records the full 40-character revision and 64-character kernel build hash.
It also records a hash of `/disk/bin/run-tests`, which identifies the deployed file.
That file hash does not attest to an executable launched from another path.
The directory uses a shortened build hash; the manifest retains the complete value.
Exclusive creation selects a new run number even when a boot ID repeats.
The namespace admits 256 run names per boot and build directory, subject to available storage.

The tar archive contains these members:

| Members | Evidence |
| --- | --- |
| `catalog.bin` | Versioned, generation-consistent hardware records for offline comparison |
| `system.bin`, `pci.bin`, `usb-boot.bin` | Existing observation headers and records |
| `lshw.txt`, `lspci.txt`, `lsblk.txt` | Platform, PCI, and storage presentation |
| `lsusb-before.txt`, `lsusb-after.txt` | USB catalog observations before and after input testing |
| `input.txt` | Connected input identities, names, kinds, catalog associations, and state |
| `kernel-before.bin`, `kernel.bin` | Framed kernel snapshots before and after the checks |

Runner and test-suite version 2 add `catalog.bin` and automatic diagnostic startup.
The [acceptance workflow](input-collections-test-workflow-plan.md) defines compact collections, profile comparison, and startup behavior.
Earlier measurements below describe version 1.

Extract `captures.tar` with a standard tar reader after copying the data partition.
Decode the kernel members with `tools/log_dump.py`.
A partial archive can retain complete earlier members.
Treat a missing summary or a persistence error as an incomplete run.
Preserve its journal and files for investigation.

The runner creates five files per run and one run directory.
It does not remove older results when ext2 runs out of file entries or data blocks.
Snapshots use temporary RAM files before archive serialization.
Each archive member is bounded to 256 KiB; existing API and temporary-filesystem limits can be smaller.
The runner synchronizes completed stages and the final result.
This reduces pending writes but cannot guarantee recovery after interrupted ext2 updates.

## Result interpretation and limits

`PASS` describes only the named check.
Idle devices receive `SKIP` for physical stimulus, even when acquisition and subscriptions pass.
One observed event does not prove all movement axes, buttons, or keys work.
Use the separate movement, button-change, and Left Shift counters for that assessment.
No raw key bitmap or typed text is stored by the input test.
Existing kernel and shell logging policies remain separate.

Input enumeration records connected devices at query time.
It cannot provide an atomic view across an attachment change.
Removal, closed subscriptions, lost events, and missing expected USB pointers produce failures.
The optional test does not automatically test hotplug replacement.
The storage check proves write, synchronization, and readback through the current filesystem.
It does not independently prove persistence after power loss or reboot.
Host QEMU checks separately verify preservation across clean shutdown and reboot.

The runner uses existing synchronous native calls and cannot recover from a stalled kernel provider.
Its progress journal identifies the last completed stage in that case.
The command does not execute every verification program, fault injection, shutdown, or destructive storage tests.
A missing writable `/disk` prevents persistent result creation and returns an error.

## Input failure evidence

The memory log records input attachment and removal once per lifetime transition.
These `OS1_LOG_EVENT` records use the input interface ID.
`arg0` contains the input identity; `arg1` contains the hardware catalog identity.
`status` contains the input kind.
EHCI transfer failures also record the input identity, root port, hub port, address, endpoint, and qTD token.
These records do not change transfer handling or enable raw keyboard logging.

## Laptop log inspection: 2026-11-10

The attached DataTraveler contains a 128 MiB ext2 partition at `/dev/disk13s3`.
The user copied this partition to `build/hardware-input-2026-11-10/data.ext2` for read-only inspection.
The decoder accepted 41 frames and 809 records, with no reported overwrite or drop.
The capture identifies revision `89030709910c056b3004125dec5427911455df66`, UEFI boot, and normal logging.

Both EHCI controllers started; USB storage mounted successfully.
The shell history records successful `context os.input.mouse2` calls.
The log does not contain those commands' output or a HID transfer-failure reason.
Thus, this capture cannot identify the missing mouse or prove why its input object became unavailable.
Raw capture and decoded records remain under `build/hardware-input-2026-11-10/`.

## Validation record

Baseline revision: `89030709910c056b3004125dec5427911455df66`.
Baseline artifacts: `build/hardware-tests/baseline/`.
Physical acceptance of the new runner remains pending until the user returns a new capture.

### Failures found during implementation

All first-failure evidence remains under `build/hardware-tests/` and the isolated test workspaces.

| Evidence | Cause and correction |
| --- | --- |
| `runner-first.log` | The runner expected data from the empty log-export response. It now reads the exported frame. |
| `runner-second.log` | The saved output descriptor lacked delegation rights. Restoration now preserves the original rights and descriptor flags. |
| `runner-third.log` | A test matched `failed=0` inside `io_failed=0`. It now checks complete lines. |
| `runner-fourth.log`, `failure-final.ext2` | Four runs exhausted 128 ext2 file entries. Snapshots now share a tar archive. |
| `runner-stimulus.log` | The sandbox rejected QEMU's local QMP socket. Subsequent runs use the authorized local test environment. |
| First full local run | The mouse attachment was not yet published when the stimulus check started. The test now waits for live discovery. |

The exhausted image had zero free file entries and 7703 free 4096-byte blocks.
The archive change reduces each run from 15 files to five files.
Both layouts also require one run directory.
No existing results are deleted to recover capacity.

### Independent review corrections

The first review found two substantive defects.
The initial subscription probe discarded a successful response without checking its loss flag or payload.
A shared event validator now checks both the probe and observation reads.
A host regression covers loss, removal, invalid size, and incorrect identity.

The executable also lacked an explicit dependency on `src/uapi/os1/input.h`.
The build now declares that dependency.
The reviewer withdrew an extraction-directory concern after verifying isolated test workspaces.
The correction review found no remaining source blockers.
A final evidence review verified the test totals, artifact hashes, measurement samples, and reported differences.

### Later test coverage

Add supervised execution before including arbitrary verification programs.
Each such check needs a timeout, cleanup rules, and an explicit destructive-test classification.
Add a separate hotplug exercise with expected removal and replacement identities.
The present observation treats removal as a failure instead of qualifying hotplug behavior.

### Final automated validation

`./scripts/check-local.sh` passed after the review corrections and test readiness change.

| Profile | Result | Duration |
| --- | --- | --- |
| Host tests | 653/653 passed | 21.37 seconds |
| Unfiltered normal images | 92/92 passed | 459.13 seconds |
| Diagnostic images | 3/3 passed | 5.84 seconds |

The final output is `build/hardware-tests/full-local-final.log`.
The first full run passed 652 host tests, 91 normal checks, and three diagnostic checks.
Its sole failure was the UEFI mouse-discovery test race described above.
The first profile logs remain in `build/hardware-tests/first-full/`.
The final test tree includes the event-validator regression, which increases the host count to 653.

Both runner checks verify four unique runs, an expected-device failure, blocked result storage, archive contents, and preservation after reboot.
Their final run injects QEMU mouse movement during the optional observation.
The tests validate ext2 with `e2fsck -fn` and compare earlier files byte for byte.
The Ninja dependency query confirms that `run-tests.elf` depends on `src/uapi/os1/input.h`.

Scoped formatting, Python syntax, whitespace, and local document file links passed checks.
The diff scan found no credentials; qTD tokens are hardware status words.
The correction review found no remaining source blockers.
The complete ASD-STE100 dictionary was unavailable; technical prose received manual review.

### Resource comparison

The [measurement record](2026-11-10-device-runner-measurements.json) retains samples, artifact hashes, commands, and configuration.
The baseline source hash matches revision `8903070` and its saved pre-commit artifacts.
Those artifacts still embed revision `9ae4285`; their source/build hash is `b79c5d0142dc45172089ccf77dedbd259ce50e1c4e568e88e843d69e3b9177ae`.
The final candidate uses source/build hash `d9271e6cf2f68603dbab39abb9d485202990cbefd6ed45aae9caf23c615ffd13`.

Measurements ran alone after local validation, with three alternating baseline/candidate repeats per firmware.
The host used macOS 26.6.2, Apple M4 Max, 16 logical CPUs, and 64 GiB RAM.
The toolchain used GCC 11.2.0, CMake 4.4.5, and QEMU 11.1.3.
Guests used Q35, TCG, four CPUs, 128 MiB RAM, and virtio program storage.
Build settings were Debug, allocator diagnostics enabled, debugcon enabled, and normal boot diagnostics disabled.
Firmware was bundled SeaBIOS or the recorded Homebrew EDK2 code and variables.

The common workload runs `lspci`, `lsusb -t`, `lsblk`, and `lshw`, with memory observations before and after.
These measurements include shell dispatch and output; they do not isolate driver latency.

| Firmware | Metric, seconds | Baseline median | Candidate median | Absolute change | Relative change |
| --- | --- | --- | --- | --- | --- |
| BIOS | Boot | 0.4706 | 0.4675 | -0.0031 | -0.66% |
| BIOS | Workload | 1.8164 | 1.8075 | -0.0090 | -0.49% |
| BIOS | QEMU CPU | 2.4800 | 2.4304 | -0.0495 | -2.00% |
| UEFI | Boot | 1.9962 | 1.9973 | +0.0011 | +0.06% |
| UEFI | Workload | 0.7055 | 0.7067 | +0.0012 | +0.17% |
| UEFI | QEMU CPU | 2.7750 | 2.7776 | +0.0025 | +0.09% |

No performance improvement is claimed from these three-repeat samples.
The candidate uses one additional 4096-byte page on BIOS and two additional pages on UEFI before the common workload.
The corresponding increases are 4096 and 8192 bytes.
Relative increases in used guest pages are 0.087% and 0.036%, respectively.

| Artifact | Baseline bytes | Candidate bytes | Absolute change | Relative change |
| --- | --- | --- | --- | --- |
| Kernel ELF | 613848 | 614440 | +592 | +0.096% |
| Initrd | 7680 | 7680 | 0 | 0% |
| BIOS image | 1474560 | 1474560 | 0 | 0% |
| UEFI image | 4347904 | 4349952 | +2048 | +0.047% |
| Program disk capacity | 34603008 | 34603008 | 0 | 0% |

The new runner ELF occupies 28600 bytes on disk.
Its text, data, and BSS sections total 109049 bytes, excluding stack and temporary filesystem allocations.
No baseline runner exists; relative differences for this new workload are undefined.
Peak process memory remains unmeasured because the current API has no peak-memory counter.

Three default runs per firmware used the same kernel and program images.
Median command duration was 1.7755 seconds on BIOS and 1.7509 seconds on UEFI.
Each run stored 110157–110170 bytes in five files, excluding ext2 allocation overhead.
After three runs, each guest retained 42 additional pages, or 172032 bytes, across result metadata, exports, and allocator capacity.
This is a net system-memory observation, not a measurement of peak runner memory or an allocation attribution.
Retained results and bounded filesystem capacity remain explicit resource costs.

The common measurement driver is `tests/scripts/measure_hardware_catalog.py`.
The runner measurement driver remains at `build/hardware-tests/measure_runner.py`; the JSON records its hash and command.
That driver runs `sys`, `kmem`, and `run-tests --machine measurement` three times per firmware.
Raw measurement output remains under `build/hardware-tests/measurements/` and `build/hardware-tests/runner-measurements/`.
Functional and bounded-capture checks pass; physical mouse acceptance and peak-memory measurement remain pending.
