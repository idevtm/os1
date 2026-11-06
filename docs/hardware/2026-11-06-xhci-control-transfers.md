# xHCI control-transfer correction — 2026-11-06

The second desktop capture confirms ownership transfer and port routing, but device descriptor reads still fail.
This change corrects endpoint-zero transfer construction and completion tracking. Physical keyboard acceptance remains pending.

```text
setup TD -> optional data TD -> status TD -> completed request
                | short packet: retain actual byte count, wait for status
any stage error -> failed request -> quarantine device DMA
```

## Evidence and plan

The returned image contains the kernel from `d8b9c46`.
Its SHA-256 is `46433b735993b1ad0e98b877783c7b1e281af3a97d57d9bda13eb0da7eccf3b2`.
The saved log contains 11,034 captured bytes.
Firmware handoff succeeds; USB2 routing becomes `0xfff` and USB3 routing becomes `0x3`.
Ports 4, 5, and 6 become visible. The controller receives 13 interrupts.
Port 4 returns completion code 13, an invalid device identity, then a configuration-descriptor timeout.
No keyboard reaches configured state.

The hypothesis is incorrect control-stage boundaries and incomplete transfer-event accounting.
The captured `d8b9c46` code chains setup and data stages and tracks only the status-stage TRB.
The [xHCI specification](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf), section 4.11.2.2, requires separate stage descriptors.
The [Linux control-transfer implementation](https://github.com/torvalds/linux/blob/master/drivers/usb/host/xhci-ring.c) also retains short data lengths until status completes.
The old setup TRB sets reserved bit 4; its data TRB incorrectly chains into the status stage.
These are confirmed construction defects. Physical recovery after correction remains unverified.

1. Keep setup, data, and status stages separate; enable short-packet events for IN data.
2. Publish setup only after the complete request and completion state are ready.
3. Track all stage addresses; retain data residuals until successful status completion.
4. Fail immediately on stage errors or invalid residual lengths; retain DMA quarantine rules.
5. Validate descriptor headers before reading fields or configuring endpoints.
6. Test full, short, zero-length, error, stale-event, and ring-wrap cases.
7. Run host tests and UEFI/BIOS keyboard, capture, and USB storage checks.

## Impact assessment

| Area | Assessment |
| --- | --- |
| Scope | Correct the existing kernel USB transport; retain bounded, readable control requests. |
| Boot/platform | Both UEFI and BIOS use this path after firmware handoff. BIOS staging reclaims one unused page; BootInfo and runtime mappings do not change. |
| Memory/isolation | DMA buffers remain pinned after publication. Errors preserve device quarantine; descriptor bounds precede field use. |
| Concurrency | Completion state stays under the event lock. Setup publication follows all request writes. |
| Interfaces | No Native API, syscall, descriptor-handle, IPC, or executable-format changes. |
| Resources | Measure kernel size, static memory, and normal boot time against `d8b9c46`. No new heap allocation is planned. |
| Validation | Host state-machine and wire-layout regressions, four-CPU QEMU checks, then a separate physical retest. |

Requirements: [Real hardware enablement](../../GOALS.md#real-hardware-enablement),
[USB firmware handoff](../ARCHITECTURE.md#usb-firmware-handoff-and-probe-order),
and unchanged [Native API profiles](../native-api/README.md).
The [earlier handoff report](2026-11-06-usb-keyboard-handoff.md) records the first correction.

## Validation

The host suite passes 440 tests, including ten new control-transfer and descriptor-validation tests.
All three keyboard diagnostic capture checks pass across UEFI and BIOS.
Eight USB storage, combined-image, and preserving-deployment smokes pass across both boot paths.
All eleven selected tooling contracts pass.
A negative assembly check moves staging to `0xE000` and confirms rejection for page-table overlap.
QEMU cannot prove physical desktop recovery.

```sh
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
cmake --build build/keyboard-normal --target os1_usb_image os1_image user_programs
cmake --build build/usb-write/keyboard-diagnostic --target os1_usb_image
ctest --test-dir build/usb-write/keyboard-diagnostic --output-on-failure -R '^os1_smoke_usb_keyboard_'
ctest --test-dir build/keyboard-normal --output-on-failure -R '^os1_smoke_(usb_storage|ehci_storage|usb_data|usb_deploy)_(uefi|bios)$|contract$'
```

Local test summaries are `/tmp/control-all-host.log`, `/tmp/control-capture-tests.log`, and `/tmp/control-storage-tests.log`.
Independent review covered transfer stages, completion ordering, descriptor bounds, DMA lifetime, staging, and test evidence.
The review found no blocking code defects. Two documentation inconsistencies were corrected.
The CI compiler must also validate the final kernel against the expanded slot.
The complete ASD-STE100 dictionary is unavailable; full dictionary compliance remains unverified.


## CI staging correction

The preceding PR revision failed CI before smoke tests.
The runner produced a 377,088-byte kernel; the BIOS slot allowed only 373,248 bytes.
Local GCC 11.2.0 produced a smaller file, so local builds did not reveal this limit.

The BIOS loader uses five page-table pages in `[0xA000,0xF000)`.
Move kernel staging from `0x10000` to `0xF000` to use the adjacent free page.
The new slot holds 377,344 bytes. The initrd retains `[0x6b200,0x9A000)` and 192,000 bytes.
An assembler assertion rejects page-table overlap. Existing file-size and physical-load checks remain enabled.
The initrd disk slot shifts by eight sectors; generated loader constants follow that shift.
Boot-log LBAs, the raw-image size, and the EFI partition offset remain unchanged.
This change requires a rebuilt BIOS loader with its matching payload layout.
The normal preserving-update tool updates the complete boot region; historical UEFI-only replacement keeps its old BIOS payload.


## Next storage packaging task

Keep PR #33 focused on keyboard startup and its required build correction.
The next task will retain only bootstrap and recovery programs in initrd.
Deploy verification programs, object providers, demonstrations, and optional utilities to managed ext2 storage.
Define the minimal program set, command lookup, provider paths, and test-image deployment before changing packaging.
Verify that updates preserve user files and that diagnostics still boot when `/disk` is unavailable.


## Resource comparison

Baseline `d8b9c46` and the corrected build use GCC 11.2.0, Debug configuration, kernel `-Os`, and allocator checks.
Normal boot measurements disable debugcon and diagnostics.
The macOS ARM64 host runs QEMU 11.1.3 with TCG, q35, four CPUs, and 128 MiB RAM.
UEFI uses Homebrew `edk2-x86_64-code.fd` and fresh `edk2-i386-vars.fd` copies.
Each run includes the ten-second boot menu and checks USB input at the shell.

| Property | Baseline | Candidate | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| Text and read-only data | 339,968 B | 339,968 B | 0 B | 0% |
| Data | 32,768 B | 32,768 B | 0 B | 0% |
| BSS | 180,224 B | 180,224 B | 0 B | 0% |
| Stripped kernel | 372,992 B | 372,992 B | 0 B | 0% |
| USB boot image | 35,651,584 B | 35,651,584 B | 0 B | 0% |
| Median UEFI boot, three runs | 12.234 s | 12.254 s | +0.020 s | +0.163% |
| BIOS staging capacity | 373,248 B | 377,344 B | +4,096 B | +1.10% |

The control state adds 40 bytes per device, within existing BSS page padding.
No new heap or DMA allocation is required.
Candidate stripped-kernel SHA-256: `02e4672e2ee3b84c3610f3d6f5ff0be57ccaf1ded0ded526b71dabe64d1194bb`.
Measurements cover QEMU behavior; physical control-transfer timing and keyboard recovery remain unmeasured.

Baseline boot times: 12.255, 12.211, and 12.234 seconds.
Candidate boot times: 12.254, 12.286, 12.244 seconds.
Logs are under `build/control-evidence/`. Normal serial output remains 25,891 bytes per run.
