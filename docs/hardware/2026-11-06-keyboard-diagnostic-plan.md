# USB keyboard diagnostic boot — 2026-11-06

This historical report records the diagnostic implementation before the returned USB log was available.
The [handoff report](2026-11-06-usb-keyboard-handoff.md) contains the subsequent evidence, correction, and validation.
Measurements and artifact hashes below apply to the earlier diagnostic build.

This task collects evidence for issue #27 without requiring a working os1 keyboard.
Key presses have no effect on the affected desktop. Sluggish input is not the reported failure.

```text
os1.sh USB menu -> separate diagnostic build -> verified USB write
    -> automatic diagnostic boot -> USB summary and bounded capture
    -> warm restart -> firmware saves log -> collect log on development computer
```

## Evidence and scope

The desktop has a Gigabyte H81M-S1 motherboard and a Lenovo Calliope USB G2 keyboard, `17ef:6190`.
The keyboard connects to a rear motherboard port.
Linux reports Intel xHCI `8086:8c31` and EHCI controllers `8086:8c2d` and `8086:8c26`.
Linux connects the keyboard directly to xHCI root port 4 at low speed.
Interface 0 is a boot keyboard with an eight-byte interrupt IN endpoint, `0x81`, and `bInterval=10`.
Interface 1 is another HID interface. The os1 controller path remains unknown.

Missing ownership handoff and Intel port routing are hypotheses, not confirmed causes.
The separate interval-conversion defect does not establish the cause of absent input.
This task changes neither port routing nor endpoint scheduling policy.

## Plan and acceptance

1. Add bounded, read-only ownership and port observations before xHCI reset.
2. Record device identities, enumeration stages, command results, and control-transfer results.
3. Record a bounded sample of input completion and delivery counts without recording typed characters.
4. Display a compact controller/device summary before starting the shell.
5. Add a separate USB diagnostic build and automatic diagnostic menu default.
6. Reuse the verified writer, terminal restoration, progress display, and limited sudo boundary.
7. Test register bounds, menu isolation, diagnostic packaging, and both boot paths.
8. Obtain an independent review before completion.

The image must boot into diagnostics without a menu key press.
The normal USB build must retain its ordinary configuration.
A successful capture must not depend on entering a shell command.
Automatic warm restart depends on the existing Intel TCO watchdog; a physical reset remains the fallback.
Physical keyboard recovery is outside this task's acceptance criteria.

## Impact assessment

| Area | Assessment |
| --- | --- |
| Goals | Supports Real hardware enablement and serial-first debugging in GOALS. |
| Architecture | Extends Build Outputs And Boot Artifacts and existing boot-log diagnostics. |
| Boot and platform | Uses existing BIOS capture and UEFI pre-loader; dedicated configuration selects diagnostics automatically. |
| Memory and isolation | Bounds extended-capability reads to the claimed MMIO region. Retains existing boot-log capacity and ownership. |
| Concurrency | Bounds runtime samples under existing event/input locks. No new blocking waits or routing writes. |
| Interfaces | No syscall, Native API, descriptor, or public binary-format change. |
| Runtime resources | Extra diagnostic I/O changes timing. Measure image/kernel sizes and comparable QEMU boot times. |
| Validation | Host bounds tests, menu contracts, UEFI/BIOS smoke checks, and capture-path checks. Physical desktop acceptance remains pending. |

The [Native API index](../native-api/README.md) defines ABI status; this work changes no implemented profile.
The [architecture](../ARCHITECTURE.md) and [USB workflow](usb-image-workflow.md) define the affected subsystem boundaries.

## Validation record

Implementation and automated checks are complete. Desktop evidence collection remains pending.

### Commands and results

| Check | Result |
| --- | --- |
| `cmake --build build-host-tests` and `ctest --test-dir build-host-tests --output-on-failure` | 420 host tests passed, including three new ownership-table tests. |
| `python3 -B tests/scripts/test_usb_menu.py` | 22 tests passed, including build isolation and menu behavior. |
| `ctest --test-dir build/keyboard-normal --output-on-failure -R 'contract$'` | Ten passed initially; the monitor test needed local-socket access outside the sandbox. Its rerun passed. |
| `ctest --test-dir build/usb-write/keyboard-diagnostic --output-on-failure -R '^os1_smoke_usb_keyboard_'` | Three passed: UEFI, BIOS, and UEFI without a keyboard. |
| Normal USB image, UEFI and BIOS | Both reached the shell and echoed a QEMU-injected USB key. |
| Documentation and shell checks | 174 local link destinations exist; `git diff --check` and `sh -n os1.sh` passed. |

