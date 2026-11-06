# USB keyboard firmware handoff — 2026-11-06

This change addresses the controller routing failure observed on the Gigabyte H81M-S1.
The keyboard has no effect because os1 does not enumerate it on the supported xHCI path.
The second physical capture confirms handoff and routing, but keyboard enumeration still fails during control transfers.
The [control-transfer report](2026-11-06-xhci-control-transfers.md) records that next failure and correction.
The original measurements and acceptance status below describe this handoff revision.

```text
PCI discovery -> early xHCI probe -> request firmware ownership
    -> disable legacy SMIs -> reset and start xHCI
    -> Intel supported-port routing -> connection settling -> device enumeration
    -> other PCI probes, including EHCI -> filesystem discovery
```

## Hardware evidence

The returned diagnostic image matches the built kernel.
The saved UEFI log contains 9,602 captured bytes and reaches the shell.
Its journal records successful recovery on boot 2 from boot 1.

| Observation | Result |
| --- | --- |
| Controller | Intel `8086:8c31` at `00:14.0` |
| Legacy support | `0x00010001`: BIOS ownership set, OS ownership clear |
| USB 2.0 routing | `0x0`, supported mask `0xfff` |
| USB 3.0 enable | `0x0`, supported mask `0x3` |
| xHCI initialization | Successful; no connected devices enumerated; zero configured keyboards |
| USB stick | Working through EHCI `00:1a.0`, root hub 1, downstream port 1 |
| Watchdog | Firmware locks `NO_REBOOT`; automatic reset is unavailable |

Linux routes keyboard `17ef:6190` directly through xHCI on the same rear port.
The current EHCI profile supports high-speed storage, not this low-speed boot keyboard.
The earlier polling-interval defect does not explain failure before enumeration and remains separate.

Evidence files remain under `local-evidence/keyboard-2026-11-06/` and are excluded from Git.
Returned-image SHA-256: `0026f4c221a778b65ef7d1a9d35be763f3ce11b90de69ca479dd540ce3a49836`.
Captured-kernel SHA-256: `c2f99ad316b272265d18811791ac5fbcece292e12301adb16ecc1d756110a453`.

## Implementation plan

1. Request OS ownership when the bounded xHCI legacy-capability walk succeeds.
2. Wait at most one second for BIOS release, using calibrated delays.
3. Disable legacy SMI enables and acknowledge their write-one-to-clear status bits.
4. Reject malformed capabilities or failed handoff before controller reset or DMA publication.
5. Probe early drivers before other devices, independent of PCI discovery order.
6. Enable routing only for Intel `8086:8c31`, after xHCI starts and before device enumeration.
7. Enable supported SuperSpeed ports before switching supported USB 2.0 ports.
8. Preserve unrelated register bits, verify writes, and restore original routing if verification fails.
9. Allow a bounded connection-settling delay after routing changes.
10. Test handoff outcomes, register preservation, routing failures, probe order, and USB input/storage coexistence.

The legacy-capability registers are optional. Their absence does not prevent ordinary xHCI startup.
A timed-out BIOS retains control; this change does not force-clear BIOS ownership.
The early-probe mechanism preserves first-matching-driver selection and isolates individual probe failures.
The `os1.noxhci` boot option leaves Intel routing unchanged.

## Impact assessment

| Area | Assessment |
| --- | --- |
| Goals | Implements Real hardware enablement and fixed driver boundaries in GOALS. |
| Boot/platform | Shared UEFI/BIOS startup gains bounded ownership waits and ordered USB probes. |
| Memory/isolation | Existing MMIO bounds and resource ownership remain; failed handoff must not stop firmware hardware. |
| Concurrency | Routing occurs during BSP boot before user processes and before EHCI device publication. |
| Interfaces | No syscall, Native API, descriptor, or object protocol changes. PCI driver metadata gains an internal early-probe flag. |
| Resources | One-second maximum handoff wait; bounded settling after routing changes. Measure binary footprint and boot time. |
| Validation | Host register/probe-order tests, normal and diagnostic UEFI/BIOS boots, and USB storage regression checks. |

