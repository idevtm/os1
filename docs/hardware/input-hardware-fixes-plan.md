# Returned hardware results and input corrections

This work records the returned laptop and desktop tests and corrects three capture or USB defects.
The [subsequent retest](input-failure-diagnostics-plan.md#hardware-retest-november-10-2026) confirms report capture and pointer movement.
Input reliability still requires investigation and another physical test.

```text
returned ext2 image -> preserved logs and reference comparison
  -> bounded report streaming
  -> EHCI endpoint-sized input
  -> xHCI control-packet negotiation and composite input
  -> host checks -> full local suite -> independent review -> hardware retest
```

## Requirements and impact

| Area | Requirement and effect |
| --- | --- |
| Scope | GOALS, Driver model and Testability and observability: correct observed failures without adding general HID report parsing. |
| Boot and platform | ARCHITECTURE, Input services and attachment changes: preserve UEFI, BIOS, storage, and keyboard operation. |
| Memory and isolation | Input ABI, Bounds and failures: bound receive buffers, descriptor validation, and archive members. Preserve DMA quarantine. |
| Concurrency | Keep xHCI event state under its existing lock. Retire every input interface when a composite device fails or disconnects. |
| Interfaces | Preserve native input records and archive names. Support boot keyboard and mouse interfaces on one xHCI attachment. |
| Runtime resources | Compare image sizes and runner duration with revision 65fae14. Measure archive bytes and added endpoint resources. |
| Validation and documents | Test large captures, packet negotiation, composite selection, report lengths, failure handling, and both firmware paths. |

`BootInfo`, syscall layouts, permissions, filesystem quotas, and persistent deployment rules do not change.
The Native API index classifies the input ABI as implemented; general HID decoding remains outside this profile.
The full ASD-STE100 dictionary is unavailable. Technical prose receives manual review against the available project rules.

## Hardware results: November 10, 2026

The returned runs identify revision `7fb69e73923a9450de6f8d8daae8cddaa067d19c`.
The [saved evidence summary](2026-11-10-returned-input-evidence.json) records image and archive hashes.
Their build hash is `02eca581020d5708e34ebf9802f94a07e8ac9071fb37d3dfe197c6bcc25b93de`.
Both runs used normal logging, without physical stimulus observation or an expected USB mouse requirement.
The copied ext2 image passes `e2fsck -fn`; both saved 4096-byte storage probes match the expected pattern.

| Evidence | Asus G74SX laptop | Gigabyte H81M-S1 desktop |
| --- | --- | --- |
| Machine fingerprint | `da00c1febcd0571d` | `a15c974cde8bf3c2` |
| Boot and run | Boot 1, run 1 | Boot 2, run 1 |
| PCI comparison | 18/18 match Linux identities and classifications | 17/17 match Linux identities and classifications |
| Runner outcomes | 14 PASS, 1 FAIL, 3 SKIP | 14 PASS, 1 FAIL, 3 SKIP |
| Failed report | `lshw`, status -23; 65,536 bytes | `lshw`, status -23; 65,532 bytes |
| Available input | PS/2 keyboard and relative touchpad | Lenovo USB keyboard and PS/2 keyboard entry |
| Attached USB pointer | Wired SIGMACHIP mouse, `1c4f:0034` | Logitech G502 Lightspeed receiver; Linux reference `046d:c539` |
| Failure | Four EHCI attach/fail/remove cycles; token `0x30d52` | xHCI port 6 device-descriptor failure; identity unavailable |

The source image, decoded logs, comparison, and hashes remain in `build/hardware-acceptance-2026-11-10/`.
The original failures remain separate from subsequent local validation.
Neither API acquisition nor subscription checks establish physical movement delivery.
These runs do not qualify diagnostic autorun or retained handles during hotplug.

The laptop comparison also reports missing Bluetooth identity `0cf3:3005` and observed identity `13d3:3304`.
Linux identifies the latter as AR3011 with SFLASH firmware.
A firmware-dependent identity change is plausible, but remains unconfirmed for this capture.
The comparison failure remains visible; no reference profile is weakened.
The Kingston boot stick is an expected difference from the Linux reference capture.

## Investigation and implementation sequence

1. Preserve baseline runner checks, artifact sizes, and the returned evidence.
2. Stream inventory reports into bounded tar members, without temporary-file staging.
3. Receive complete EHCI endpoint packets; decode only the supported boot fields.
4. Read the initial eight device-descriptor bytes before requesting the full xHCI descriptor.
5. Validate the advertised control packet size and apply it through Evaluate Context.
6. Configure bounded keyboard and mouse endpoints for composite xHCI devices.
7. Add regression checks, run the complete local suite, and obtain independent review.

The laptop endpoint permits four-byte packets; the baseline transfer requests only three bytes.
Its failure token reports a halted transfer with babble.
This identifies a buffer-sizing defect, but does not prove that no additional physical-controller defect exists.

The desktop reference advertises a 32-byte control packet and separate boot keyboard and mouse interfaces.
The baseline xHCI code retains an eight-byte full-speed packet size and selects only the first HID interface.
Correct both defects before the hardware retest.
Preserve useful control-transfer failure details in normal logs without capturing report payloads.

## Retest

Update the kernel and deployed programs through the preserving USB menu.
Keep the wired mouse or receiver connected before boot.
Select `os1 diagnostics` to verify automatic capture.
Run the following command on the laptop:

```text
run-tests --machine laptop --input --expect-usb-mouse
```

Run the following command on the desktop:

```text
run-tests --machine desktop --input --expect-usb-mouse
```

During observation, move each pointer and press and release each button.
Press and release Left Shift on each keyboard.
Retain an open pointer object before disconnecting its USB device.
Verify its disconnected state after removal; then verify a new attachment after reconnect.
Run another capture after reconnect, then use `shutdown` before returning the stick.

## Local validation record

The first baseline attempt could not create QEMU sockets inside the sandbox.
The unchanged baseline passed after local socket access was authorized: BIOS 33.94 seconds; UEFI 36.47 seconds.
Both checks verify result preservation and input observation across two guest boots.
Evidence remains in `build/input-hardware-fixes/baseline/`.

The first targeted candidate run passed runner and native-input checks on BIOS and UEFI.
The first host regression used the packet-size byte instead of the descriptor-length byte for a malformed-record test.
The corrected fixture changes the length byte; production assertions remain intact.
Original output remains in `build/input-hardware-fixes/host-targeted.log`.

### Source references

- [USB HID specification, Appendix B.2](https://www.usb.org/sites/default/files/hid1_12.pdf): boot mouse fields and extension bytes.
- [xHCI specification](https://www.intel.ie/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf): endpoint contexts and packet-size updates.
- [Linux xHCI source](https://github.com/torvalds/linux/blob/master/drivers/usb/host/xhci.c): control endpoint packet-size evaluation.
- [Linux AR3011 driver](https://github.com/torvalds/linux/blob/master/drivers/bluetooth/ath3k.c): firmware-dependent Bluetooth identification.

## Resource comparison

[Measurement samples](2026-11-10-input-hardware-fixes-measurements.json) record revisions, build settings, firmware hashes, workloads, and limitations.
The original baseline hashes and embedded emulator revision were not retained; the measurement file identifies this provenance limit.
The comparison uses Q35, four CPUs, 128 MiB RAM, TCG, and two concurrent guests.
Each firmware profile has two default command samples within one runner-test invocation per tree.

| Property | Baseline | Candidate | Difference |
| --- | ---: | ---: | ---: |
| Kernel ELF file | 614,952 bytes | 616,328 bytes | +1,376 bytes; +0.224% |
| `run-tests` ELF file | 29,688 bytes | 30,768 bytes | +1,080 bytes; +3.638% |
| BIOS command median | 1.7705 seconds | 2.1474 seconds | +0.3769 seconds; +21.287% |
| UEFI command median | 1.5680 seconds | 1.5683 seconds | +0.0003 seconds; +0.017% |
| BIOS first-run files | 115,862 bytes | 115,864 bytes | +2 bytes; +0.0017% |
| UEFI first-run files | 115,862 bytes | 115,863 bytes | +1 byte; +0.0009% |

These short timing samples do not establish a performance trend.
The first BIOS candidate command took longer; the UEFI median was almost unchanged.
Timestamp and counter text can change stored byte counts.
The archive format and five-file result layout remain unchanged.

Each second xHCI input interface requires two DMA pages, or 8192 bytes.
Non-HID xHCI attachments no longer allocate those two unused input pages.
Inventory streaming adds a 4096-byte stack buffer and avoids report-sized temporary-file allocation.
Peak process memory, physical latency, and composite-controller operation remain unmeasured.
The user hardware retest is the next validation step for those controller paths.

## Final validation and review

`./scripts/check-local.sh` passed on the final executable and test tree before commit.

| Profile | Result | Duration |
| --- | --- | --- |
| Host tests | 664/664 passed | 21.66 seconds |
| Unfiltered normal images | 92/92 passed | 466.06 seconds |
| Diagnostic images | 3/3 passed | 9.51 seconds |

Full output: `build/input-hardware-fixes/full-local-first.log`.
Profile logs: `build/input-hardware-fixes/final-suite/`.
The candidate includes the hardware corrections on source revision `65fae14`.
No executable changes followed this validation run.

The independent adversarial source review found no blocking defects.
The documentation review verified measurement arithmetic, artifact sizes, and local links.
It corrected an unsupported claim that baseline manifests remained available after test cleanup.
It also labeled the old USB behavior as baseline behavior.
Scoped formatting, whitespace, local document file links, and credential-signature checks passed.
The review did not execute code; the implementer ran the recorded validation commands.
Physical confirmation remains pending for both USB pointers.
