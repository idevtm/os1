# Build and update a USB boot disk

The USB menu creates a bootable BIOS/UEFI disk with a preformatted ext2 data partition.
Separate update actions preserve the existing data layout and files.

```text
Create: build boot image + ext2 + programs -> select disk marked Erase and write -> write -> verify -> eject
Update: build -> select USB -> snapshot and backup -> stage and validate
        -> select Apply -> write selected regions -> verify -> eject
```

## Initial creation

1. Run `./os1.sh` without sudo.
2. Select **USB > Build and write USB stick**.
3. Select **Create boot + ext2 data image (erase USB)**.
4. After the build, move from **Cancel** to the target disk marked **Erase and write**.
5. Press Enter to start the write.
6. If sudo requests your macOS password, enter it.
7. Wait for readback verification and ejection.

A full-image write replaces the selected disk's partition layout and contents.
The tool removes an old backup GPT outside the generated image.
It does not securely erase unused sectors.

The USB menus discard queued input and start on **Cancel**.
Press `q` or Escape to cancel a menu.
The tool records the macOS I/O Registry entry ID for the selected disk attachment.
It checks that ID and the disk metadata again after selection and before writing.
Select a replacement disk in a new operation.

The generated file is `build/usb-write/artifacts/os1-usb-data.img` by default.
It contains the BIOS payload, a 32 MiB FAT16 EFI System Partition, and a 128 MiB ext2 partition.
The complete image occupies 162 MiB. Unused stick capacity remains outside these partitions.
The current ext2 implementation supports at most 128 MiB per filesystem.
Set `OS1_USB_DATA_MIB` in the USB build directory to choose 4 through 128 MiB before creation.
Updates do not resize partitions.

Build an image without accessing a physical device:

```sh
cmake --preset default
cmake --build build --target os1_usb_data_image
```

This target creates `build/artifacts/os1-usb-data.img`.
It recreates that output image explicitly; it does not modify the interactive QEMU data disk.
The host requires e2fsprogs for validation and program deployment.
On macOS, install it with `brew install e2fsprogs`.

## Upgrade from the full initrd

Initrd contains only `init` and `rescue`.
The normal shell and default resources reside on ext2.
Other programs live under `/bin`; the shell finds them by name.
Select **Update boot and programs (preserve data)** when upgrading an existing managed boot-plus-ext2 stick.
A boot-only update preserves data but does not install the moved programs.
Boot-only images provide the rescue console when no usable ext2 shell is available.
Use a boot-plus-ext2 image for the normal shell, editor, and guided observers.
Boot-only images do not contain the optional verification programs.

## Keyboard-independent diagnostics

Use this image when USB key presses have no effect after the kernel starts.
The [diagnostic plan](2026-11-06-keyboard-diagnostic-plan.md) records the hardware evidence and validation.

```text
USB menu -> diagnostic image -> automatic boot -> try keys
    -> warm restart -> saved boot log -> collect on development computer
```

1. Run `./os1.sh usb` without sudo.
2. Select **Create keyboard diagnostic image (erase USB)**.
3. After the build, select the target USB disk marked **Erase and write**.
4. If the writer requests your password, enter it.
5. Wait for verification and ejection.
6. Boot the target computer from the UEFI USB entry.
7. Wait for the USB diagnostic summary and shell.
8. Press a few ordinary keys.
9. Wait for a warm restart and the **previous boot log saved** message.
10. Power off after that message.
11. Connect the stick to the development computer.
12. Copy the `os1logs` folder from the **OS1 EFI** volume.

The image starts diagnostics without a boot-menu key press or shell command.
It replaces USB contents and contains no ext2 data partition.
Its separate build directory is `build/usb-write/keyboard-diagnostic/`.
Normal USB builds explicitly disable this profile.
The new records contain counts and transfer results, not key values; ordinary console output can still contain typed text.

The existing Intel watchdog attempts a reset about three minutes after user space starts.
If automatic reset is unavailable, use the physical reset button without removing power.
Then boot the same USB image again to save the RAM log.
A power cycle can discard that log. Photograph the summary if warm restart is unavailable.
Firmware behavior can prevent RAM-log recovery; QEMU validation does not establish physical recovery.
After saving a log, the pre-loader waits to prevent a restart loop.

