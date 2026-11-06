# Historical USB image menu workflow

These records describe earlier boot-only tooling. Use the [current guide](../../hardware/usb-image-workflow.md) for boot and data deployment.

# Build and write a USB image

The new `os1.sh` USB option builds current kernel and initrd files before selecting a target disk.
Only the disk-write helper requests administrator privileges.

```text
os1.sh -> choose image layout -> ordinary-user build -> choose USB disk
       -> sudo writer -> readback verification -> eject
```

## Plan and impact

| Area | Requirement and effect |
| --- | --- |
| Scope | GOALS: Vision, Simplicity first, and Runnable vertical slices. Add one menu action with a direct command equivalent. |
| Boot/platform | Architecture: Build Outputs And Boot Artifacts. Fresh hybrid packaging and the pinned UEFI layout both receive current kernel and initrd files. |
| Memory/concurrency | Kernel behavior is unchanged. Preserve existing uncommitted reliability work and its validation evidence. |
| Interfaces | Extend the menu manifest and add host-side helpers; no syscall or BootInfo change. |
| Resources | Measure build/package time and image size; writing affects the selected external USB disk. |
| Validation | Test target eligibility, menu selection, identity rechecks, image replacement, and readback without writing a physical disk. |

The workflow initially supports macOS `diskutil`.
It refuses internal, virtual, partition-only, non-USB, read-only, and undersized devices.
Both image formats require 512-byte logical sectors; the helper rejects all other sector sizes.
Arrow-key menus select the layout and write destination.
Selecting **Write to disk…** starts the writer without a typed confirmation.
Press `q` or Escape to cancel either menu.
The writer rechecks identity after unmounting and verifies the exact image prefix by SHA-256.
It uses the macOS `DKIOCSYNCHRONIZECACHE` command before readback; a synchronization error stops the operation.
It ejects only after successful readback.
Do not run `os1.sh` with sudo; configure and build steps remain unprivileged.

The preserved image requires the pinned round-6 capture used by the existing kernel-only updater.
It updates both payloads while preserving the firmware-visible layout and unrelated files.
Its legacy BIOS area remains historical; select its UEFI boot entry.
The fresh image includes current UEFI and BIOS payloads but still requires physical qualification.

## Initial validation

These results describe the initial workflow, before the menu revision below.

The following checks passed on macOS 26.6.2 ARM64:

| Check | Result |
| --- | --- |
| Helper contracts | 14 tests passed; temporary files exercise actual copying and checksum verification. |
| CTest | All four script contracts passed with local socket access, including both USB contracts. |
| Menu | Historical: `sh -n run.sh` passed; the generated menu includes `usb`. |
| Interactive cancellation | Historical: `./run.sh usb` reached layout selection; `q` returned without elevation. |
| Disk inventory | Read-only inspection identified the external Kingston USB disk as eligible. |
| Preserved FAT | Independent parser verified 14 entries, matching FAT copies, valid chains, and unchanged unrelated files. |
| Preserved UEFI | QEMU boot and `ipccheck` passed. |
| Fresh UEFI and BIOS | Both QEMU boots and `ipccheck` passed. |

QEMU used q35, TCG, eight CPUs, and 256 MiB RAM.
UEFI used Homebrew QEMU OVMF firmware; BIOS used QEMU SeaBIOS.
Each smoke used USB mass storage and a one-second observation interval after its required markers.
Logs, exact commands, hashes, and results are in `build/usb-menu-evidence/`.

The build used GCC 11.2.0, Debug, `OS1_QEMU_DEBUGCON=OFF`, and `OS1_BOOT_DIAGNOSTICS=OFF`.
Sources included commit `7e330ea` and the uncommitted reliability, clock, and USB changes.
The helper reused the current cross-toolchain configuration in a separate build directory.

Three repetitions compared the existing kernel-only updater with the new payload updater, using identical base and kernel files.
The new updater also copied the current initrd.

| Measurement | Kernel-only baseline | Kernel and initrd | Difference |
| --- | ---: | ---: | ---: |
| Median packaging time | 72.436 ms | 85.364 ms | +12.928 ms; +17.85% |
| Image size | 41,943,040 bytes | 41,943,040 bytes | 0 bytes; 0% |

