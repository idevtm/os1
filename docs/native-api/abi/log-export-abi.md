# Log export ABI

This profile exports existing memory records to bounded files under `/tmp/log` and `/disk/log`.
Memory capture and queries remain available when export fails.

```text
kernel ring + retained shell histories
  -> copy one record under the capture lock
  -> shared 64 KiB staging buffer
  -> explicit /tmp snapshots OR periodic /disk segments
  -> CRC32 validation -> host JSON Lines reader
```

## Calls and authority

Native Log retains interface version 1 and its existing read layouts.
The [memory ABI](memory-log-abi.md) defines the captured records and suppression policy.
The true namespace root supplies the binding authority.

| Member | Request | Reply | Required rights |
| --- | --- | --- | --- |
| `export`, ID 3 | `os1_log_export_request`, 16 bytes | Application status | `LOG_READ` and `SYSTEM_CONTROL` |
| `export_info`, ID 4 | Empty | `os1_log_export_info`, 96 bytes | `LOG_READ` |

The export request contains four `u32` fields: version, size, target, and reserved.
Version is 1, size is 16, and reserved is zero.
Target 1 selects `/tmp`; target 2 selects `/disk`.
No caller-supplied path is accepted.
Export includes the available kernel records and all registered shell histories.
Own-process history queries retain their existing access restriction.

```text
os.system.log.export target:"tmp"
os.system.log.export target:"disk"
os.system.log.export_info
```

Tab completion exposes the `tmp` and `disk` enum choices.
An explicit disk export retries suspended export, drains bounded current records, and requests synchronization.
Explicit exports and lifecycle drains bypass the background byte rate limit.
They use at most 100 stream visits and a 2 MiB framed-data budget.
A pending command can appear again after completion; readers identify it by boot, session, and command ID.

The status record starts with version, size, enabled, and suspended as `u32` fields.
An `i64` last_status follows.
Eight `u64` fields follow: boot_id, disk_bytes, tmp_bytes, failures, closing_lost, kernel_cursor, last_drain_ns, and last_sync_ns.
Two `u32` fields finish the record: consecutive_failures and reserved.
`disk_bytes` counts successfully written frame bytes during this runtime, including identity frames.
`tmp_bytes` reports successfully written snapshot bytes from the latest refresh.

`closing_lost` counts retired histories whose pending records could not all reach disk.
`kernel_cursor` records the last exported kernel sequence.
Timestamps use the memory log clock.
The fixed status record has no unbounded error text.

## Storage and scheduling

| Destination | Policy | Limits |
| --- | --- | --- |
| `/tmp/log` | Explicit replacement of managed snapshots | 32 KiB kernel plus 32 KiB aggregate shell data |
| `/disk/log` | Append complete frames and rotate managed segments | 3 MiB kernel plus 1 MiB shell data |
| Persistent retention | Oldest boot and segment first | Four boots; sixteen segments; 256 KiB per segment |
| Free-space reserve | Checked before directory creation and file extension | Greater of 2 MiB or 10% of filesystem capacity |
| Background drain | Normal / diagnostic | 2 seconds / 250 milliseconds, or 32 KiB pending |
| Background synchronization | Normal / diagnostic, when dirty | 5 seconds / 1 second |
| Background framed-data rate | Normal / diagnostic | 64 KiB/s / 256 KiB/s |

A dedicated I/O worker checks export deadlines at most every 250 milliseconds when idle.
It also dispatches File, Directory, system-control, and explicit-export requests.
A separate service worker handles input, console, and memory-log queries.
Export requires exclusive admission against filesystem operations, file providers, and deferred descriptor I/O.
Read and write check admission after they retain a file descriptor.
Console I/O, process exit, and Buffer operations can proceed during export.
A conflicting syscall yields before side effects. It rewinds the saved instruction pointer by two bytes to retry.
The VFS worker retries deferred descriptor operations when admission is available.
An active foreground entry causes automatic export to defer without counting a storage failure.
Native explicit export retains its Operation when foreground entries prevent admission.
A deferred request has no export effects and does not emit an IPC completion record.
The worker retries admission after one millisecond of scheduler time.
It sleeps until eligible work or a deadline, and it can dispatch other ready requests.
The existing Operation and binding limits bound deferred requests.
Closing caller descriptors does not cancel an accepted export.
After admission, the worker executes export once and completes the Operation with its result.
A storage error, including `WOULD_BLOCK`, is final for that Operation and is not replayed.
Direct internal export calls retain their nonblocking admission result.