The summary reports controller state, configured keyboards, and armed input transfers.
Detailed records include pre-reset port state, firmware ownership, enumeration, and completion codes.
Intel routing-register reads apply only to controller `8086:8c31`.
Legacy result values are `0` absent, `1` present, and `2` invalid.
The legacy support register uses bit 16 for BIOS ownership and bit 24 for OS ownership.
Diagnostic observations do not modify these registers.
All profiles include the [firmware handoff and Intel routing correction](2026-11-06-usb-keyboard-handoff.md), pending physical acceptance.
Expect `xhci: firmware handoff complete`, then Intel routing result `2` when routes change or `1` when already enabled.
Result `3` means routing verification failed; the driver attempts to restore original routing.

BIOS diagnostic boot also starts automatically and captures logs in reserved image sectors.
The EFI volume does not contain BIOS logs; use [BIOS log extraction](boot-log.md#operator-procedure) after a warm restart.
UEFI is the recommended collection path for this desktop test.

Build without writing a device:

```sh
cmake --preset default -B build/usb-write/keyboard-diagnostic \
  -DOS1_USB_KEYBOARD_DIAGNOSTICS=ON -DOS1_BOOT_DIAGNOSTICS=ON -DOS1_QEMU_DEBUGCON=OFF
cmake --build build/usb-write/keyboard-diagnostic --target os1_usb_image user_programs
ctest --test-dir build/usb-write/keyboard-diagnostic --output-on-failure -R '^os1_smoke_usb_keyboard_'
```

The image is `build/usb-write/keyboard-diagnostic/artifacts/os1-usb.img`.

## Boot and use storage

Fresh UEFI images offer `os1` and `os1 diagnostics`.
The menu selects `os1` after ten seconds unless you select another entry.
The diagnostic entry enables a 1 MiB kernel ring and all instrumented API entry and completion records.
It drains persistent logs every 250 milliseconds and synchronizes pending writes every second.
Its background export ceiling is 256 KiB/s; retention and privacy rules remain unchanged.
Scheduler and individual interrupt tracing remain opt-in.
Normal images do not enable watchdog resets when you select diagnostics.
The dedicated keyboard-diagnostic image retains its automatic capture and reset workflow.
Preserved-layout kernel updates retain the template menu; a fresh image installs these entries.
The custom BIOS loader retains its build-selected profile and has no interactive menu.


Boot the USB disk through the computer's BIOS or UEFI menu.
The kernel mounts the data partition at `/` when the device matches the [USB storage profile](usb-storage.md).
It requires an xHCI-connected root-port device; EHCI-only controllers and external USB hubs remain unsupported.
The BIOS/UEFI firmware's ability to boot a stick does not prove kernel controller support.

```text
ls /
/bin/diskutil seed
/bin/diskutil verify
echo saved > /note
/bin/diskutil sync
```

Run `/bin/diskutil sync` before shutdown or removal after writes.
Then reconnect the stick to the development computer for updates.
A dirty or unsupported filesystem prevents update staging; the tool does not repair or format it automatically.

## Copy the data partition to a file

Use this action to examine the files of a stick, for example an exported log, on the development computer.

```text
./os1.sh usb_capture -> select the stick if more than one is connected -> sudo reads the stick
read -> temporary file with the ext2 data partition -> the tool prints the file path
```

Run `./os1.sh usb_capture` without sudo, or select **USB > Copy USB stick data to a temporary file**.
The tool accepts only a stick with the os1 boot and data layout. It unmounts the stick and does not write to it.
The last output line is `USB data partition copy:` and the path of the file.
The lines before it are from the build configuration. Use `./os1.sh usb_capture | tail -n 1` to get only that line.
The file is in the temporary directory of your user. Delete it after use.
If only one eligible USB disk is connected, the tool reads it without a question.
The tool checks the os1 layout after it unmounts the disk. A different disk gets no write, and the tool then stops.
The stick stays unmounted and is not ejected. A stick with write protection is not eligible.
The `os1_usb_capture_contract` check runs this action with a disk image in place of the stick and of sudo.
One run against a physical stick on November 14, 2026 gave a correct file.
Use `debugfs` of e2fsprogs to list or copy files from the ext2 file, for example `debugfs -R "ls -l /log" <file>`.

## Update an existing installation

Run `./os1.sh usb` and choose one of these actions:

| Action | Written region | Preservation |
| --- | --- | --- |
| Update boot files | BIOS payload and ESP | Every data-partition byte remains identical. |
| Deploy programs | Ext2 partition | Unlisted files keep their contents and supported inode metadata. |
| Update boot and programs | Both existing regions | The partition table and installation identity remain identical; unlisted files remain intact. |

The tool builds first, then asks you to select the disk.
It unmounts the disk and reads the managed region into a local backup.
Backup names include the disk identifier, date, time, and a unique suffix.
Backups remain in `build/usb-write/artifacts/usb-backups/` after success, cancellation, or failed staging.
Retain them until the updated stick passes your boot and file checks.

Staging and e2fsprogs run as your normal user.
After staging succeeds, move from **Cancel** to **Apply** for the displayed disk.
Press Enter to apply the update.
The tool checks the device identity and complete original managed-region hash again before writing.
A changed disk, changed layout, or changed content stops the operation before any update write.
It verifies readback after cache synchronization and then ejects the stick.

Only images created by this boot-and-data workflow support these updates.
The ownership record resides in a reserved sector before the ESP.
Foreign layouts and earlier boot-only images are rejected; updates never fall back to whole-disk formatting.
Initial creation is required to convert an earlier boot-only stick.

Updates use raw-device writes and are not power-loss atomic.
An interrupted boot update can prevent booting. An interrupted program update can require filesystem recovery or backup restoration.
The retained backup contains all managed boot and data regions, not the unused remainder of the stick.
A synchronization or readback failure leaves the backup available and does not report successful ejection.

## Program selection and privilege boundaries

`OS1_STORAGE_MANIFEST` selects programs for initial creation and deployment.
The USB build directory defaults to its generated manifest containing `/bin/diskutil`.
Set a custom manifest in that build directory to deploy additional programs.
See [program deployment](../filesystems/program-deployment.md) for its format and preservation rules.
The old standalone QEMU deployment command still rejects combined USB images.

The initial writer prints its active stage immediately after sudo starts it.
Image checks, writes, and readback show completed MiB and percentages.
Unmount, cache synchronization, and eject show elapsed time once per second while waiting.
A waiting message confirms that the helper is active; it does not claim that the device has completed the operation.
Only successful cache synchronization and readback permit the final verified-and-ejected message.

Only raw-device snapshot and write steps request sudo.
Before sudo starts, the helper enables line input and Enter-to-newline conversion.
It restores the prior terminal settings after sudo returns, including cancellation.
Password entry stays visible in the normal terminal context; sudo hides the typed characters.
The writer accepts only external physical writable USB disks with 512-byte logical sectors and sufficient capacity.
Build tools, filesystem tools, and program manifests never execute with elevated privileges.

## Managed image layout

| Region | Sector range or capacity |
| --- | --- |
| MBR | Sector 0; BIOS boot code and three partition records |
| BIOS partition | Type `0xDA`, start 1, count 2879 |
| Ownership record | Sector 4095; outside all partitions |
| EFI System Partition | Type `0xEF`, start 4096, count 65536 |
| Ext2 data partition | Type `0x83`, start 69632; 4 through 128 MiB |
| Fourth partition record | Empty |

All sectors contain 512 bytes. The data start is 34 MiB from the beginning of the disk.
The ownership record contains a magic value, version, data capacity, partition-table hash, installation identifier, and record checksum.
It detects unsupported or accidentally changed layouts; it is not an authentication credential.
Boot updates preserve the existing MBR disk-signature/table bytes and ownership record.
They replace supported boot payloads without moving any partition.

## Compatibility, checks, and evidence

**Boot-only image options** retains the old fresh-image and verified-base replacement workflows.
Those actions write a complete boot-only image and do not preserve an existing data partition.
The preserved-base option still requires its exact verified input image.

`smoke_usb_storage` includes BIOS and UEFI boot from the combined image as the only USB disk.
The tests create guest files, update boot files, replace a program, and verify subsequent boots.
Host tests cover partition bounds, ownership, locks, staging failure, raw-sector alignment, changed content, and byte preservation.
No physical disk was written during implementation validation.

See the [plan and evidence](../archive/filesystems/2026-11-06-usb-data-deployment.md) for measurements and review dispositions.
The [historical workflow record](../archive/tooling/2026-11-06-usb-image-menu-history.md) preserves earlier boot-only measurements.
