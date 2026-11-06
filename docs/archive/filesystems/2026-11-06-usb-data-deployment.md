# USB boot and data deployment plan

Add a preformatted ext2 partition to the current hybrid boot image.
Keep initial creation separate from updates that preserve existing data.

```text
Initial: build boot payloads + ext2 + manifest -> USB write -> verify -> eject
Update: build -> read USB snapshot -> retain backup -> stage changes as user
        -> check original identity and bytes -> write bounded regions -> verify -> eject
```

## Scope and decisions

Baseline: `22e2865`, the USB mass-storage implementation in PR 31.
Use the fresh BIOS/UEFI layout, with an explicit ownership record in its unused pre-ESP gap.
Append one Linux data partition after the ESP. Default capacity is 128 MiB.
The remaining stick space stays unused because the current ext2 profile supports one bounded block group.
Install the configured program manifest, including the disk-only utility by default.

Provide separate menu actions for creation, boot update, program deployment, and combined update.
Boot updates preserve every data-partition byte. Program deployment preserves all unlisted files and their supported metadata.
Keep a local pre-update backup. Reject foreign, modified, overlapping, truncated, or unsupported layouts.
Reject a changed device identity or changed source bytes before writing.
Keep the disk signature and partition-table bytes identical during updates. Write only the selected existing regions.
Keep builds and e2fsprogs unprivileged. Request sudo only for raw-device reads and writes.
Use arrow-key menus without typed erase phrases. Retain the existing boot-only options under a separate submenu.

Physical writes are not part of implementation validation.
Raw-device updates are not power-loss atomic. A retained backup supports explicit recovery after interruption.
No kernel, Native API, ext2-capacity, GPT, EHCI, or hub-support changes are planned.

## Impact assessment

| Area | Requirements and effect |
| --- | --- |
| Scope | GOALS: Storage and filesystems, Simplicity first; complete the boot-and-data deployment path. |
| Boot/platform | ARCHITECTURE: Persistent QEMU storage and boot artifacts; retain BIOS and FAT16 ESP contents, add bounded MBR partition three. |
| Memory/isolation | Validate host image bounds and ownership before publication. Stream copies; retain source snapshots. No guest allocation or mapping changes. |
| Concurrency | Require offline images and unmounted USB volumes. Use file locks, disk identity checks, and complete managed-region hashes before writes. |
| Interfaces | Native API implemented profiles remain unchanged. Extend host tools, ownership metadata, and the USB menu only. |
| Runtime resources | Measure image size, build/staging time, peak temporary storage, and update I/O against the boot-only baseline. |
| Validation/documents | Real file-backed image/update tests, malformed layout and interruption checks, menu/privilege contracts, BIOS/UEFI same-device boot and reboot persistence. |

## Sequence and acceptance

1. Add strict managed-layout creation, inspection, and offline staging.
2. Add bounded physical snapshot and update helpers with retained backup and identity checks.
3. Connect the flows to the USB menu and explicit image targets.
4. Test boot-only updates for byte-identical data and program updates for unlisted-file preservation.
5. Boot the combined image as the only USB disk under BIOS and UEFI; verify files after updates and reboot.
6. Update guides, record measurements, and obtain an independent read-only review.

The original stick must remain unchanged when validation or staging fails.
Unsupported layouts must never fall back to full-image replacement during an update.
Do not claim physical firmware or device qualification from QEMU results.

## Implementation and validation

The USB menu now provides creation, boot update, program deployment, and combined update actions.
The generated data image contains a 128 MiB ext2 partition and the selected program manifest.
No kernel or native-interface source changed in this task.

```sh
cmake --preset default
cmake --build build --target os1_image os1_bios_image os1_usb_image
ctest --test-dir build --output-on-failure
python3 -I tests/scripts/test_usb_data_image.py
python3 -I tests/scripts/test_usb_menu.py
python3 -I tests/scripts/test_build_configuration.py
```

The full matrix passed 35 checks in 171.73 seconds.
It includes the new combined-image BIOS and UEFI tests, existing USB failure checks, SMP checks, and host script contracts.
Both combined-image tests use USB as the only boot and data disk.
Four independent guest runs verify initial content, byte-preserving boot replacement, program-version replacement, and later reboot persistence.
A sentinel file and the 128 KiB verification payload survive both updates.

