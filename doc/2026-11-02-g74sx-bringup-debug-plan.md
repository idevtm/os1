# ASUS G74SX Bring-Up: Debugging Plan and Handoff

Status: active, branch `g74sx-bootlog` (local). Owner: whoever picks up the
next stick round. Companion doc for the capture mechanism:
[2026-11-02-real-hardware-bootlog.md](2026-11-02-real-hardware-bootlog.md).

## Goal

A successful os1 boot (shell prompt, working keyboard) on an ASUS G74SX laptop.
Evidence comes only from boot attempts the human runs with a USB stick, so every
round must be designed to return as much information as possible.

## Target machine (known facts)

- ASUS G74SX: Intel Sandy Bridge (4C/8T), HM65 PCH, NVIDIA GTX 560M, 2011 AMI
  Aptio firmware.
- **UEFI-only boot, no CSM.** The legacy BIOS raw image is never offered.
- No serial port. Internal keyboard is PS/2 (i8042 via EC).
- USB 3.0 ports on a Renesas/NEC xHCI controller (bound by the os1 xHCI driver).
- On reset the firmware writes `0x5A5A5A5A` at least at 16 MiB-aligned
  addresses (observed in the first dword of four windows); upper dwords there
  survived. Full extent of the fill is unknown — the journal now measures it.
- An older, unapplied investigation stash exists: `git stash list` →
  `stash@{0}: On real-device: ASUS G74SX investigation` (PIC masking, AP startup
  skipped on BIOS, xHCI handoff skipped, 64-bit BAR sizing fix, user DS/ES
  reload). Treat it as hints, not as a verified fix list.

## Round-trip workflow (human in the loop)

1. Build: `cmake --build build` → `build/artifacts/os1-usb.img` (hybrid MBR:
   legacy area + FAT16 ESP with the os1 EFI pre-loader, Limine, kernel).
2. Write the stick (erases it; needs sudo; checks the device is the Kingston
   DataTraveler 3.0 on `disk13`): scratchpad `write-stick.sh`, equivalent to
   `sudo dd if=build/artifacts/os1-usb.img of=/dev/rdisk13 bs=1m`.
3. Human boots the laptop (UEFI boot menu entry for the stick), picks a Limine
   menu entry, photographs any `FATAL` screen, presses a key, repeats as asked.
4. Read back (needs sudo): scratchpad `read-stick.sh` copies the first 40 MB of
   `/dev/rdisk13` to `stick.img`. Then without sudo:
   `hdiutil attach -readonly -nomount stick.img`, `mount -t msdos -o rdonly
   /dev/diskNs2 <dir>`, read `os1logs/journal.txt` and `os1logs/boot-NNNN.txt`.
   Detach afterwards.

Agents cannot use sudo in this environment; ask the human to run the two
scripts. Never write the stick without the human's go-ahead.

## Capture mechanism (summary)

- `\EFI\BOOT\BOOTX64.EFI` = os1 pre-loader (`src/boot/efi/preloader.cpp`): saves
  the previous RAM log to `\os1logs\boot-NNNN.txt`, appends one journal line per
  boot, reserves a 1 MiB window (`kBootLogUefiCandidates`), pauses after a save,
  chain-loads `\EFI\OS1\LIMINE.EFI`.
- Limine shim and kernel append to the window; fatal paths show a `FATAL`
  banner, wait for a key (or the TCO watchdog, ~90 s), then reset.
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
| 5 | late UEFI attach + shim log | Pending. |

## Round-4 findings

- The window survives a reset: headers intact, only 4 fill dwords per 1 MiB
  window (`[5a:4@…]`), at 256 KiB-aligned physical addresses (e.g. `0x4A40000`,
  `0x5B80000`). The pause-after-save works.
- Neither the Limine shim nor the kernel attached: both required the window to
  be a `Reserved` region in Limine's memory map. On this firmware it was not,
  although the pre-loader allocated it as `EfiReservedMemoryType`. With the log
  inactive, the TCO watchdog was not armed and `halt_forever` would only halt,
  so the observed resets are most likely triple faults (or a firmware-side
  reset), in all three variants — xHCI and AP startup alone are not the cause.
- Fix (round 5): the kernel attaches after its direct map exists, maps the
  candidate itself, validates the header, logs the region type it saw, and
  reserves the window if it was reported usable. The shim buffers its serial
  output and hands it over as the `shimlog` boot module (copied first into the
  log). The pre-loader journals the firmware's own type for the armed window
  (`type=N`, 0 = reserved). In QEMU, an allocation reported as boot-services
  data (Limine type 9) still attaches.
- Remaining blind spot: a fault before the kernel's direct map exists (or before
  the IDT) still resets without a saved log. Ask the human to film the screen.

## Hypotheses for the reset loop (ranked)

1. **Fault after `Interrupts initialization successful`, in xHCI probe** — next
   step in `kernel_main` is `platform_probe_devices`; the Renesas controller may
   have memory decoding off (registers read all ones), a 64-bit BAR with
   hardwired upper bits (old sizing produced ~2^64), or MSI/MSI-X programming
   issues. Round 4 logs each probe step; the `os1 (no xHCI)` menu entry is the
   A/B test.
2. **Triple fault in an exception handler** (no IST before `8463110`), which
   resets without a visible fault message. Now NMI/#DF/#MC use IST1.
3. **AP startup** (INIT/SIPI, trampoline at 0x1000) — the old stash skipped APs
   on BIOS. A/B test: `os1 (no xHCI, one CPU)`.
4. **Spurious/unexpected interrupt right after `sti`** (firmware-left MSI on a
   device, LAPIC error/spurious vectors). Visible as an exception or unknown
   vector in the log.
5. **HPET/LAPIC timer calibration** on the UEFI path (HPET is disabled only on
   the BIOS path).

## Plan

1. Read round-5 logs: which menu entries ran, last lines before reset/FATAL,
   exception name and registers, `type=` and `memmap type` of the window.
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

## Open follow-ups

- Merge plan for `main`: which pieces are permanent (EFI pre-loader, TSC clock,
  IST, BAR sizing, PIC masking, non-fatal probes) versus bring-up only
  (watchdog post-init reset, scancode logging, on-screen mirror).
- xHCI BIOS→OS ownership handoff (USBLEGSUP) is still not implemented.
- x2APIC mode is not detected; LAPIC is assumed to be in xAPIC MMIO mode.