The fresh hybrid image occupies 35,651,584 bytes.
The incremental image build took 16.665 ms at the median of three runs.
The preserved layout costs no additional disk space compared with its baseline.
Packaging uses temporary image-sized allocations; peak host memory was not measured.
USB throughput and privileged raw-device I/O remain unmeasured because this task does not write hardware.
A future confirmed write must validate macOS unmount, raw-device flush, readback, and eject behavior on the selected stick.
Disk identity checks use available `diskutil` fields; they cannot distinguish every identical replacement device.
The first independent review found missing raw-device cache synchronization and sector-size validation.
It also found stale test totals in the architecture document.
All three findings were confirmed and corrected; regression tests cover the two behavioral defects.
The second independent review approved the complete revised change with no remaining findings.
It reviewed all eight task files and the recorded validation evidence.
Physical-device validation and full language-standard verification remain outside that approval.
No physical disk write is part of this development task.
Full ASD-STE100 compliance remains unverified because the complete standard and dictionary are unavailable.


## Launcher rename

The launcher is now `os1.sh`; the old `run.sh` path was removed.

| Requirement | Impact |
| --- | --- |
| GOALS: Simplicity first; Runnable vertical slices | Use the project name for the shared build, run, and USB menu. |
| Architecture: Build Outputs And Boot Artifacts | Update current commands and CMake guidance to `./os1.sh`. |
| Interfaces | Preserve arguments and environment variables; change only the launcher filename and its diagnostic text. |
| Boot, memory, isolation, concurrency, and resources | No executable behavior or kernel change; no runtime measurements apply. |
| Validation | Check shell syntax, executable permission, reference consistency, and interactive cancellation. |

Older dated reviews retain the historical `run.sh` name.
The validation rows above record commands executed before the rename.
Shell syntax, executable permission, reference checks, and `./os1.sh usb` cancellation passed.
The rename adds no kernel or image changes; no build or QEMU test was required.
Independent review approved the rename, permissions, current references, and validation evidence with no findings.


## Menu revision and firmware investigation

The USB menus now use Up/Down and Enter; the typed erase phrase is removed.
The layout menu names operations rather than computer models.

| Choice | Result |
| --- | --- |
| Replace kernel and initrd | Update the verified base image, then write the complete image to USB. Retain the UEFI layout. |
| Create a fresh boot image | Build new UEFI and BIOS payloads with a fresh filesystem. |
| Write to disk… | Select the destination and start the privileged writer. |
| Cancel, `q`, or Escape | Return without starting a write. |

The replacement option does not modify files on the attached stick in place.
It is available when the base image exists at the configured path.
Set `OS1_USB_BASE_IMAGE` to select another location for the same verified base image.
An unsupported base still fails its checksum validation before any disk write.
The terminal restores its settings and cursor after selection, cancellation, SIGINT, SIGTERM, or SIGHUP.
Job suspension through Ctrl-Z does not run this cleanup and can leave the menu screen active and cursor hidden until the process resumes.

This revision serves GOALS: Simplicity first and Runnable vertical slices.
It changes the host interaction described in Architecture: Build Outputs And Boot Artifacts.
Image construction, disk eligibility, identity checks, cache synchronization, and readback remain unchanged.
Boot, UAPI, memory isolation, and kernel concurrency contracts are unaffected.
The [firmware investigation](../../hardware/uefi-layout-investigation.md) records the format audit and proposed controlled hardware tests.

Seventeen helper tests pass, including real pseudo-terminal navigation, cancellation, signal interruption, and terminal restoration.
The tests cover layout cancellation before building and destination cancellation before elevation.
The actual `./os1.sh usb` menu accepted Up and Enter to select Cancel.
The two USB CTest contracts pass.
No physical disk was written.

Three runs measured process startup through the first menu prompt, excluding user response time.
The host used macOS 26.6.2 ARM64 and Python 3.15.2, with a 100-column, 24-row pseudo-terminal.
Both versions used commit `7e330ea` plus the same prior uncommitted changes; only the menu revision differed.
The median increased from 27.493 ms to 40.392 ms: +12.899 ms, or +46.92%.
This small sample is descriptive; it is not a performance guarantee.
The image hashes and sizes remain unchanged: 35,651,584 bytes fresh and 41,943,040 bytes preserved, each with a 0% size difference.
The menu blocks while waiting for input; no polling loop consumes idle CPU.
The packaging and raw-device functions are unchanged; their timing was not remeasured.
Evidence is in `build/usb-menu-v2-evidence/menu-measurements.json` and `unit-tests.log`.
Independent review found no blocking defects in the menus, privilege boundary, tests, or firmware investigation.
It identified ambiguous layout guidance and a Ctrl-Z cursor limitation.
The footer now explicitly limits its preservation claim to the replacement choice.
The Ctrl-Z limitation is recorded above; job-control suspension remains outside the tested signal handling.
The correction review approved the footer and documentation changes with no blocking findings.


