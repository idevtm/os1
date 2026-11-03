# ASUS G74SX Bring-Up: Debugging Plan and Handoff

Status: **the user reports round 8 works on the physical ASUS G74SX**.
Round 7's freshly rebuilt filesystem was not offered by the firmware;
restoring the exact round-6 stick recovered boot. Round 8 changed only the
kernel file contents in that known-working image. Use the constrained
`os1_usb_kernel_update` target below for this laptop. The exact round-7 failure
mechanism remains unproven; do not reuse its repacking path for the G74SX. The round-8 report
does not establish a ten-minute run or specific shell-command results.
The investigation used `g74sx-bootlog`; the `codex/g74sx-bringup` follow-up was
merged as [PR #26](https://github.com/idevtm/os1/pull/26), commit `85ebb10` on
`main`. Those names describe historical branches, not required local checkouts.
Owner: whoever picks up the
next stick round. Companion doc for the capture mechanism:
[2026-11-02-real-hardware-bootlog.md](2026-11-02-real-hardware-bootlog.md).

**2026-11-03 status update:** the [project review](2026-11-03-review.md) covers
the merged runtime and its remaining source findings. The final early-buffer
fix below postdates the physical round-8 report. No subsequent physical boot,
ten-minute session, or detailed shell-command results are claimed here.

## Goal

A successful os1 boot (shell prompt, working keyboard) on an ASUS G74SX laptop.
Evidence comes only from boot attempts the human runs with a USB stick, so every
round must be designed to return as much information as possible.

## Target machine (known facts)

- ASUS G74SX: Intel Sandy Bridge (4C/8T), HM65 PCH, NVIDIA GTX 560M, 2011 AMI
  Aptio firmware.
- **UEFI-only boot, no CSM.** The legacy BIOS raw image is never offered.
- **No serial port**: I/O ports `0x3F8`–`0x3FF` read `0xFF`, so the COM1 line
  status register looks like "data ready" forever.
- Internal keyboard is PS/2 (i8042 via EC).
- USB 3.0 ports on a **Fresco Logic FL1000** xHCI controller (`1b73:1000`, PCI
  04:00.0, BAR0 `0xf4c00000`+64 KiB, 2 ports, 32 slots, no scratchpads); binds
  and starts fine with MSI. (Earlier notes guessed Renesas/NEC.)
- Round-6 measurements: TSC 2195 MHz; LAPIC timer count 99773 for 1 kHz
  (100 MHz bus); HPET at `0xfed00000`, 8 timers, period 69841279 fs
  (14.318 MHz); MADT: 8 CPUs (APIC IDs 0–7, BSP 0), 1 IOAPIC (id 2), 2 ISOs;
  ACPICA: 102 devices, 63 PCI routes; 18 PCI functions (GTX 560M `10de:1251`,
  Atheros Wi-Fi `168c:002b`, Realtek NIC `10ec:8168`, HM65 AHCI `8086:1c03`).
- Firmware leaves the TCO watchdog halted with NO_REBOOT set (GCS `0xc21`);
  os1 clears NO_REBOOT (`0xc01`) and arms it successfully.
- On reset the firmware writes single `0x5A5A5A5A` dwords at 256 KiB-aligned
  physical addresses (4 per 1 MiB window; first seen as damaged headers at
  16 MiB-aligned windows in round 3). Everything else in the windows survived.
- The firmware reports the pre-loader's `EfiReservedMemoryType` allocation
  correctly (`type=0` in the round-5 journal).
- The CPU caches are not written back on a platform reset (watchdog or reset
  button): log data still in cache is lost (see round-5 findings).
- An older, unapplied investigation stash exists: `git stash list` →
  `stash@{0}: On real-device: ASUS G74SX investigation` (PIC masking, AP startup
  skipped on BIOS, xHCI handoff skipped, 64-bit BAR sizing fix, user DS/ES
  reload). Treat it as hints, not as a verified fix list.

## Round-trip workflow (human in the loop)

1. Build from the verified, pinned round-6 capture using the optional kernel-update preset:

   ```sh
   cmake --preset usb-kernel-update -DOS1_USB_BASE_IMAGE=/absolute/path/to/verified-round6-stick.img
   cmake --build --preset usb-kernel-update
   ```

   Output: `build/usb-kernel-update/artifacts/os1-usb-kernel-update.img`. The expected base is the
   40 MiB capture with SHA-256
   `5a0643210bf7fdb2f5a95c741329dfd9d88f5c589e3473df1e124d28d17298ae`.
   `os1_usb_kernel_update` requires `OS1_USB_BASE_IMAGE`; a missing or wrong
   base fails instead of falling back to a fresh filesystem. The preset is a
   shortcut for `cmake --build build/usb-kernel-update --target os1_usb_kernel_update`.
2. The target replaces only `kernel.elf` contents with the stripped new
   kernel, padded to the original 650,952-byte file size. All filesystem
   metadata, cluster locations/slack, EFI binaries, shim, menu, initrd and
   BIOS area stay unchanged. A kernel exceeding this capacity, or a change
   requiring any of those other components, needs separate physical
   qualification; do not resize or reformat implicitly.
3. Preserve the current stick before writing, re-identify the Kingston
   DataTraveler 3.0, and verify the candidate and written bytes. Disk numbers
   can change. Use the current kernel-update output, not an old round-7 or
   scratchpad write script. The user boots the UEFI USB entry and selects the
   original `os1` menu entry. The new kernel's capture and watchdog remain off
   because the preserved menu has no `os1.bootlog` or `os1.bootdiag` option.
4. Read back the identified device's first 40 MiB to a fresh capture when
   collecting evidence. Then without sudo:
   `hdiutil attach -readonly -nomount stick.img`, `mount -t msdos -o rdonly
   /dev/diskNs2 <dir>`, read `os1logs/journal.txt` and `os1logs/boot-NNNN.txt`.
   Detach afterwards. The retained pre-loader may update its journal, but the
   current G74SX menu does not enable new kernel-log capture.

The separate `os1_usb_image` target builds a new hybrid filesystem. Its
ordinary and diagnostic variants are not G74SX-qualified. The failed round-7
fresh-repack path is excluded from the G74SX workflow.

Writing the stick requires administrator access and the human's go-ahead.

## Capture mechanism (summary)

- `\EFI\BOOT\BOOTX64.EFI` = os1 pre-loader (`src/boot/efi/preloader.cpp`): saves
  the previous RAM log to `\os1logs\boot-NNNN.txt`, appends one journal line per
  boot, reserves a 1 MiB window (`kBootLogUefiCandidates`), pauses after a save,
  chain-loads `\EFI\OS1\LIMINE.EFI`.
- With kernel capture enabled, the shim log is copied into the window and
  the kernel appends to it; fatal paths show a `FATAL` banner and wait for a
  key. Full diagnostics also arm the TCO watchdog. This describes the capture
  mechanism used in earlier rounds and generic diagnostics; the current
  kernel-update target preserves the old menu and leaves kernel capture disabled.
- Journal line fields: `wN=<first qword of window N>[5a:<fill dwords>@<first
  offset>]`, `(boot#… bytes=… flags:…)` for a found log, `saved`/`SAVE-FAILED`,
  `armed=<window>`. Flags: kernel-attached, user-init-started, fatal-stop,
  watchdog-armed, kernel_main-returned.

## History of rounds

| Round | Image | Result |
| --- | --- | --- |
| 1 | `5e4b338` legacy raw image | Not offered by firmware (UEFI-only). |
| 2 | `19e644c` hybrid, no UEFI capture | Boots via UEFI, reset loop (~10–15 s per cycle incl. POST); last visible line "about interrupts" per the human. |
| 3 | `5f95037` UEFI capture | Loop continued; journal: 3 boots, all `no previous log`; window headers hit by `0x5A5A5A5A` (`w0=0x31474f4c5a5a5a5a`). |
| 4 | `8463110` windows moved, FATAL stop, IST, menu | All three menu entries: kernel log scrolled, then reset (no FATAL banner). Journal: 4 boots, each saved log only the pre-loader's 54-byte banner. See round-4 findings. |
| 5 | `2843375` late UEFI attach + shim log | Both `os1` and `os1 (no xHCI, one CPU)` hung (no instant reset) with `root alloc 0x…` as the last screen line, then reset after a while. Journal: firmware `type=0`; saved logs again only 54 bytes. Root cause found, see round-5 findings. |
| 6 | `0e09566` UART detection + cache-safe log | **Success.** `os1` boots to the shell; keyboard works; the post-init watchdog reset saved a complete 42 KB log (`boot-0001.txt`, flags: kernel-attached, user-init-started, watchdog-armed). |
| 7 | Uncommitted follow-up candidate, SHA-256 `32abefcc32b0aaf8423f7f8768dfdb3f09640b244158fcea1fdafe0b378a9976` | Missing from ASUS firmware boot menu on the same USB port. The write verified; the subsequent raw backup exactly matches the candidate. Restoring the exact round-6 capture reappeared as UEFI USB and booted. |
| 8 | Kernel-only isolation, SHA-256 `59e37f1951bbb63281f5261e0fa57b4629f3a638c62e356ef9ea5814efadddde` | **The user reports it works on the physical laptop.** Round-6 stick bytes with only kernel.elf contents replaced by the round-7 loadable kernel. No duration or detailed command results were supplied. |

## Round-7 firmware discovery regression

- Failed-stick readback is preserved in
  `local-evidence/g74sx/round7-restore-round6/before-write-20261102T215245Z/`.
  Its entire 34 MiB written range exactly matches the round-7 candidate, and
  the following 6 MiB is unchanged from round 6. This excludes a partial or
  corrupt write as the explanation for this attempt.
- Full MBR, FAT16 boot sector/BPB, EFI pre-loader, Limine and kernel shim are
  byte-identical between rounds 6 and 7. Both filesystems pass read-only
  `fsck_msdos` and independent FAT/directory/chain validation.
- The new kernel grew from 650,952 to 651,472 bytes, requiring one extra
  2,048-byte cluster. The builder consequently relocated initrd and every EFI
  directory/file by one cluster. All pointers are consistent. Menu, kernel,
  initrd archive timestamps, and BIOS payloads also changed. The exact cause
  of the firmware-menu disappearance is still unknown; relocation is a
  hypothesis, not a demonstrated ASUS firmware defect.
- Round 8 preserves the complete round-6 filesystem metadata, file locations,
  EFI loaders, menu, initrd, BIOS area and cluster slack. Only kernel.elf file
  contents change: the stripped round-7 ELF is padded to the old file size.
  Its entry, program headers and all PT_LOAD bytes match the full round-7
  kernel exactly, and the UEFI shim ignores symbol tables and trailing data.
  The unchanged menu selects normal mode, so capture and watchdog are off.
  This tests the new kernel in the known-working disk layout; its success
  does not establish which other round-7 difference caused enumeration to fail.
- Independent round-8 review verified that every byte outside the original
  kernel file is unchanged, including its 312-byte final-cluster slack. QEMU
  booted this exact artifact with eight CPUs, all seven APs online, and PS/2
  `echo mode-ok`; no kernel capture, watchdog or scancode diagnostics appeared.
  The pristine image hash stayed unchanged. Evidence is under
  `local-evidence/g74sx/round8-kernel-only/qemu-validation/`. The user then
  reported round 8 works physically. Keep that report separate from the
  detailed QEMU checks; QEMU success did not predict the round-7 ASUS result.
- **Excluded G74SX path:** do not freshly repack the FAT16/hybrid image with
  the growing unstripped kernel as round 7 did. Keep round 7 under
  `local-evidence/g74sx/round7-candidate/` as failed evidence, not as an image
  to flash again for routine use. The `os1_usb_kernel_update` target implements round 8's
  one-file-content replacement and refuses to silently fall back to repacking.

## Round-4 findings

- The window survives a reset: headers intact, only 4 fill dwords per 1 MiB
  window (`[5a:4@…]`), at 256 KiB-aligned physical addresses (e.g. `0x4A40000`,
  `0x5B80000`). The pause-after-save works.
- Each saved log held only the pre-loader's 54-byte banner. At the time this was
  read as "the window is not reported as reserved"; round 5 disproved that
  (`type=0`). The round-5 cache explanation below fits rounds 4 and 5 better.
- Changes made for round 5 regardless (still useful): the kernel attaches after
  its direct map exists, maps the candidate itself, logs the region type, and
  reserves the window if it was reported usable; the shim buffers its serial
  output and hands it over as the `shimlog` boot module; the pre-loader journals
  the firmware's type for the armed window.

## Round-5 findings (root cause of the hang)

- **Hang:** `console_input_poll_serial()` runs on every scheduler tick on the
  BSP and drained COM1 with `while(LSR & DATA_READY)`. With no UART the port
  reads `0xFF`, so the loop never ended, with interrupts off and the console
  lock held. The first tick fires a few milliseconds after the timer starts,
  i.e. while the boot thread prints `root alloc` (that line was even cut off in
  the QEMU reproduction). Independent of xHCI and SMP, matching both variants.
  **Reproduced in QEMU with `-serial none`** (q35 + OVMF + the USB image).
- **Empty logs:** the kernel did attach and arm the TCO watchdog (the machine
  reset itself after the hang, which nothing else in os1 could do). But every
  byte written updates the header's write count, so that cache line never left
  the CPU cache; the platform reset dropped it, and DRAM still held the
  pre-loader's header (54 bytes, no flags). QEMU has no cache, so every QEMU
  test passed.
- **Fixes (round 6):** `Debug::init_serial` probes the UART scratch register and
  marks serial unavailable when it does not answer; serial output and the tick
  poll are skipped then, and the poll drains at most 16 bytes per tick. The boot
  log `clflush`es each finished line plus the header (`mfence` after), every
  flag update flushes, and every intentional reset path (kernel, BIOS loader,
  EFI pre-loader after arming) runs `wbinvd` first.
- **Verified in QEMU without a serial port:** boots to the shell, PS/2 keyboard
  input works (`echo hi`), and after a reset the saved log holds the whole boot
  including the shell session.
- Rounds 2–4 reset loops are not fully explained. Candidates: triple faults
  removed by the round-4 IST and xHCI changes, or watchdog resets after this
  same hang once a log was attached. Confirm with round-6 logs.

## Round-6 findings

- Full bring-up with no faults: ACPICA init, HPET, LAPIC timer, xHCI probe, all
  7 APs started, `/bin/sh` reached. The human ran `help`, `pid`, `sys`, `ps`,
  `cpu`, `pci`, `initrd`, `devices`, `irqs`, `resources`, `kmem`, `events`,
  `exec`; scancodes and command output are in the log.
- Anomalies seen only on this machine (not blocking):
  - **I/O BAR sizes** print as `+0xffff00NN` (e.g. AHCI `bar0:io@0xe070+0xffff0008`):
    the upper 16 bits of an I/O BAR read back 0 here, and the sizing treats them
    as address bits. Same class of bug as the 64-bit memory BAR fix; no driver
    claims I/O BARs yet.
  - **Memory is clipped at 16 GiB**: `pages=4051062/4194304`, exactly the
    page-frame bitmap limit (`kPageFrameBitmapSizeBytes` 512 KiB); RAM remapped
    above 16 GiB is ignored.
- Seen in QEMU too (existing behavior): `ps` shows the kernel threads of pid 1
  as `dying running`.
- **Handoff correction:** the preserved round-6 log contains the xHCI device,
  all 102 ACPI devices, and `devices complete`. The previous header-only and
  undersized-observe-buffer claim was incorrect for this run. No `devices`
  code change is needed.

## Remaining hypotheses (kept for reference; resolved by round 6)

1. **Timer source** on the UEFI path: HPET is used for LAPIC calibration there
   (it is disabled on the BIOS path after an earlier hang on this machine).
   Check the `timer: LAPIC source … count=` line; ~100000 is expected for a
   100 MHz bus at 1 kHz.
2. **xHCI probe** on the Fresco Logic controller: compare `os1` with `os1 (no xHCI)`;
   every probe step is logged.
3. **AP startup**: compare with `os1 (no xHCI, one CPU)`.
4. **Keyboard**: PS/2 IRQ1 routing via IOAPIC; the first 32 scancodes are logged.

## Follow-up implementation

- Round-6 evidence is preserved under `local-evidence/g74sx/round6-0e09566/`
  (ignored by Git): the 40 MiB stick capture, extracted ESP and journal/log,
  matching binaries, source archive, original handoff documents, and SHA-256
  manifest. Kernel, shim, initrd, and pre-loader bytes match the captured ESP.
- I/O BAR sizing now handles hardwired-zero upper 16 bits. Regression tests
  reproduce the G74SX masks and retain full-width and empty-mask cases.
- `smoke_no_uart` removes COM1, observes output through test-only QEMU debugcon,
  and types `echo no-uart` through PS/2. CI enables `OS1_QEMU_DEBUGCON=ON`;
  ordinary hardware builds keep it off. This is a UEFI/QEMU regression; it
  does not establish no-UART BIOS or arbitrary physical-console coverage.
- `OS1_BOOT_DIAGNOSTICS=OFF` is the normal build default. Diagnostic builds
  for fresh images opt into the EFI pre-loader and BIOS capture. Their USB menu has ordinary,
  capture-only (`os1.bootlog`), and full diagnostic (`os1.bootdiag`) entries.
  Only full diagnostics enable noisy mirroring/scancodes and the TCO watchdog.
  See the companion capture document for the full mode contract.
- The optional `usb-kernel-update` preset selects `os1_usb_kernel_update`,
  turns QEMU debugcon and boot diagnostics off, and requires the verified
  round-6 base image via `OS1_USB_BASE_IMAGE`. The target
  preserves the original pre-loader and menu rather than installing generic
  diagnostic components. That menu leaves kernel capture and watchdog off
  regardless of the build's diagnostic setting.
- The 16 GiB bitmap ceiling remains unchanged; expanding it is separate
  allocator work.

Validation completed locally: all 158 host tests and all 11 existing UEFI/BIOS
smokes passed. The no-UART test reached the shell and ran the PS/2 command;
an isolated kernel with the old unbounded serial poll timed out without either
marker, confirming regression sensitivity. UEFI ordinary/capture/diagnostic
and BIOS normal/diagnostic boots all reached the shell and accepted PS/2 input
with the expected capture/watchdog/scancode gates. Capture-only warm-reset
harvest saved a 4,586-byte log containing the shell command and no watchdog
flag. Evidence is in `local-evidence/g74sx/followup-validation/` and
`local-evidence/g74sx/mode-validation/`. Independent diff review passed.
The screen-mirror gate was checked in source, and watchdog programming by log
markers; these checks do not prove a ten-minute physical run or watchdog expiry.

Initial PR verification added 13 image-updater contract cases and confirmed
the former `g74sx` preset reproduced the exact successful round-8 image from the same kernel
inputs. Missing templates and incompatible build options fail without an
image. The final code passes all 158 host tests and all 12 QEMU smokes
(including no-UART). A review fix also stops the early log replay buffer before
IRQ/AP startup in every mode; its updated kernel passes eight-CPU capture-only
and kernel-update QEMU checks. That final buffer fix postdates the user's
physical round-8 report, so no additional physical result is claimed.

## Next steps

1. Use the dedicated `os1_usb_kernel_update` target for subsequent kernel updates and keep
   the round-6 base immutable. Record each candidate's source and checksum.
   Do not install a new shim, EFI loader, menu, initrd or BIOS payload through
   this target; qualify such changes separately on the laptop.
2. If sustained-operation evidence is needed, use the original `os1` entry
   and record a ten-minute responsive session plus `sys`, `cpu`, `pci`,
   `devices`, `irqs`, `resources`, `kmem`, `smpcheck`, and `balancecheck`
   results. Confirm eight CPUs and sensible AHCI I/O BAR sizes (8, 4, 8, 4,
   32 bytes). These detailed physical checks have not been established by
   the user's round-8 success report. Capture screen evidence because this
   preserved menu does not enable kernel RAM logging.
3. Address the [2026-11-03 review findings](2026-11-03-review.md#4-priority-findings)
   before expanding the affected runtime capabilities. The bring-up PR is
   already merged; preserve the failed round-7 evidence and the limits of
   round-8 success when qualifying subsequent kernels or packaging changes.

## Plan used during rounds 1–6
2. Map the faulting RIP to a symbol: `x86_64-elf-addr2line -f -C -e
   build/artifacts/kernel.elf <rip>` (the build must match the stick's commit;
   rebuild from that commit if needed). For shim addresses use
   `kernel_limine.elf`.
3. Fix the narrowest cause, add host-test or QEMU evidence where possible, keep
   diagnostics in place, and prepare the next round with targeted menu entries.
4. Once the shell is reached: test the internal keyboard (PS/2 scancodes are
   logged), then decide which diagnostics to keep (on-screen mirror, watchdog
   post-init reset) before merging anything to `main`.

## Decision log

- Logs survive via RAM + next-boot harvest because the kernel has no USB
  storage driver and firmware services are gone after ExitBootServices.
- Windows avoid 16 MiB-aligned addresses after the round-3 fill observation.
- Fatal paths wait for a key instead of resetting at once: in round 2/3 the
  reset destroyed the only on-screen evidence.
- Boot options (`os1.noxhci`, `os1.nosmp`) live in a USB-only Limine config so
  the smoke-test ISO keeps a zero-timeout single entry.
- The boot log is flushed per line (`clflush`) instead of being mapped
  uncached: the kernel direct map already maps the window write-back, and
  mixing memory types for one physical page is undefined on x86.
- Serial presence uses the 16550 scratch register (write `0xA5`/`0x5A`, read
  back) rather than "LSR != 0xFF", so a partially decoded port cannot pass.
- G74SX images preserve the pinned round-6 boot/filesystem layout and replace
  only stripped, padded kernel contents. Round 8 worked physically; round 7's
  fresh repack did not. This is an evidence-based packaging constraint, not
  proof of a particular firmware defect.

## Open follow-ups

- The opt-in SMP boot log still has a flush-cursor/store-completion race that
  may lose late bytes across hardware reset; see the capture document's
  caveats. Its early replay buffer is now stopped before interrupts/AP startup
  in every mode, avoiding concurrent writes into that bootstrap-only buffer.
- The pre-loader and both reset paths rely on `wbinvd`; consider flushing the
  window header in the shim/pre-loader paths with `clflush` as well.
- Complete sustained physical validation with the bounded command set above.
- `acpiprobe` is currently an active power diagnostic: its observation path
  attempts D3 then D0 on live devices without a driver-quiescence or recovery
  protocol. Keep this separate from passive hardware inspection; see
  [review finding R2](2026-11-03-review.md#r2--p1-acpi-observation-performs-ungated-power-transitions).
- xHCI BIOS→OS ownership handoff (USBLEGSUP) is still not implemented.
- x2APIC mode is not detected; LAPIC is assumed to be in xAPIC MMIO mode.

## Model-neutral packaging operation (2026-11-03)

The public operation is now `os1_usb_kernel_update`, taking
`OS1_USB_BASE_IMAGE` and producing `os1-usb-kernel-update.img`. The optional
`usb-kernel-update` preset selects that target. It is independent of fresh
`os1_usb_image` creation and is not included in the default build. The updater
and contract test are named `update_usb_kernel.py` and
`test_update_usb_kernel.py`. The same pinned base, slot capacity and ELF checks
remain; this does not add support for arbitrary templates or qualify new
machines. See the capture document for old-cache migration instructions.

This names the operation rather than assuming a model-specific defect. UEFI
specifies file-path discovery and permits FAT16 removable media; it does not
require preserving EFI file locations. Round 8 established a workaround, not
that the one-cluster relocation caused round 7's failure. Other payloads also
changed. OVMF boots both variants, and this exact failure has not been shown on
another firmware implementation. Keep the machine model in the evidence record.

References: [UEFI media formats](https://uefi.org/specs/UEFI/2.10/13_Protocols_Media_Access.html),
[UEFI boot-file convention](https://uefi.org/sites/default/files/resources/UEFI_Spec_Final_2.11.pdf).
To isolate the cause, vary EFI allocation and other changed payloads separately,
then compare the same images across physical and virtual firmware. Do not
replace the working update path with a newly generated layout before validation.

Validation of the target refactor: all 13 updater contract cases pass through
CTest. The preset and explicit target produce SHA-256
`3be357ed8b1af9cec461d64b701f2fe17b3f5a007f897fc83f56a99466f847a7`, identical
to the previously validated final profile image, with every byte outside the
kernel file unchanged. Missing-base and incompatible-option targets fail;
obsolete cache settings fail configuration with migration instructions. The
fresh-image target still builds independently, and the default build excludes
the kernel-update target. No new physical boot is claimed.
