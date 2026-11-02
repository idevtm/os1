# Real-Hardware Boot Log Capture (USB stick)

Status: diagnostic tooling on branch `g74sx-bootlog`, built for the first
bring-up attempts on an ASUS G74SX (Sandy Bridge, HM65 PCH, UEFI-only firmware
without CSM). Logs are captured on both the legacy BIOS and the UEFI path.

## Hybrid USB image

`build/artifacts/os1-usb.img` (target `os1_usb_image`, built by
`cmake/scripts/create_usb_image.py`) boots on both firmware types:

| LBA | Content |
| --- | --- |
| 0 | MBR: BIOS boot code, entry 1 = active type `0xDA` legacy area, entry 2 = type `0xEF` ESP |
| 1–2879 | BIOS raw image (loader, kernel, initrd, boot log area) |
| 4096–69631 | FAT16 ESP: `EFI/BOOT/BOOTX64.EFI` (os1 pre-loader), `EFI/OS1/LIMINE.EFI`, `limine.conf`, `kernel_limine.elf`, `kernel.elf`, `initrd.cpio`, and `os1logs/` once logs exist |

The active partition entry also lives in `os1.raw` because some AMI BIOSes only
offer a USB stick for legacy boot when it has one. On UEFI boots the kernel
mirrors its debug stream to the framebuffer terminal (replaying output from
before the terminal existed) until `/bin/init` starts, since laptops have no
serial port.

## Why

Real laptops have no serial port, and the BIOS path cannot write the USB stick
after leaving real mode (no USB mass-storage driver). Photos of the screen lose
everything that scrolled away. This feature keeps the full debug stream in RAM
and has the next boot's loader, while the BIOS still works, save it on the stick.

## UEFI flow

1. **Pre-loader** (`src/boot/efi/preloader.cpp`, `\EFI\BOOT\BOOTX64.EFI`),
   every boot, while boot services still work:
   - numbers the boot from the line count of `\os1logs\journal.txt`;
   - checks the candidate windows `0x04000000`, `0x05000000`, `0x06000000`,
     `0x07000000` (1 MiB each, `kBootLogUefiCandidates`) for a valid header and
     saves the log as plain text in `\os1logs\boot-NNNN.txt`;
   - reserves the first free candidate as `EfiReservedMemoryType` and writes a
     fresh header, which keeps Limine and the kernel allocator away from it;
   - appends a journal line with the first header word of every candidate
     (shows whether RAM survived the reset), the saved log, and the armed window;
   - after saving a log, waits for a key; then chain-loads
     `\EFI\OS1\LIMINE.EFI` with a device path on the same partition.
   It is built position-independent, linked with `--emit-relocs`, and wrapped
   into PE32+ by `cmake/scripts/elf_to_pe.py` (the cross binutils lack PE).
2. **Limine shim** — finds the reserved window in Limine's memory map, appends
   its serial messages to it, and identity-maps up to the last candidate. The
   identity map is required: Limine's HHDM does not map reserved memory, and
   reading the window through it triple-faulted.
3. **Kernel** — `bootlog_attach` accepts a candidate only inside a `Reserved`
   BootInfo region with a valid header, then behaves as on the BIOS path
   (failure resets, TCO watchdog, flags).

## BIOS flow

1. **Loader (real mode, every boot)** — after E820, `bootlog_prepare16`:
   - reads the journal sector, increments the boot counter;
   - checks the RAM window at `0x04000000` (must lie in a usable E820 entry);
   - if a valid `OS1BLOG1` header is present, copies header + log into the next
     disk slot, records the result, prints the slot, and **waits for a key** so
     a crash→reset→crash cycle cannot loop unattended;
   - writes a fresh header (clearing the old magic) and publishes the window as
     boot module #1 named `bootlog`.
   Disk errors are reported and never stop the boot.
