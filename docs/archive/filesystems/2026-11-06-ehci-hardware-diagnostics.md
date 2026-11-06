# EHCI hardware discovery diagnostics

The laptop starts both EHCI controllers but does not mount `/disk` from the combined boot/data image.
The controller binding list cannot distinguish root-port, hub, device, storage, and mount failures.

```text
EHCI boot discovery -> bounded immutable records -> observe(USB) -> shell usb
Block mount result ----------------------------^
```

## Plan and impact

Add a read-only `usb` command with cached EHCI boot-stage records and USB mount results.
Capture root-port status, parent addresses, enumeration stages, transfer errors, and admitted storage capacity.
Use the existing observation boundary without issuing hardware requests from the command.
Keep driver behavior unchanged until hardware evidence identifies the failure.

| Area | Impact |
| --- | --- |
| Scope | Diagnose the remaining physical storage failure against GOALS Milestone D and the USB storage guide. |
| Boot/platform | Capture the same boot probes on BIOS and UEFI; no firmware writes or reset-policy changes. |
| Memory/isolation | Bounded boot records; no pointers or physical DMA addresses reach userspace. |
| Concurrency | Record before user processes start; observations read immutable copies. |
| Interfaces | Add observe kind 12 and fixed 64-byte records; preserve existing kinds and layouts. Native IPC is unchanged. |
| Resources | Measure linked kernel sections and bounded snapshot memory against the pre-diagnostic tree. |
| Validation | ABI and record-bound tests, BIOS/UEFI QEMU output and redirection, storage regressions, independent review. |

The physical failure is not yet explained or fixed.
High-speed hub behavior remains pending hardware qualification.

## ABI and interpretation

`OS1_OBSERVE_USB=12` returns the existing observation header followed by packed 64-byte `Os1ObserveUsbRecord` records.
The first eight fields are `u32`: kind, PCI index, root port, port, address, parent address, status, and detail.
They are followed by `u64 sectors` at offset 32 and a 24-byte null-padded stage string at offset 40.
Kinds are controller=1, root-port=2, device=3, and USB mount=4.
The existing ABI version and record layouts remain unchanged.

| Record | Status | Detail |
| --- | --- | --- |
| Controller | Cached USBSTS | Address slots consumed during discovery |
| Root port | Last captured PORTSC | PORTSC before reset |
| Device before storage probing | Last transfer's final qTD token, combined with error bits | Last control request: type in bits 31–24, request in 23–16, value in 15–0 |
| Device after storage probing starts, except rejected-flush sense | Last transfer's final qTD token, combined with error bits | SCSI opcode in bits 31–24, result in 23–16, returned mode byte in 7–0 |
| Device at `storage-flush-rejected` | Last transfer's final qTD token, combined with error bits | Opcode `35`, sense key, ASC, ASCQ as four hexadecimal bytes |
| Mount | Signed mount result represented as u32 | Index in the boot USB block-device list |

Stage names identify the last attempted enumeration stage, or explicit ready states.
The first capture covers EHCI controllers that reach root-port scanning; early PCI probe failures still use the boot debug stream.
Snapshots do not include xHCI topology. USB mount outcomes include both controller types.
The shell hides disconnected root ports to keep the initial output short; binary observations retain those records.
The fixed 256-record limit exceeds the current maximum topology and USB mount-attempt counts.
Records remain immutable after boot, including after runtime device failure.
The command must therefore not be interpreted as live health monitoring.

## Build boundary correction

The added kernel code exceeded the BIOS staging slot by one 512-byte sector.
The first QEMU command check consequently ran the previous image and reported an unknown `usb` command.
The build failure, rather than shell dispatch, explained that result.
Move the initrd staging start from `0x6b000` to `0x6b200` and reduce its slot from 376 to 375 sectors.
The kernel gains one sector; the current 192000-byte initrd still fits.
The low-memory ceiling remains `0x9A000`. Existing overlap and size checks remain enabled.
The initrd address is sector-aligned and paragraph-aligned; its archive reader does not require page alignment.
Both boot paths require rebuilt images after this change.

## Hardware evidence

The supplied photo shows EHCI PCI indices 3 and 9 started, alongside xHCI index 16, with no `/disk` entry.
The user selected Create boot + ext2 data image.
The generated 169869312-byte host image passes the managed-layout check and contains ext2 magic `53ef` at the expected offset.
This verifies the host artifact, not the physical stick or laptop reads.

## Validation and resource comparison

Validation commands:

