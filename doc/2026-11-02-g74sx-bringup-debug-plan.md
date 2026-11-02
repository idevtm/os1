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
- **No serial port**: I/O ports `0x3F8`–`0x3FF` read `0xFF`, so the COM1 line
  status register looks like "data ready" forever.
- Internal keyboard is PS/2 (i8042 via EC).
- USB 3.0 ports on a Renesas/NEC xHCI controller (bound by the os1 xHCI driver).
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
| 5 | `2843375` late UEFI attach + shim log | Both `os1` and `os1 (no xHCI, one CPU)` hung (no instant reset) with `root alloc 0x…` as the last screen line, then reset after a while. Journal: firmware `type=0`; saved logs again only 54 bytes. Root cause found, see round-5 findings. |
| 6 | UART detection + cache-safe log | Pending. |

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

## Remaining hypotheses (if round 6 still fails)

1. **Timer source** on the UEFI path: HPET is used for LAPIC calibration there
   (it is disabled on the BIOS path after an earlier hang on this machine).
   Check the `timer: LAPIC source … count=` line; ~100000 is expected for a
   100 MHz bus at 1 kHz.
2. **xHCI probe** on the Renesas controller: compare `os1` with `os1 (no xHCI)`;
   every probe step is logged.
3. **AP startup**: compare with `os1 (no xHCI, one CPU)`.
4. **Keyboard**: PS/2 IRQ1 routing via IOAPIC; the first 32 scancodes are logged.

## Plan

1. Round 6: write the stick, boot `os1`; if the shell appears, type a few keys
   and wait ~3 minutes for the post-init watchdog reset so the log is saved.
   If anything fails, boot `os1 (no xHCI, one CPU)` next. Read back the stick.
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

## Open follow-ups

- Regression coverage for machines without a UART: the smoke runner depends on
  serial markers, so a `-serial none` variant needs another channel (e.g.
  QEMU `isa-debugcon` on port `0xE9`, or a screendump check).
- The pre-loader and both reset paths rely on `wbinvd`; consider flushing the
  window header in the shim/pre-loader paths with `clflush` as well.
- Merge plan for `main`: which pieces are permanent (EFI pre-loader, TSC clock,
  IST, BAR sizing, PIC masking, non-fatal probes) versus bring-up only
  (watchdog post-init reset, scancode logging, on-screen mirror).
- xHCI BIOS→OS ownership handoff (USBLEGSUP) is still not implemented.
- x2APIC mode is not detected; LAPIC is assumed to be in xAPIC MMIO mode.
