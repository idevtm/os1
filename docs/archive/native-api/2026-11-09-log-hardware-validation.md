# USB log inspection — 2026-11-09

The retained logs pass integrity and correlation checks. They also contain a separate desktop USB enumeration failure.
The user reports successful tests on both physical machines; this snapshot proves only the retained coverage below.

```text
USB data partition -> local read-only snapshot -> ext2 check -> frame decoder -> correlation checks
```

| Boot | Hardware signature | Profile | Kernel records | Unique shell commands |
| --- | --- | --- | ---: | ---: |
| 1 | Eight CPUs, EHCI storage, Fresco xHCI; consistent with ASUS G74SX | Normal | 799 | 7 |
| 2 | Four CPUs, Intel 8086:8c31 xHCI; consistent with H81M-S1 | Normal | 2097 | 19 |
| 3 | Same desktop signature | Normal | 324 | 4 |
| 4 | Same desktop signature | Diagnostic | 2726 | 4 |

## Requirement impact

This documentation change records evidence against existing requirements; it changes no runtime behavior.

| Area | Assessment |
| --- | --- |
| Project scope | Records a complete physical test result without expanding implementation scope |
| Boot and platform | Records UEFI evidence and hardware signatures; does not establish physical BIOS coverage |
| Memory and isolation | Verifies capture counters and framing; changes no allocation or authority rules |
| Concurrency | Checks sequence continuity and command correlation; does not measure contention or prove every interleaving |
| Interfaces | Uses the implemented memory-log and export ABIs; changes no layouts or commands |
| Runtime resources | Records file sizes and counts; no comparable physical performance baseline was collected |
| Validation and documents | Distinguishes user reports from retained evidence and preserves unresolved findings |

Affected requirements are GOALS `Scope and platform goals`, architecture `Memory logging and correlation`, and the accepted context-diagnostics defaults.
The [Native API index](../../native-api/README.md) identifies the implemented [memory-log](../../native-api/abi/memory-log-abi.md) and [export](../../native-api/abi/log-export-abi.md) contracts.
Architecture, ABI layouts, and existing runtime measurements remain unchanged.
No build or runtime benchmark is required solely for this evidence update.

## Verified evidence

- The local partition copy passes `e2fsck -fn` with no errors.
- Thirteen log files contain 263 valid frames and 1661184 bytes.
- Every frame passes length, layout, sequence-bound, and CRC32 checks.
- Filename boot, session, and segment identifiers match frame headers.
- Each boot has a continuous kernel sequence from 1 to its final retained record.
- Capture-lock drops are zero for all four boots.
- The diagnostic boot contains 1055 syscall-entry records; normal boots contain none.
- All boots report UEFI and revision `7eb3cee89c8bd576683cd5e464e154397ae70968`.
- All 34 unique shell commands correlate with the expected kernel command records.
- `/log-test.txt` contains `normal boot survived`; later boots record successful reads.
- Timestamp anchors remain consistent within each boot; completed commands end after they start.

## Expected details

Boot 2 reports 1201 overwritten ring entries. Its exported sequence has no gaps, so this counter does not indicate lost exported records.
One explicit export command appears first as pending and then completed. The decoder must combine these states by command identity.
Shutdown and reboot remain pending because successful lifecycle calls do not return to the shell.
The failed command `os.system.log info` has status 1; the corrected `os.system.log.info` succeeds.

The diagnostic boot records 67 `WOULD_BLOCK` replies from console input polling.
Boot 3 records four 50-millisecond wait timeouts, consistent with bounded prompt Escape handling.
The logs do not contain the input keys needed to prove that action.
These results do not indicate storage failures.

## Separate device finding

Every retained desktop boot records these lines:

```text
xhci: device descriptor failed port=6
xhci: device DMA quarantined port=6
```

The keyboard initializes successfully on port 4. The descriptor failure concerns another controller port.
The snapshot does not identify the device on port 6.
This is an enumeration issue captured by the logger, not a frame-integrity failure.

## Limits

No laptop diagnostic boot is retained in this snapshot.
The logs record commands and operation status, not console output or return payloads.
Consequently, export-info values printed on screen cannot be reconstructed from command history alone.
Early startup records have time zero before the coarse logging clock advances.
Later timestamps have one-millisecond resolution.
The files do not prove exact physical flush intervals or sudden-power-loss durability.
Temporary `/tmp/log` snapshots disappear across reboot and cannot be checked from this persistent partition.

All inspection used the local copy. No inspection command wrote to the USB stick.
The decoder's extraction warnings concerned restoring root ownership on local files; extracted lengths and checksums pass.
A separate read-only review independently decoded the files and checked profile, sequence, timestamp, and command evidence.

The committed [evidence summary](2026-11-09-log-hardware-evidence.json) contains partition and file hashes, counts, profile flags, and sequence results.
Raw logs and the partition copy remain local under `build/hardware-logs-2026-11-09/`.
They are not committed because they can contain user commands and device details.

## Collection and validation

The source was a 15.5 GB Kingston DataTraveler 3.0 with a 128 MiB ext2 partition.
The user copied that unmounted partition to the local analysis directory.
The copy contains four UEFI boots from revision `7eb3cee89c8bd576683cd5e464e154397ae70968`.
The frame `image` field is a source/build fingerprint, not a whole-image checksum.
Physical RAM capacities and firmware versions were not recorded during this test.
The [implementation measurements](2026-11-09-log-export-measurements.json) record the local build and emulator configuration separately.

The inspection used e2fsprogs 1.47.4 and the repository frame decoder.
These read-only commands operated on the local partition copy:

```sh
e2fsck -fn build/hardware-logs-2026-11-09/data.ext2
mkdir -p build/hardware-logs-2026-11-09/extracted
debugfs -R 'rdump /log build/hardware-logs-2026-11-09/extracted' build/hardware-logs-2026-11-09/data.ext2
python3 -B tools/log_dump.py build/hardware-logs-2026-11-09/extracted/log/*.bin
```

The inspector also checked filename identities, kernel sequence continuity, command BEGIN/END pairing, and frame timestamp bounds.
The separate reviewer independently repeated frame decoding and checked those relationships.
No physical timing comparison was performed; absolute and relative performance differences remain unmeasured.
The complete ASD-STE100 dictionary was unavailable; prose received terminology, active-voice, sentence-length, and link review.

## CI and merge status

[CI run 42592359135](https://github.com/idevtm/os1/actions/runs/42592359135) failed while building revision `7eb3cee`.
Its GCC 16.2.0 cross compiler produced a 434432-byte BIOS kernel.
The checked staging slot contains 434176 bytes, leaving a 256-byte overflow.
The BIOS contract check correctly rejected the image before guest smoke tests.
Local hardware success does not resolve this compiler-dependent build failure.
PR 43 remains blocked until the BIOS build fits its validated layout and CI passes.

The desktop port 6 enumeration failure is a separate recorded follow-up.
It does not invalidate the successful log integrity and persistence checks.