```sh
cmake --build build --target os1_image os1_bios_image os1_usb_image
ctest --test-dir build-host-tests --output-on-failure
ctest --test-dir build --output-on-failure -R '^os1_smoke_ehci_storage_(bios|uefi)$'
ctest --test-dir build --output-on-failure
ctest --test-dir build --output-on-failure -R '^(os1_smoke|os1_smoke_bios|os1_smoke_usb_storage_bios)$'
ctest --test-dir build --output-on-failure -R '^os1_smoke_usb_storage_bios$' --repeat until-fail:10
```

The complete host suite passed 409 tests. BIOS and UEFI QEMU tests passed the new command checks and disk persistence checks.
They verify controller, root-port, storage-ready, and mount records; output redirection; and identical repeated snapshots.
The full root suite passed 39 of 42 tests in 354.01 seconds.
Two shell tests still expected the previous help text. The expected text now includes `usb`.
Both corrected tests passed on repeat.
The third failure was one BIOS xHCI storage boot with a general-protection fault during kernel thread restoration.
Its address resolves to `restore_frame_ptr.resume_kernel`, at `iretq`.
That test passed an immediate repeat and ten further repetitions in 16.42 seconds.
The boot fault remains unexplained. Passing repetitions do not establish its cause or close it.
Earlier [BIOS faults](../reviews/2026-11-03-review.md) exist, but this run does not establish a common cause.
No scheduler code or forbidden-fault assertions were changed.
All six EHCI tests passed, including BIOS and UEFI storage, combined-image updates, and failure handling.
Independent review found stale BIOS sector ranges in the architecture document.
The ranges were corrected, and a second read-only review passed with no remaining findings.
Review covered the isolated change, observation boundary, boot record lifetime, and BIOS staging alignment.
A third review verified the corrected help markers and found no direct diagnostic lifetime defect.
The diagnostic memory footprint and boot timing can still expose other faults. Full boot qualification remains incomplete.
Failure-path tests check controller and root records but do not assert each failure stage or status value.

The baseline is the completed EHCI working tree immediately before adding diagnostics, saved under `/tmp/os1-usbdiag-baseline`.
Both builds use GCC 11.2.0 for x86_64-elf on macOS 26.6.2 ARM64, with the existing size optimization and debug configuration.
Measurements use linked kernel sections from `x86_64-elf-size`; each deterministic build was measured once.

| Section | Baseline bytes | Candidate bytes | Change bytes | Relative change |
| --- | ---: | ---: | ---: | ---: |
| Text | 331776 | 335872 | +4096 | +1.23% |
| Data | 32768 | 32768 | 0 | 0% |
| BSS | 159744 | 176128 | +16384 | +10.26% |
| Total | 524288 | 544768 | +20480 | +3.91% |

The new snapshot array uses 16384 bytes in BSS; it performs no allocation and no runtime hardware I/O.
There is no additional DMA allocation. Boot capture adds bounded record copies and passive register reads.
No timing improvement is claimed. Physical timing and peak host memory remain unmeasured.
The existing BIOS staging, firmware ceiling, payload-size, and overlap checks pass after the one-sector redistribution.
Logs are retained in `build/usb-diagnostics-evidence/`.
Full ASD-STE100 dictionary compliance could not be verified because the dictionary was unavailable.

## Follow-up: storage admission failure

The next laptop snapshot identifies the stick behind EHCI PCI index 9, root port 1, hub address 1, downstream port 2.
Both hubs reach `hub-ready`. The stick reaches `storage-probe` and reports 30210842 sectors, or 15467951104 bytes.
No mount record appears. Storage admission fails before the filesystem is examined.
The first controller's other devices stop at `storage-config`; their identities remain unknown.

```text
INQUIRY -> TEST UNIT READY -> READ CAPACITY -> MODE SENSE -> write protection -> SYNCHRONIZE CACHE
                              passed             |                 |                  |
                                                 +------ possible failure ------------+
```

The current code assigns capacity only after successful readiness and capacity checks.
Thus the remaining candidates are MODE SENSE, write protection, or SYNCHRONIZE CACHE.
The snapshot does not distinguish them. Successful USB transfer status alone does not establish SCSI command success.

Record the exact initialization stage, opcode, command result, and mode byte.
Copy these values into the existing boot snapshot without changing command order, retries, or admission requirements.
Do not add requests or relax the required cache synchronization check.
Keep runtime I/O separate from boot evidence.

