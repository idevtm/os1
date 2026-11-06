# Real-Hardware Boot Log Capture (USB stick)

This guide preserves the earlier firmware experiments.
For current fresh images and preserving updates, use the [USB workflow](usb-image-workflow.md).
The [November 6 review](../reviews/2026-11-06-review.md) includes the later fresh-image and USB persistence evidence.

Status: the user reports round 8 works on the physical ASUS G74SX (Sandy
Bridge, HM65 PCH, UEFI-only firmware without CSM). Its image preserves the
round-6 boot/filesystem layout and replaces only kernel file contents. The
freshly repacked round-7 image was not offered by that laptop's firmware.
The kernel-update operation below preserves the working layout; fresh
images are not G74SX-qualified. Permanent UART and platform fixes remain
active in every build.

**2026-11-03 status update:** the bring-up changes are merged in `main` at
`85ebb10` ([PR #26](https://github.com/idevtm/os1/pull/26)). The final early-buffer
fix postdates the physical round-8 report; later QEMU checks do not qualify that
kernel on the laptop. The [historical project review](../archive/reviews/2026-11-03-review.md) records
the source findings and validation limits. The experiment history below remains
the evidence for the reported hardware result.

## Kernel-only USB image update

Use the pinned round-6 40 MiB capture, SHA-256
`5a0643210bf7fdb2f5a95c741329dfd9d88f5c589e3473df1e124d28d17298ae`, as the
explicit base:

```sh
cmake --preset usb-kernel-update -DOS1_USB_BASE_IMAGE=/absolute/path/to/verified-round6-stick.img
cmake --build --preset usb-kernel-update
```

The optional preset selects the `os1_usb_kernel_update` target with
`OS1_QEMU_DEBUGCON=OFF` and `OS1_BOOT_DIAGNOSTICS=OFF`. The output is
`build/usb-kernel-update/artifacts/os1-usb-kernel-update.img`.
The target also works without a preset:

```sh
cmake -S . -B build/usb-kernel-update -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-elf.cmake \
  -DOS1_USB_BASE_IMAGE=/absolute/path/to/verified-round6-stick.img \
  -DOS1_QEMU_DEBUGCON=OFF -DOS1_BOOT_DIAGNOSTICS=OFF
cmake --build build/usb-kernel-update --target os1_usb_kernel_update
```

A missing base or incompatible build option makes this target fail without
preventing other targets from building. An incorrect base is rejected by its
size and SHA-256. There is no fresh-image fallback. The separate
`os1_usb_image` target always builds `os1-usb.img` from current sources;
`os1_usb_kernel_update` is explicit and is not part of the default build.

The former `g74sx` preset and `OS1_USB_PROFILE` were removed. For an existing
build directory, clear the obsolete cache variables when reconfiguring:

```sh
cmake -S . -B <build-dir> -UOS1_USB_PROFILE -UOS1_G74SX_BASE_IMAGE \
  -DOS1_USB_BASE_IMAGE=/absolute/path/to/verified-round6-stick.img
```

Then explicitly build `os1_usb_kernel_update` and use its new output filename.
Old profile caches fail configuration rather than silently changing recipes.

`cmake/scripts/update_usb_kernel.py` replaces only the original
`kernel.elf` file contents with the stripped new kernel padded to 650,952
bytes. The FATs, directories, file locations, final-cluster slack, pre-loader,
Limine, shim, menu, initrd, and entire BIOS area remain unchanged. If the
stripped kernel no longer fits, or an update needs any of those other
components changed, it needs a separate physically qualified image. Do not
silently resize or reformat the template.

Choose the preserved menu's original `os1` entry. It has neither
`os1.bootlog` nor `os1.bootdiag`, so the new kernel leaves RAM capture,
diagnostic mirroring/scancodes, and watchdog off regardless of the build's
diagnostic setting. The preserved EFI pre-loader still runs, reserves its
RAM window, and updates its journal before displaying the menu; that is not
evidence of new kernel-log capture. This target does not install newly built
EFI binaries, shim, initrd, menus or BIOS code.

Round 7 increased the unstripped kernel size by one FAT cluster, and the fresh
builder relocated later EFI files and directories. Other payloads changed
too. Its verified stick was absent from the ASUS boot menu; restoring round 6
recovered boot, and the user reports the kernel-only round 8 works. The exact
failure mechanism remains unproven. The round-7 fresh-repack path is excluded
from routine G74SX use. See the
[handoff](../archive/boot/2026-11-02-g74sx-bringup-debug-plan.md) for the evidence and limits;
no ten-minute duration or detailed round-8 shell-command results were supplied.

## Selecting generic capture and diagnostics

`os1_usb_image` creates a fresh hybrid filesystem. The following options
apply to that packaging path; the kernel-update target retains the base image's
boot components. `OS1_BOOT_DIAGNOSTICS` defaults to `OFF`; ordinary generic images
boot Limine directly without persistent capture or diagnostic watchdog. A separate build with
`-DOS1_BOOT_DIAGNOSTICS=ON` enables the BIOS capture loader and packages the EFI
pre-loader plus the diagnostic USB menu. The UEFI ISO remains an ordinary boot
image. Keep the two build directories separate so the normal image is not
mistaken for an image that writes logs to its boot media.

On the diagnostic USB image, choose:

| Menu entry | Behavior |
| --- | --- |
| `os1` | Ordinary sustained kernel boot; no RAM capture or diagnostic watchdog. |
| `os1 (capture only, no watchdog)` | `os1.bootlog`: retain the debug stream in RAM, without screen/scancode diagnostics or an automatic reset. |
| `os1 (diagnostics, automatic log harvest)` | `os1.bootdiag`: capture plus the screen mirror, first 32 scancodes, and watchdog reset to harvest the log. |
| Diagnostic entries disabling xHCI or SMP | Same diagnostics, with `os1.noxhci` and optionally `os1.nosmp` to isolate bring-up failures. |

The pre-loader itself is selected when the diagnostic image is built, before
the menu is shown. It therefore still reserves a window and updates the journal
when the ordinary `os1` entry is chosen on that image. The default build has no
pre-loader and performs none of those writes. BIOS diagnostic builds publish
`os1.bootdiag` directly because the BIOS loader has no interactive menu.

Capture-only boots require a warm reset followed by another boot of the
diagnostic image to harvest the log; a power-off may lose the RAM contents.
Fatal stops show a banner and offer a key-triggered reset in either capture
mode. Only `os1.bootdiag` arms the automatic watchdog.

## Generic hybrid USB image

`build/artifacts/os1-usb.img` (target `os1_usb_image`, built by
`cmake/scripts/create_usb_image.py`) targets both BIOS and UEFI. This fresh
packaging path is not qualified for the G74SX:

| LBA | Content |
| --- | --- |
| 0 | MBR: BIOS boot code, entry 1 = active type `0xDA` legacy area, entry 2 = type `0xEF` ESP |
| 1–2879 | BIOS raw image (loader, kernel, initrd, boot log area) |
| 4096–69631 | FAT16 ESP: `EFI/BOOT/BOOTX64.EFI` (Limine normally, os1 pre-loader in diagnostic builds), `EFI/OS1/LIMINE.EFI` (diagnostic builds only), `limine.conf`, `kernel_limine.elf`, `kernel.elf`, `initrd.cpio`, and `os1logs/` once diagnostics save logs |

The active partition entry also lives in `os1.raw` because some AMI BIOSes only
offer a USB stick for legacy boot when it has one. With `os1.bootdiag`, the
kernel mirrors its debug stream to the framebuffer terminal (replaying output
from before the terminal existed) until `/bin/init` starts, since laptops have
no serial port. Normal and capture-only boots leave this mirror disabled.

## Why

Real laptops have no serial port, and the BIOS path cannot write the USB stick
after leaving real mode in those historical images (which had no USB mass-storage driver). Photos of the screen lose
everything that scrolled away. This feature keeps the full debug stream in RAM
and has the next boot's loader, while the BIOS still works, save it on the stick.

## UEFI flow

1. **Pre-loader** (`src/boot/efi/preloader.cpp`, `\EFI\BOOT\BOOTX64.EFI`),
   every boot of a generic diagnostic image or the preserved G74SX template,
   while boot services still work:
   - numbers the boot from the line count of `\os1logs\journal.txt`;
   - checks the candidate windows `0x04A35000`, `0x05B47000`, `0x06C59000`,
     `0x07D6B000` (1 MiB each, `kBootLogUefiCandidates`) for a valid header and
     saves the log as plain text in `\os1logs\boot-NNNN.txt`. A header whose
     magic lost one 32-bit half still counts;
   - reserves the first free candidate as `EfiReservedMemoryType` and writes a
     fresh header, which keeps Limine and the kernel allocator away from it;
   - appends a journal line with the first header word of every candidate and
     `[5a:count@offset]`, the number of `0x5A5A5A5A` fill dwords found in it
     (shows what the firmware overwrote), the saved log, the armed window, and
     `type=N`, the firmware's memory type for it after allocation;
   - after saving a log, waits for a key; then chain-loads
     `\EFI\OS1\LIMINE.EFI` with a device path on the same partition.
   It is built position-independent, linked with `--emit-relocs`, and wrapped
   into PE32+ by `cmake/scripts/elf_to_pe.py` (the cross binutils lack PE).
2. **Limine shim** — keeps a copy of its serial output and hands it to the
   kernel as the `shimlog` boot module. (An earlier version read the window
   through Limine's HHDM, which does not map reserved memory, and triple-faulted.)
3. **Kernel** — when `os1.bootlog` or `os1.bootdiag` is selected and its direct
   map exists, `attach_uefi_bootlog` maps each
   candidate itself, attaches to the first valid header, copies the shim log
   and the buffered early output in, reserves the window if the memory map
   called it usable, and logs the region type. It does not require a `Reserved`
   region, although the round-5 journal confirmed the G74SX firmware correctly
   reported `EfiReservedMemoryType` (`type=0`). The old missing-reservation
   theory was superseded by the cache-persistence finding. Completed log lines
   and flag updates flush the ring/header to RAM; reset paths use `wbinvd`.

### First G74SX result

The first UEFI attempt saved nothing: on reset the firmware wrote `0x5A5A5A5A`
over the first dword of every 16 MiB-aligned window (`w0=0x31474f4c5a5a5a5a`:
the upper half of the magic survived), so every header was invalid and the
pre-loader never paused. The windows now sit off those boundaries.

### Fatal stops and bring-up variants

- With the log active, `halt_forever` prints a `FATAL` banner on screen, waits
  for a polled i8042 key press, then resets so the log is saved. In diagnostic
  mode the TCO watchdog also resets if nobody presses a key: approximately
  90 s during bring-up or 180 s after init, measured from its last reload.
- NMI, double fault, and machine check run on a per-CPU IST1 stack, so a fault
  on a broken kernel stack reaches the handler instead of triple-faulting.
- The normal generic stick uses `src/boot/limine/limine-usb.conf`; generic diagnostic builds
  use `src/boot/limine/limine-usb-diagnostic.conf` (10 s timeout, ordinary
  `os1` default). The kernel-update target preserves the template's original config
  bytes instead. The kernel logs its command line.
- The xHCI probe logs each step, enables memory decoding before touching
  registers, and skips controllers whose registers read all ones or whose BAR is
  implausible. 64-bit BARs whose upper half reads back 0 are sized from the low
  half (previously a size near 2^64).

## Generic diagnostic BIOS flow

The kernel-update target retains its old BIOS area byte-for-byte and is qualified for
the laptop's UEFI path only; it does not install a new BIOS kernel or loader.
The following describes the separately built generic diagnostic BIOS image.

1. **Loader (real mode, diagnostic builds only)** — after E820, `bootlog_prepare16`:
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

Reset order: keyboard controller `0xFE`, `0xCF9` system reset, then triple fault.
The BIOS-loader fallback also pauses 5 s before the first reset attempt.
Whether the firmware preserves RAM across these resets is exactly
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

For the G74SX, build with the kernel-update target above and select the original
`os1` menu entry. Do not flash the failed round-7 candidate or substitute a
fresh generic image. Preserve the current stick, identify the target device,
verify the candidate checksum, and verify the written bytes before booting.

For a separately qualified generic diagnostic target, write its image
(macOS, replace `diskN`; this overwrites the stick's boot area):

```sh
cmake --preset default -DOS1_BOOT_DIAGNOSTICS=ON -DOS1_QEMU_DEBUGCON=OFF
cmake --build build --target os1_usb_image
diskutil unmountDisk /dev/diskN
sudo dd if=build/artifacts/os1-usb.img of=/dev/rdiskN bs=1m
sync
```

On the generic diagnostic target: boot the UEFI stick entry. If the
screen shows `previous boot log saved`, either press a key to try again or
power off. An automatic watchdog reset requires the full `os1.bootdiag`
entry; capture-only and the preserved G74SX menu do not arm it.

Read the logs back: UEFI logs are plain files in `os1logs/` on the stick's
`OS1 EFI` volume. Legacy BIOS logs live in raw sectors:

```sh
sudo python3 -I tools/bootlog_extract.py /dev/rdiskN --all --out-dir bootlogs/
```

## Bring-up changes merged in main

- ACPICA OSL: `AcpiOsStall`/`AcpiOsSleep` now delay and `AcpiOsGetTimer` runs,
  using a TSC clock calibrated on PIT channel 2
  (`src/kernel/drivers/timer/tsc_clock.cpp`). Before, the ACPI-mode switch had
  microseconds instead of 3 s, and AML `While` loops could never time out.
  If calibration fails, the timer advances by queries and delays are approximate;
  this is firmware-wait support, not a public clock/deadline contract.
- A failing PCI driver probe no longer aborts bring-up; the device is marked
  `Failed` and probing continues.
- `Interrupts::initialize` keeps all 8259 lines masked.
- With `os1.bootdiag`, the first 32 PS/2 scancodes are logged.
- `rdtsc()` returns the full 64-bit counter.

## Caveats

- Concurrent writers can advance the persistent flush cursor before an earlier
  reserved byte is stored. A hardware reset can therefore lose a late store
  even after another CPU flushed a completed line. The current opt-in capture
  is best-effort under SMP; QEMU does not model this cache-persistence race.
  Normal G74SX boots leave capture off. Closing this remaining durability gap
  requires a completion-safe flush policy and hardware validation.
- A successful generic QEMU boot does not qualify a fresh image for the G74SX:
  round 7 passed QEMU checks but was absent from the laptop's firmware menu.
- The kernel-update target deliberately keeps the original boot chain and userspace.
  It supports kernel-only updates within the original file size; bootloader,
  shim, initrd, menu, BIOS, and filesystem changes need separate qualification.
- Diagnostic BIOS builds on q35 also arm the TCO watchdog and reset ~3 minutes
  after reaching the shell (`run_bios` exits because of `-no-reboot`). Ordinary
  BIOS builds do not arm it. Smoke tests finish long before that.
- Each diagnostic BIOS boot writes the journal sector into the raw image
  (also under QEMU); ordinary BIOS boots do not.
- The UEFI ISO used by the smoke tests has no pre-loader, so log capture and the
  TCO watchdog stay off there.