The diagnostic smoke boots the generated USB disk without serial input or monitor commands before the shell.
It checks ownership, port, enumeration, and transfer records.
It injects one key only in the keyboard-present cases.
It then requests a QEMU warm reset and reads the saved capture from the disk image.
The UEFI test also requires the saved-log banner.
The keyboard-absent test requires a zero-keyboard summary and saves the log without injecting any key.
These checks exercise four virtual CPUs and 128 MiB of guest RAM.

The first smoke fixture assumed controller port 4. QEMU instead exposed the device on companion port 8.
The corrected fixture derives the port from the enumeration record.
An initial QEMU launch failed because the sandbox prohibited its local monitor socket.
The authorized rerun outside that restriction passed.
Neither test failure established a guest keyboard defect.

### Resource comparison

Baseline revision: `f1aaf804377547a3353dc18bb74b2c73557936bd`, built from a local archive with unchanged dependency checkouts.
Candidate: this branch's working tree; the final stripped kernel SHA-256 is recorded below.
Both builds used GCC 11.2.0, Debug configuration, kernel size optimization, allocator debugging, and disabled debugcon.
The host was macOS ARM64. QEMU 11.1.3 used TCG, q35, four CPUs, and 128 MiB RAM.
UEFI used Homebrew `edk2-x86_64-code.fd` and a fresh copy of `edk2-i386-vars.fd` per run.
Each image booted through QEMU USB storage with a separate xHCI keyboard.

| Property | Baseline | Normal candidate | Change |
| --- | ---: | ---: | ---: |
| Stripped BIOS kernel | 368,896 bytes | 372,992 bytes | +4,096 bytes (+1.11%) |
| Kernel text and read-only sections | 335,872 bytes | 339,968 bytes | +4,096 bytes (+1.22%) |
| Kernel data section | 32,768 bytes | 32,768 bytes | 0 bytes (0%) |
| Kernel BSS section | 176,128 bytes | 176,128 bytes | 0 bytes (0%) |
| USB image | 35,651,584 bytes | 35,651,584 bytes | 0 bytes (0%) |
| Median UEFI boot to shell | 12.206 s | 12.251 s | +0.045 s (+0.37%) |

Three sequential repetitions produced baseline times of 12.201, 12.206, and 12.246 seconds.
Normal candidate times were 12.268, 12.091, and 12.251 seconds.
Diagnostic times were 2.379, 2.410, and 2.394 seconds; the median was 2.394 seconds.
The diagnostic profile removes the ordinary ten-second menu countdown, so this comparison does not measure driver speed.
Its median is 9.812 seconds shorter than baseline, a reduction of 80.39% caused mainly by menu configuration.
Normal BIOS boot took 0.724 seconds in one functional run; no BIOS timing baseline was collected.

A longer help message initially exceeded the BIOS staging slot by one page.
Shorter diagnostic text restored the existing layout without reducing initrd capacity or changing firmware-memory limits.
The final stripped kernel fits the 373,248-byte slot with 256 bytes remaining.
The initrd remains 192,000 bytes. Runtime log capacity and image partitions are unchanged.
Additional diagnostic logging changes timing and performs existing boot-capture cache flushes.
Physical interrupt latency, CPU cost, and warm-reset recovery remain unmeasured until the desktop test.

Evidence remains under `build/keyboard-evidence/` and the diagnostic build's `Testing/Temporary/LastTest.log`.
The repeated workload is `tests/scripts/test_usb_keyboard_diagnostics.py`, with `--normal` for ordinary images.
The new MMIO helper never writes registers and bounds traversal to 256 entries and the claimed BAR.
Detailed samples stop after 32 commands per controller, 32 control transfers per device, and eight input completions per device.
Console dispatch sampling stops after eight nonempty batches.

Final stripped-kernel SHA-256:

```text
47f309cd0b7ef7af98f646295e8fa14b2bbcdcbff13faff9ef6cf11649595258
```

Diagnostic image SHA-256:

```text
9747b5dcccbaeb6ece760c9e87390de45c7230013bcd315f92da7cb1946e3961
```

### Review and remaining hardware acceptance

An independent read-only review found no blocking implementation defect in the first pass.
It confirmed resolution of the QEMU port-number fixture error.
The final independent review covered all corrections, capture results, measurements, and artifact identities.
It found no substantive blocking defect. Generated Python cache files were removed.
The desktop root cause remains unconfirmed. The user must collect the diagnostic log from the same rear port.
No USB disk was written during automated validation.

Full ASD-STE100 dictionary compliance is unverified because the complete dictionary is unavailable in this session.