| Impact area | Assessment |
| --- | --- |
| Scope and requirements | GOALS storage milestone and USB storage admission rules; diagnose the physical rejection without assuming its cause. |
| Boot and platform | Shared xHCI/EHCI probe records more detail; retain both firmware paths and existing image bounds. |
| Memory and isolation | Fixed scalar fields and a kernel-only literal pointer; copy values into existing 64-byte observation records. |
| Concurrency | Initialization owns diagnostic fields; runtime I/O leaves them unchanged. No new locks or waits. |
| Interfaces | Refine storage device stage names and detail meaning; record layout, observation kind, descriptors, and Native IPC remain unchanged. |
| Resources | Measure linked kernel sections against the preceding diagnostic build. No extra USB commands, allocations, or snapshot records. |
| Validation | Test command rejection, transport failure, write protection, flush rejection, immutable boot evidence, and both QEMU firmware paths. |

Results encode `Ok=0`, `CommandFailed=1`, and `TransportFailed=2`.
Bits 15–8 remain zero. The mode byte is recorded only after MODE SENSE succeeds.
For example, `storage-mode-sense detail=0x1a010000` means MODE SENSE returned a command failure.
`storage-write-protect detail=0x1a000080` means the command succeeded and reported write protection.
`storage-flush detail=0x35010000` means SYNCHRONIZE CACHE returned a command failure.
These results do not identify the SCSI sense reason. They must not be interpreted as proof that a command is unsupported.

Follow-up validation commands:

```sh
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
cmake --build build --target os1_image os1_bios_image os1_usb_image
ctest --test-dir build --output-on-failure -R '^os1_smoke_(ehci|usb)_(storage|failure)_(bios|uefi)$'
ctest --test-dir build --output-on-failure -R '^os1_smoke_ehci_(storage|failure)_(bios|uefi)$'
```

All 411 host tests pass in 7.89 seconds. Both firmware images build within the existing staging limits.
Four xHCI storage and failure tests pass on BIOS and UEFI.
The first EHCI run exposed an incorrect assertion: writable QEMU media returns mode byte `0x10`, not zero.
The assertions now check opcode, result, reserved bits, and write protection while allowing other mode flags.
All four EHCI tests then pass in 38.92 seconds, including readonly admission evidence and persistent disk execution.
These QEMU runs use version 11.1.3, four CPUs, and 128 MiB RAM.
The earlier full-suite BIOS fault remains open; these targeted runs do not close it.

The baseline is `/tmp/os1-storage-probe-baseline/kernel.elf`, captured immediately before this follow-up.
Both builds use the same GCC 11.2.0 size-optimized configuration and macOS host described above.
One measurement per deterministic build uses `x86_64-elf-size` and symbol sizes from `x86_64-elf-nm -S`.

| Resource | Baseline bytes | Candidate bytes | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| Linked text | 335872 | 335872 | 0 | 0% |
| Linked data | 32768 | 32768 | 0 | 0% |
| Linked BSS | 176128 | 176128 | 0 | 0% |
| EHCI controller pool | 28672 | 29696 | +1024 | +3.57% |
| xHCI controller pool | 11648 | 11904 | +256 | +2.20% |

Existing section padding absorbs the additional code and fixed diagnostic fields; this does not mean their storage cost is zero.
The stripped BIOS kernel remains 373248 bytes. No new allocation, USB request, or runtime update is added.
Physical latency and peak host memory remain unmeasured; the next hardware snapshot must identify the actual rejection.
Follow-up logs are retained in `build/usb-diagnostics-evidence/storage-probe/`.

Independent review confirmed the assertion defect. The second review accepted its correction with no remaining findings.
Review covered the complete isolated change, result encoding, command equivalence, runtime evidence stability, test logs, and resource measurements.
The reviewer did not rerun tests or qualify physical hardware. Full ASD-STE100 dictionary compliance remains unverified.

## Follow-up: unsupported flush and cache policy

The next laptop snapshot reports `storage-flush detail=0x35010000` with the same capacity.
This identifies a SCSI command failure for SYNCHRONIZE CACHE(10), after successful writable-media checks.
It does not yet identify the sense reason or the device cache policy.

```text
Flush succeeds -----------------------------> admit with hardware flush
Flush command fails -> REQUEST SENSE
  invalid opcode -> current caching page -> WCE=0 -> admit write-through
  other error, unknown policy, or WCE=1 ------------> reject
```

Add bounded sense decoding and a cache-policy query after initialization flush rejection.
Allow write-through admission only for current invalid-opcode sense and an explicit disabled write cache.
Do not assume write-through behavior when the caching page is absent or malformed.
Retain transport failure retirement and exact-length validation for ordinary reads and writes.
Accept consistent short input only when a bounded metadata caller requests the actual length.
Record sense key, ASC, and ASCQ when flush admission still fails.