Relevant requirements are [Platform discovery](../../GOALS.md#platform-discovery), [Real hardware enablement](../../GOALS.md#real-hardware-enablement),
and the [architecture](../ARCHITECTURE.md).
The [Native API index](../native-api/README.md) remains authoritative for unchanged API profiles.

The register handling follows the [Linux firmware handoff and Intel routing implementation](https://github.com/torvalds/linux/blob/master/drivers/usb/host/pci-quirks.c)
and its [legacy capability definitions](https://github.com/torvalds/linux/blob/master/drivers/usb/host/xhci-ext-caps.h).

## Validation

The host suite passes all 430 tests.
It covers ownership release, timeout, malformed capabilities, read-only status, routing preservation, rollback, and probe ordering.
The three diagnostic capture tests pass with and without a keyboard, including UEFI and BIOS warm-reset capture.
All four xHCI/EHCI storage checks pass across UEFI and BIOS, including persistence and keyboard coexistence.
The tooling contract checks pass; the monitor socket test requires execution outside the local sandbox.
CI now builds the separate diagnostic profile and runs the three diagnostic checks.
QEMU cannot establish recovery on the physical Intel controller.

Commands:

```sh
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
cmake --build build/keyboard-normal --target os1_usb_image os1_image user_programs
cmake --build build/usb-write/keyboard-diagnostic --target os1_usb_image
ctest --test-dir build/usb-write/keyboard-diagnostic --output-on-failure -R '^os1_smoke_usb_keyboard_'
ctest --test-dir build/keyboard-normal --output-on-failure -R '^os1_smoke_(usb_storage|ehci_storage)_(uefi|bios)$'
```

Independent review found that the first handoff check incorrectly rejected read-only event/error status bits.
The correction checks only the five writable SMI enable bits, `0xe011`.
A regression test retains each read-only status bit and verifies successful handoff with SMI enables cleared.
The [Intel register definition](https://edc.intel.com/content/www/id/id/design/publications/14th-generation-core-processors-ioe-p-registers/usb-legacy-support-control-status-usblegctlsts-offset-8470/) specifies these access types.
Independent review completed three cycles with no remaining substantive findings.
It also corrected the physical procedure to separate diagnostic capture from data-preservation acceptance.
Full ASD-STE100 dictionary compliance is unverified because the complete dictionary is unavailable in this session.

## Resource measurements

The comparison uses main `f1aaf804` and the corrected working tree after diagnostic commit `bdf9b24`.
The final stripped kernel identifies the candidate independently of its later commit metadata.
Its SHA-256 is `a24fc1e8cadd62de2d46d2972059e905f08bfc416f4bad2011aeb2a09e02c478`.

Both builds use GCC 11.2.0, Debug configuration, kernel `-Os`, allocator diagnostics, and disabled debugcon output.
The host is macOS ARM64. QEMU 11.1.3 uses TCG, q35, four CPUs, and 128 MiB RAM.
UEFI uses Homebrew `edk2-x86_64-code.fd` with fresh `edk2-i386-vars.fd` copies.
Boot timing includes the ordinary image's ten-second boot menu.

| Property | Main | Candidate | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| Text and read-only data | 335,872 B | 339,968 B | +4,096 B | +1.22% |
| Data | 32,768 B | 32,768 B | 0 B | 0% |
| BSS | 176,128 B | 180,224 B | +4,096 B | +2.33% |
| Stripped kernel | 368,896 B | 372,992 B | +4,096 B | +1.11% |
| USB boot image | 35,651,584 B | 35,651,584 B | 0 B | 0% |
| Initrd | 192,000 B | 192,000 B | 0 B | 0% |
| Median UEFI boot, three runs | 12.230 s | 12.234 s | +0.004 s | +0.033% |
| Normal serial output, including injected key | 25,870 B | 25,891 B | +21 B | +0.081% |

Main boot samples are 12.263, 12.230, and 12.222 seconds.
Candidate samples are 12.255, 12.211, and 12.234 seconds.
Each run reaches the shell and accepts a QEMU-injected USB key.
Logs are under `build/handoff-evidence/`; timings do not show a meaningful QEMU regression.

The stripped kernel fits the 373,248-byte BIOS staging slot with 256 bytes available.
No staging limit changed. No new runtime allocation or disk I/O occurs during handoff.
The ownership polling budget is 1,000 waits of 1 ms each, plus register access and loop overhead.
A changed Intel routing configuration adds a 100 ms busy wait.
QEMU does not exercise this Intel-specific delay or firmware release timing.
Physical boot latency, CPU cost during firmware release, and keyboard recovery remain unmeasured.

## Physical acceptance

Use a spare stick for diagnostics; this menu action erases the selected USB device and creates no ext2 data partition.
Build through `./os1.sh`, then select USB and `Create keyboard diagnostic image (erase USB)`.
Boot the desktop with the keyboard in the same rear port.
Check that typed commands reach the shell.
If input still fails, restart into the capture preloader and return the USB log for analysis.
The desktop's locked `NO_REBOOT` bit requires a manual warm restart for capture.

Test storage separately with an existing os1 boot-plus-ext2 stick.
Select `Update boot files (preserve data)` in the USB menu.
Boot that stick, check keyboard input, and verify that existing `/disk` files remain available.
If no data stick exists, use another spare stick with `Create boot + ext2 data image (erase USB)`.
Write a test file, synchronize storage, and verify the file after reboot.
Keep issue #27 open until physical keyboard acceptance succeeds.