This admission rule prevents background export from causing foreground ext2 guard failures.
It delays filesystem access during export.
Input IPC, IPC clocks, basic descriptor operations, observation, and memory-log calls bypass this admission gate.
IPC pair creation is memory-only and bypasses admission.
Launch, spawn, and exec retain admission because they can load executable files.
Descriptor release frees retained references without entering a filesystem backend.
The I/O worker serializes lifecycle control with export.
Interrupt capture and storage completions continue while export owns admission.
A short lock protects the published status snapshot; readers see the latest completed export state.
No status lock spans storage I/O.
Stream admission rotates to prevent sustained traffic from starving another stream.
Errors request an early drain and synchronization, coalesced to one request per second.
Backend metadata writes are additional I/O; the byte ceiling applies to framed log data.
Existing filesystem synchronization can flush logs before their periodic deadline.

Capture never performs filesystem I/O.
History snapshots copy one record under the capture lock.
Process cleanup transfers a closing history to the exporter without I/O.
Export completion or failure releases that history within the existing four-history limit.
Snapshot membership is fixed before allocating each history's share of the temporary budget.

Three consecutive disk-export failures suspend automatic export.
Explicit retry or a changed mount clears suspension.
A retired ext2 backend still requires a healthy mount; retry does not repair it.
Temporary export failures do not reset disk suspension.
Shutdown and reboot use a 250 ms scheduler-time deadline for exclusive export admission.
If admission is busy at the limit, the power request returns WOULD_BLOCK.
The system holds admission through the final drain and filesystem synchronization.
A disk-export error does not prevent the final filesystem synchronization attempt.

Disk export splits each frame at ext2 block boundaries.
Temporary export uses writes of up to 256 bytes.
A failed append abandons that segment for subsequent writes.
The next segment preserves a readable prefix in the previous file.
Ext2 has no journal; successful synchronization does not guarantee recovery from every sudden power loss.

## Names and identity

Temporary names are `kernel.bin` and `shell-<session>.bin`.
Persistent names are `kernel-<boot>-<segment>.bin` and `shell-<boot>-<session>-<segment>.bin`.
Each numeric filename field uses sixteen lowercase hexadecimal digits.
The first frame in each file has zero records and identifies the managed file.
Retention validates this identity again under namespace admission before deletion.
Foreign files are preserved; occupied or invalid snapshot names can cause export failure.

The persistent boot number exceeds all recognized existing boot numbers, including incomplete matching files.
The first identity frame is synchronized before the number is used for data export.
A temporary snapshot made before persistent initialization uses the memory log's local boot identity.
That temporary identity is not globally unique.
Directory scans are bounded to 256 entries and 128 recognized managed files.
These bounds can stop export in a directory containing excessive unrelated files.

Each frame carries the source revision and a source/build-profile SHA-256 fingerprint.
The revision field holds up to 40 bytes.
A dirty checkout uses the first 34 characters of its revision followed by `-dirty`.
An archive uses the supplied `OS1_SOURCE_REVISION` label or `unknown`.
The label does not change the fingerprint.
The fingerprint covers project-owned source, CMake scripts, and the configured compiler profile.
It is not a byte hash of the complete boot image or third-party dependencies.
Clock fields provide coarse monotonic time, wall time when available, and resolution.
Wall and monotonic samples are approximate anchors; they are not an atomic clock snapshot.

## Binary frames and recovery

All fields use little-endian encoding.
The [shared frame definition](../../../src/common/log/frame.hpp) specifies the 256-byte header.
CRC32 uses polynomial `0xedb88320`, with the checksum field treated as zero.
A complete frame contains the header followed by zero or more complete records.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 8 | Magic `0x31474f4c31534f` |
| 8 | 24 | Six `u32`: version, frame bytes, record bytes, count, stream, checksum |
| 32 | 80 | Ten `u64`: boot, session, segment, first, last, monotonic, wall, resolution, overwritten, dropped |
| 112 | 8 | Two `u32`: diagnostic flag, boot mode |
| 120 | 40 | Source revision |
| 160 | 64 | Source/build-profile fingerprint |
| 224 | 32 | Reserved zero bytes |
| 256 | Variable | Kernel records of 256 bytes, or history records of 2048 bytes |

Version is 1; stream 1 is kernel and stream 2 is shell.
The complete frame cannot exceed 65536 bytes.
Sequence bounds describe the records in that frame.
Loss counters retain the memory source's cumulative values; sequence gaps also expose missing records.
An empty identity frame has zero sequence bounds.

After copying exported files to the host, run:

```sh
python3 tools/log_dump.py kernel-*.bin shell-*.bin
```

The reader emits one JSON object per complete frame.
It escapes text so stored control bytes cannot control the host terminal.
A partial or corrupt frame ends recovery for that file and returns a nonzero status.
Previously emitted complete frames remain usable.
See the [implementation record](../../archive/native-api/2026-11-09-log-export.md) for impact, tests, measurements, and review results.
