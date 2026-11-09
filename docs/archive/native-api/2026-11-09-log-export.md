# Log export and persistence

This plan implements bounded log snapshots and persistent export from the existing memory records.
Memory capture remains available when export fails.

```text
kernel ring + registered shell histories
  -> bounded snapshot copies -> shared 64 KiB staging
  -> /tmp/log snapshots, on request
  -> /disk/log framed segments, periodic or explicit
kernel worker -> export status and failure counters
shutdown/reboot -> final drain -> filesystem synchronization
```

## Accepted scope

Use the budgets and intervals in [accepted defaults](../../native-api/context-diagnostics.md#accepted-defaults--2026-11-08).
Kernel and shell snapshots share the 64 KiB staging allocation.
The existing kernel provider worker performs filesystem operations outside capture locks.
Timed worker wakeups permit export while the shell waits for input.
No interrupt capture path performs storage I/O.

Add Native Log `export` and `export_info` members.
Export requires both LOG_READ and SYSTEM_CONTROL authority; status requires LOG_READ.
`os.system.log.export target:"tmp"` refreshes bounded snapshots.
`os.system.log.export target:"disk"` retries and synchronizes persistent export.
Both commands export the available kernel and shell records under the system logging policy.
No unrestricted caller-supplied destination path is accepted.

Use versioned binary frames with explicit sizes, record counts, sequence bounds, CRC32, build identity, and clock anchors.
Readers recover preceding complete frames when the final frame is incomplete or corrupt.
Record command IDs and original record flags without capturing new payload data.
A host reader prints records and reports incomplete tails.

Persistent names contain a monotonic disk-local boot number and segment number.
Select a number above every recognized segment filename before writing a new boot record.
Synchronize the first segment before treating the boot number as reserved.
Do not overwrite existing files or delete files without a validated managed frame header.
Retain at most four boots and sixteen segments within the accepted byte limits.
Reserve free space before extending files.
The existing ext2 profile has no journal; export cannot guarantee power-loss durability.

Register histories under the log lock and copy records under that lock.
Process cleanup transfers a closing history to the exporter without doing I/O.
A bounded drain releases closed histories, including when export is unavailable; expose lost closing histories in status.
Completed records advance the export cursor; final drains can include a pending command without advancing past it.
This permits a later completion record if a failed shutdown returns to the shell.

After three consecutive failures, suspend automatic export until explicit retry or a changed mount.
Storage failures update bounded status counters without recursively generating exports.
Normal errors request an early drain and synchronization, coalesced to once per second.
Shutdown and reboot attempt a final bounded drain before filesystem synchronization.

## Impact assessment

| Area | Requirements and assessment |
| --- | --- |
| Scope | GOALS: Runnable vertical slices and Educational value; implement diagnostics slice 3 without general service discovery |
| Boot/platform | Architecture: Memory logging and correlation; initialize after namespace mount; test BIOS and UEFI; preserve BootInfo; enlarge bounded BIOS reservations |
| Memory/isolation | One staging allocation; retain bounded histories; validate frame sizes and authorities; allocation failure preserves boot |
| Concurrency | Short capture locks; worker I/O outside locks; serialize export admission; synchronize history reads and teardown; preserve SMP wakeups |
| Interfaces | Extend Native Log v1 with new member IDs and fixed records; preserve existing query layouts and startup ABI |
| Runtime resources | Measure CPU, memory, images, command latency, export bytes, and capture loss against main `a9c9b45` |
| Validation/documents | Host framing, retention, failure, and authority tests; BIOS/UEFI no-disk and reboot persistence; independent adversarial review |

The Native API index distinguishes implemented memory records from the accepted persistence design.
Relevant contracts are the memory log ABI, kernel provider adapter, filesystem metadata, and system lifecycle.
Console editing and device protocols do not change. Syscall entry admission protects foreground filesystem access during export.
Kernel growth moves BIOS initrd staging from `0x76000` to `0x78000` and its checked end to `0x9c000`.
The kernel reserve and bitmap base move from `0x198000` to `0x1a0000`.
The loader still checks the conventional-memory limit before loading.
These layout changes require BIOS contract checks and separate physical qualification.
Periodic worker deadlines add bounded wakeups; they do not change user scheduling quanta.

## Implementation and acceptance

1. Add the binary frame contract and host reader.
2. Add synchronized history snapshots and deferred cleanup.
3. Add bounded storage, retention, worker scheduling, and lifecycle hooks.
4. Expose authorized methods through discovery and guided invocation.
5. Test malformed data, missing/full storage, partial writes, rotation, suppression, shell exit, and reboot recovery.
6. Measure comparable runtime costs and update current ABI and architecture documents.
7. Obtain independent review, resolve findings, and open a new PR.

Physical USB unplug and sudden-power-loss behavior require separate hardware qualification.
Host tests and QEMU do not establish those results.

## Foreground I/O correction

The full smoke matrix exposed contention between background export and direct descriptor operations.
One shell write failed; a separate executable metadata read returned an empty result.
The ext2 backend uses nonblocking admission and cannot wait inside those operations.

```text
foreground syscall / deferred descriptor operation -> shared admission -> existing operation
export worker -> exclusive admission -> bounded export -> release
conflicting syscall -> scheduler yield -> retry INT 0x80 before side effects
```

Add shared admission at syscall entry and deferred descriptor retries.
The exporter requires exclusive admission before filesystem access.
Automatic contention defers export without incrementing failure counters.
Explicit export can report `WOULD_BLOCK` without changing files.
This broad rule delays unrelated syscalls during export; a sleeping filesystem mutex remains a future refinement.
No spinlock remains held during export or device waits.

The correction also admits dirty synchronization independently from the periodic drain deadline.
A host test verifies synchronization at five seconds when the next normal drain is later.
The expanded guest test repeats forty foreground writes and reads while automatic export is enabled.

## Validation and resource evidence — 2026-11-09

The implementation passes host, guest, and independent review checks.
Physical USB log export remains unqualified.

| Check | Result | Evidence under `build/log-export-evidence/` |
| --- | --- | --- |
| Host suite | 583/583 passed | `full-host.txt` |
| Full firmware, SMP, filesystem, input, and storage matrix | 73/73 passed | `validated-guest.txt` |
| Final aggregate-threshold correction | Six affected BIOS/UEFI checks passed | `final-affected.txt` |
| Diagnostic USB images | Three checks passed, including BIOS and UEFI | `diagnostic-tests.txt` |
| Foreground contention regressions | Spawn, ext2, export, and interrupt handoff passed | `contention-regression.txt` |
| Framing reader | Valid prefix, malformed headers, checksums, and incomplete tails passed | Included in the 73-check matrix |
| Documentation | Local links and command syntax checked; no broken links | Repository link check |

The complete matrix ran after the syscall-admission correction.
The final threshold correction combines kernel and shell pending bytes; six affected guest checks and all host tests passed afterward.
The diagnostic profile already drains every 250 milliseconds, so this threshold correction does not change its cadence.
Initial sandbox failures concerned local monitor sockets; the full matrix passed with socket access.
The two foreground I/O failures were introduced regressions, not baseline failures. The admission correction resolves them.

Commands used:

```sh
cmake --build build
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
ctest --test-dir build --output-on-failure
ctest --test-dir build -V -R '^os1_smoke_(log_export|memory_logging|system_lifecycle)_(bios|uefi)$'
ctest --test-dir build/log-export-diagnostic --output-on-failure --no-tests=error -R '^os1_smoke_usb_keyboard_'
```

The diagnostic build uses `OS1_USB_KEYBOARD_DIAGNOSTICS=ON`, `OS1_KMEM_DEBUG=ON`, and `OS1_QEMU_DEBUGCON=OFF`.
Its build target is `os1_usb_image`.
The normal build uses `Debug`, `OS1_KMEM_DEBUG=ON`, and `OS1_QEMU_DEBUGCON=ON`.
Both builds retain the BIOS size and firmware-memory checks.

### Comparable measurements

[Raw measurements](2026-11-09-log-export-measurements.json) include individual samples, image hashes, firmware hashes, and the candidate source fingerprint.
The baseline is main `a9c9b452e48fc1be02cbbf193058027a6acbff79`.
The candidate is the reviewed working tree on `codex/log-persistence`, identified by that fingerprint.
The build records its pre-commit revision separately from the source fingerprint.

The host uses Apple M4 Max, 16 CPU cores, 64 GiB RAM, and macOS 26.6.2.
The compiler is x86_64-elf GCC 11.2.0; QEMU is 11.1.3.
The guest uses TCG, q35, four CPUs, and 128 MiB RAM.
UEFI uses the recorded QEMU EDK2 code and variable templates.
BIOS uses the raw image and QEMU firmware.

The serial workload warms up ten commands, then measures three batches of twenty echo commands and three batches of twenty completion selections.
The table uses medians for these batches. QEMU CPU time covers one complete process per configuration, including boot.
No other QEMU measurement workload ran concurrently.
These short TCG samples do not establish a performance improvement or physical-device latency.

| Metric | Baseline | Candidate | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| BIOS 20 echo commands | 156.589 ms | 160.459 ms | +3.871 ms | +2.47% |
| BIOS 20 completion selections | 65.145 ms | 75.690 ms | +10.545 ms | +16.19% |
| BIOS QEMU CPU | 1.521 s | 1.640 s | +0.118 s | +7.76% |
| BIOS free 4 KiB pages | 31662.000 pages | 31635.000 pages | -27.000 pages | -0.09% |
| UEFI 20 echo commands | 133.372 ms | 115.546 ms | -17.826 ms | -13.37% |
| UEFI 20 completion selections | 53.718 ms | 81.711 ms | +27.993 ms | +52.11% |
| UEFI QEMU CPU | 2.561 s | 2.669 s | +0.108 s | +4.21% |
| UEFI free 4 KiB pages | 19248.000 pages | 19216.000 pages | -32.000 pages | -0.17% |
| `kernel.elf` | 555424 bytes | 576400 bytes | +20976 bytes | +3.78% |
| `initrd.cpio` | 143360 bytes | 144896 bytes | +1536 bytes | +1.07% |
| `os1.raw` | 1474560 bytes | 1474560 bytes | +0 bytes | +0.00% |
| `os1.iso` | 4425728 bytes | 4448256 bytes | +22528 bytes | +0.51% |

The observed free-page decrease is 108 KiB for BIOS and 128 KiB for UEFI.
Both paths use one additional slab page.
The exporter adds 64 KiB staging plus bounded segment, status, and stream bookkeeping.
The final BIOS kernel remains within its 424 KiB slot; the 144 KiB initrd slot also fits.

The explicit temporary-export smoke measures three calls per firmware path.
These measurements include command parsing, serial completion redraw, IPC, and snapshot creation.
See the raw measurements for the six call times.
There is no baseline export operation, so its relative latency change is undefined.

### Storage workload

A host test uses the actual ext2 implementation over a memory-backed block device.
It exports twenty-four batches, each containing 1024 kernel events and 32 completed commands.
Three repetitions produce identical byte counts.
Counters exclude mount I/O and subsequent validation reads.

| Metric | Baseline exporter | Candidate | Absolute change | Relative change |
| --- | ---: | ---: | ---: | --- |
| Framed bytes written | 0 | 7122688 | +7122688 | Undefined |
| Block bytes read | 0 | 469979136 | +469979136 | Undefined |
| Block bytes written | 0 | 289587200 | +289587200 | Undefined |
| Flush requests | 0 | 75 | +75 | Undefined |
| Retained log bytes | 0 | 3429120 | +3429120 | Undefined |

The backend writes about 40.7 block bytes per framed byte in this workload.
Small writes update data and metadata blocks; retention scans also add reads.
These counts establish I/O amplification, not USB throughput or flash wear.
Block-sized writes and finer filesystem admission are recorded roadmap follow-ups.

### Requirement status and limits

Host tests verify storage budgets, rotation, free-space reserve, suppression, short appends, read errors, flush errors, and closing-history cleanup.
Guest checks verify decoded records, foreground file correctness, shutdown drain, missing mounts, and file preservation across separate boots.
The reader preserves complete frames before a malformed tail.
Memory queries remain available after automatic export suspends.

Capture-loop and interrupt latency under saturated physical storage remain unmeasured.
The capture algorithm is unchanged; history snapshots add one bounded record copy under the capture lock.
Next, measure interrupt latency, capture loss, command responsiveness, and storage throughput on both physical test machines.
Also test USB removal and sudden power loss during export.
Do not treat the host block-device fixture or QEMU as physical durability evidence.

The complete ASD-STE100 standard and dictionary were unavailable; the authority site returned HTTP 403.
Prose received active-voice, sentence-length, terminology, and link review.
This does not establish complete dictionary compliance.

## Independent review

| Review | Findings and disposition |
| --- | --- |
| First adversarial pass | Fixed history-membership division race, filename replacement during deletion, indirect-block reserve, and hidden header-read errors |
| Second pass | Confirmed prior fixes; corrected the independent synchronization deadline |
| Third pass | Reviewed syscall admission, saved-frame retry, deferred I/O, history lifetime, authority, retention, and framing; no blockers |
| Threshold addendum | Reviewed aggregate pending-byte admission and its regression; no blockers |

Each confirmed defect has regression coverage.
The reviewers changed no project files and ran no tests.
The implementer ran the recorded checks.
Final documentation and evidence received a separate read-only review before publication.