| Impact area | Assessment |
| --- | --- |
| Scope | Preserve durable storage semantics while supporting devices that explicitly report write-through operation. |
| Boot/platform | Shared BOT probe affects xHCI and EHCI on both firmware paths; preserve staging bounds. |
| Memory/isolation | Bounded sense and mode buffers; validate lengths before field access. No user pointers or DMA policy changes. |
| Concurrency | Existing controller serialization remains; dead devices cannot report successful no-op flush. No new locks. |
| Interfaces | Preserve BlockDevice flush semantics; hardware flush or confirmed write-through completion satisfies synchronization. Native layouts remain unchanged. |
| Resources | Measure section and pool sizes; one sense query after command rejection, then one cache query only after invalid-opcode sense. |
| Validation | Fault-injected BOT responses, cache-page bounds, sense formats, dead-device handling, BIOS/UEFI xHCI/EHCI checks, and independent review. |

[Linux SCSI documentation](https://www.kernel.org/doc/html/latest/scsi/sd-parameters.html) identifies WCE=0 as disabled write caching.
[T10 cache semantics](https://www.t10.org/ftp/t10/document.03/03-388r2.pdf) describe write-back completion and cache control.
Physical persistence and the earlier intermittent BIOS fault remain unqualified.

### Cache-policy validation

All 416 host tests pass in 8.11 seconds.
They cover fixed and descriptor sense, explicit write-through writes, dead-device flush, malformed pages, enabled write cache, and deferred errors.
They also verify that short metadata requires matching residue and explicit opt-in; ordinary data transfers retain exact-length checks.
All eight xHCI/EHCI storage and failure checks pass on BIOS and UEFI in 63.57 seconds.
These runs use QEMU 11.1.3 with four CPUs and 128 MiB RAM.
QEMU exercises the existing hardware-flush path. Host BOT fixtures exercise unsupported-flush admission; physical qualification remains pending.

```sh
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
cmake --build build --target os1_image os1_bios_image os1_usb_image
ctest --test-dir build --output-on-failure -R '^os1_smoke_(ehci|usb)_(storage|failure)_(bios|uefi)$'
```

The baseline is `/tmp/os1-cache-policy-baseline/kernel.elf`, captured before this change.
Both builds use the existing GCC 11.2.0 size-optimized configuration and macOS host described above.
One deterministic measurement uses `x86_64-elf-size`; controller pool sizes use `x86_64-elf-nm -S`.

| Resource | Baseline bytes | Candidate bytes | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| Linked text | 335872 | 335872 | 0 | 0% |
| Linked data | 32768 | 32768 | 0 | 0% |
| Linked BSS | 176128 | 176128 | 0 | 0% |
| EHCI controller pool | 29696 | 30208 | +512 | +1.72% |
| xHCI controller pool | 11904 | 12032 | +128 | +1.08% |

Existing section padding absorbs the extra code and 640 bytes of fixed controller storage.
BIOS and UEFI images build within unchanged staging limits.
Without unit-attention retries, the successful hardware-flush path retains five probe commands.
The write-through fallback uses seven commands, an increase of two commands or 40% against that sequence.
Its sense and mode buffers contain 82 bytes. No new heap or DMA allocation is required.
Physical latency, physical power-loss persistence, and peak host memory remain unmeasured.
Logs are retained in `build/usb-diagnostics-evidence/cache-policy/`.
Independent review found a missing multiple-of-eight check for MODE SENSE(6) block descriptor lengths.
The parser now rejects that malformed response, with a dedicated regression test.
All 417 host tests pass after correction in 7.54 seconds.
All four BIOS/UEFI xHCI/EHCI storage checks pass on the corrected build in 21.04 seconds.
A second independent review accepted the correction with no remaining findings.
The reviewer inspected source and logs without rerunning tests. Full ASD-STE100 dictionary compliance remains unverified.

## Subsequent hardware result

The user subsequently confirmed successful mounting and file persistence across reboot on the Asus G74SX.
The [hardware validation record](2026-11-06-usb-hardware-validation.md) separates observed behavior from remaining qualification gaps.
Earlier pending-hardware statements above describe the state when each implementation check ran.

## Subsequent interrupt-return investigation

The [interrupt-return investigation](../kernel/2026-11-06-interrupt-return.md) reproduced the retained BIOS fault and corrected interruptible scheduler handoff.
The earlier unresolved statements above describe the evidence available during this report.
Physical validation of the scheduler correction remains separate from the USB persistence result.