## Password-prompt visibility correction

The reported transcript ends at sudo's password prompt without a clear handoff from the disk menu.
The menu clears the normal screen and moves the cursor home immediately before launching sudo.
This places a bare prompt at the top of an empty screen and removes the selected-disk context.

```text
menu in alternate screen -> restore normal screen -> selected disk and image
                         -> explicit sudo prompt -> privileged writer
```

This correction supports GOALS: Simplicity first and Runnable vertical slices.
It changes the host terminal flow described in Architecture: Build Outputs And Boot Artifacts.
Both launcher and USB menus use the alternate screen and restore the normal cursor position on exit.
The USB helper prints and flushes the destination and password guidance before launching sudo.
Sudo retains control of password input and its hidden-character behavior.
No additional confirmation or authentication step is added.
Boot, image formats, UAPI, memory isolation, kernel concurrency, and disk-write checks remain unchanged.
Validation checks the terminal handoff, cancellation, output ordering, and affected script contracts without writing a USB device.


The new regression assertions failed before the correction: five terminal cases and one missing-status test.
After correction, all 18 helper tests and both USB CTest contracts pass.
The real launcher entered the USB layout menu and cancelled successfully in a pseudo-terminal.
A separate pseudo-terminal probe replaced sudo with a harmless prompt function.
It verified normal terminal settings and the order: restore screen, print destination, then display the password prompt.
No clear-screen or cursor-home sequence follows restoration.
The probe neither requests a real password nor opens a USB device.

The probe measured selection-to-prompt latency in three runs per version, excluding authentication and user response time.
It used macOS 26.6.2 ARM64, Python 3.15.2, and a 100-column, 34-row pseudo-terminal.
Both versions used commit `7e330ea` plus the same prior uncommitted work; only this correction differed.
Median latency was 0.093 ms before and 0.082 ms after: -0.011 ms, or -11.99%.
These sub-millisecond samples describe the probe and do not establish a performance improvement.
Image construction and disk-write functions are unchanged; guest resource measurements do not apply.
Evidence is in `build/usb-prompt-evidence/`: `before.log`, `after.log`, `contracts.log`, `probe.py`, transcripts, and `measurement.json`.

Actual sudo authentication and visual behavior in the user's terminal remain unmeasured.
The code uses the terminal's existing alternate-screen support and lets sudo own password entry.
No kernel build or QEMU run was needed for this host-terminal correction.
Independent review approved terminal cleanup, prompt ordering, privilege separation, tests, and evidence with no findings.
The reviewer independently reran all 18 helper tests and the shell syntax check; both passed.


## USB menu grouping

The top-level launcher now lists only `usb` for interactive USB work.

| Entry point | Purpose |
| --- | --- |
| `./os1.sh` → `usb` | Choose kernel and initrd replacement or a fresh image, then write the USB disk. |
| CMake target `os1_usb_kernel_update` | Retain the kernel-only image operation for scripted use. |

Removing the separate target-menu registration supports GOALS: Simplicity first and the architecture's existing USB workflow.
The generated menu changes; build targets, image contents, disk writing, kernel interfaces, memory, and concurrency remain unchanged.
The kernel-only target remains distinct from the interactive replacement option, which also updates the initrd.

`cmake --preset default` regenerated the menu successfully.
The manifest retains `usb`, omits `os1_usb_kernel_update`, and preserves every other entry in its original order.
`cmake --build build --target help` still lists the kernel-only target.
No image was built or written.

The menu decreased from 19 entries to 18: one fewer entry, or 5.26%.
The manifest decreased from 2,165 bytes to 1,854 bytes: 311 fewer bytes, or 14.36%.
These are direct before/after counts for the same local CMake configuration; guest runtime measurements do not apply.

Independent review found no functional defects and confirmed the manifest and retained target.
Its paragraph-length finding was corrected by separating impact, validation, and measurements.
The correction review approved the result with no remaining findings.