2. **Loader (long mode)** — `print64` messages are appended to the RAM log.
3. **Kernel** — `Debug::write` mirrors every byte into the log
   (`src/kernel/debug/bootlog.cpp`); output before `bootlog_attach` is buffered.
4. **Failure paths** reset without clearing RAM:
   - `kernel_main` returning → `bootlog_reset_machine` (and the BIOS loader's
     `bootlog_reset64` as a fallback);
   - `halt_forever` (kernel exceptions, allocator fatal paths) →
     `bootlog_reset_machine`;
   - triple faults reset by themselves;
   - hangs (even with interrupts off) → Intel TCO watchdog
     (`src/kernel/drivers/watchdog/intel_tco.cpp`). It is armed before ACPI
     discovery with a 45 s stage (reset after ~90 s without a kick) and kicked
     after platform discovery, device probing, and before the first thread.
     When `/bin/init` starts, the stage becomes 90 s, so even a successful boot
     resets after ~3 minutes and its log is saved.

Reset order: 5 s pause, keyboard controller `0xFE`, `0xCF9` system reset,
triple fault. Whether the firmware preserves RAM across these resets is exactly
what the journal's "RAM seen at last boot" fields reveal.

## Layout

Disk (all below LBA 2880, reachable even under USB floppy emulation):

| LBA | Content |
| --- | --- |
| 0–1216 | boot sector, loader, kernel, initrd (unchanged) |
| 1280 | journal sector `OS1BJRN1` |
| 1288 + 392·n, n = 0..3 | log slot n (header sector + ring data) |

RAM header (64 bytes, `BootLogHeader`): magic, version, header size, capacity,
progress flags, 64-bit write count (byte N lives at `data[N % capacity]`), boot
sequence, saved sequence. Flags: kernel-attached, user-init-started,
fatal-stop, watchdog-armed, kernel_main-returned.

Journal: boot counter, last saved sequence, next slot, last result, BIOS drive
and CHS geometry, the raw RAM header words seen at the last boot, and a 32-entry
boot history. Result codes: 0 no RAM log, 1 saved, 2 bad header, 3 disk write
failed, 4 RAM window not usable.

## Operator procedure

Write the stick (macOS, replace `diskN`; this erases it):

```sh
cmake --build build
diskutil unmountDisk /dev/diskN
sudo dd if=build/artifacts/os1-usb.img of=/dev/rdiskN bs=1m
sync
```

On the laptop: boot the stick (UEFI entry in the firmware boot menu). If the
screen shows `previous boot log saved`, either press a key to try again or
power off. Allow ~3 minutes for a hang to turn into a watchdog reset.

Read the logs back: UEFI logs are plain files in `os1logs/` on the stick's
`OS1 EFI` volume. Legacy BIOS logs live in raw sectors:

```sh
sudo python3 -I tools/bootlog_extract.py /dev/rdiskN --all --out-dir bootlogs/
```

## Other bring-up changes on this branch

- ACPICA OSL: `AcpiOsStall`/`AcpiOsSleep` now delay and `AcpiOsGetTimer` runs,
  using a TSC clock calibrated on PIT channel 2
  (`src/kernel/drivers/timer/tsc_clock.cpp`). Before, the ACPI-mode switch had
  microseconds instead of 3 s, and AML `While` loops could never time out.
- A failing PCI driver probe no longer aborts bring-up; the device is marked
  `Failed` and probing continues.
- `Interrupts::initialize` keeps all 8259 lines masked.
- The first 32 PS/2 scancodes are logged.
- `rdtsc()` returns the full 64-bit counter.

## Caveats

- Interactive QEMU sessions of the BIOS image on q35 also arm the TCO watchdog
  and reset ~3 minutes after reaching the shell (`run_bios` exits because of
  `-no-reboot`). Smoke tests finish long before that.
- Each boot writes the journal sector into the raw image (also under QEMU).
- The UEFI ISO used by the smoke tests has no pre-loader, so log capture and the
  TCO watchdog stay off there.