Nine image contract tests use real ext2 images and e2fsprogs.
They check unlisted inode metadata, partition and identity preservation, changed-content refusal, and unchanged output after staging failure.
They also check locks, dirty media, malformed layouts, synchronization failure, backup retention, and untouched device tails.
A raw-device wrapper rejects any unaligned read or write.
Twenty-one USB policy/menu tests pass, including identity changes before raw access and initial GPT-tail clearing.
The fresh build-configuration test covers normal, testing-disabled, and QEMU-unavailable configurations.
The interactive USB build function also produced `build/usb-write/artifacts/os1-usb-data.img` successfully.

No physical disk was read or written during validation.
Sudo authentication, physical media latency, firmware compatibility, and physical USB controller operation remain unmeasured.
Existing C++ host tests were not repeated because no kernel or C++ source changed.
Documentation links, Python syntax, shell syntax, and diff checks are checked separately.

## Measurements

Baseline: `22e2865` boot-only image construction. Candidate: this task's managed-image and update helpers.
Both use the same already-built kernel, initrd, BIOS payload, and Limine files.
Environment: macOS 26.6.2 ARM64, Python 3.15.2, GCC 11.2.0-built Debug artifacts, QEMU 11.1.3.
Both `debugfs` and `e2fsck` use e2fsprogs 1.47.4 from `/opt/homebrew/opt/e2fsprogs/sbin/`.
Guest tests use TCG, `q35`, four virtual CPUs, and 128 MiB RAM.
BIOS tests use QEMU's bundled SeaBIOS. UEFI tests use the following Homebrew firmware files:

| Firmware | SHA-256 |
| --- | --- |
| `/opt/homebrew/share/qemu/edk2-x86_64-code.fd` | `33090cc07675baa5190d9f1e84bf5176b33bcbfa9bacac522961150cdb6dbb2a` |
| `/opt/homebrew/share/qemu/edk2-i386-vars.fd` | `5d2ac383371b408398accee7ec27c8c09ea5b74a0de0ceea6513388b15be5d1e` |

Each UEFI run receives a private variable-store copy.
The main matrix uses `OS1_KMEM_DEBUG=ON` and `OS1_QEMU_DEBUGCON=ON`.
The normal USB-menu build disables debugcon and boot diagnostics; its additional BIOS test also passed.
Host CPU model and physical-device measurements were not recorded.

Packaging measurements exclude compilation, CMake configuration, authentication, and user response time.
Three repetitions use temporary regular files on the local host filesystem.
Update timing includes managed-region hashing, writes, fsync, and readback; it does not measure USB hardware.
The program-staging workload installs one compiled probe beside the existing disk utility and readme.

| Measurement | Baseline | Candidate | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| Image packaging median, seconds | 0.033053 | 0.140836 | +0.107783 | +326.09% |
| Image bytes | 35651584 | 169869312 | +134217728 | +376.47% |
| Kernel and initrd payload changes | 0 | 0 | 0 | Undefined; zero baseline |

| New operation | Median seconds | Written region |
| --- | ---: | --- |
| Stage boot update | 0.086633 | 162 MiB staged file |
| Stage program deployment | 0.335092 | 162 MiB staged file plus filesystem staging |
| Apply boot update to regular file | 0.268304 | 34 MiB; data bytes remain untouched |
| Apply program deployment to regular file | 0.224762 | 128 MiB data partition |

A combined update writes 162 MiB. A snapshot retains 162 MiB per update attempt at the default capacity.
Programs update the complete bounded data partition, not only changed sectors.
Peak temporary storage can reach approximately 676 MiB plus program payloads during deployment.
This bound includes the backup, wrapper, validation copy, and deployment copies; earlier backups and generated images are additional.
Copies use one-MiB chunks. Peak process memory and physical transfer time were not measured.
No numeric packaging-latency gate exists. All tested preservation and boot requirements passed.

Evidence: `build/usb-data-evidence/`, including `measurements.json`, the measurement script, and validation logs.

## Independent review

The first read-only review found an unaligned raw-disk identity read and a missing `smoke_all` image dependency.
Both were corrected, with alignment-enforcing I/O and generated-build-graph regressions.
The second review found that the new dependency assertion incorrectly required QEMU in build-only environments.
The test now follows the generated target availability and explicitly covers QEMU absence.
An outdated architecture sentence also omitted privileged snapshot reads; it now includes them.
The third independent review passed with no remaining implementation findings.
It checked raw-device boundaries, privilege flow, preservation checks, test dependencies, documents, and measurement calculations.

The complete ASD-STE100 dictionary was unavailable.
Prose review applied the repository structure, sentence, and terminology rules without claiming complete dictionary validation.
