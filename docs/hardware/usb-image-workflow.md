# Build and update a USB boot disk

The USB menu creates a bootable BIOS/UEFI disk with a preformatted ext2 data partition.
Separate update actions preserve the existing data layout and files.

```text
Create: build boot image + ext2 + programs -> select USB -> write -> verify -> eject
Update: build -> select USB -> snapshot and backup -> stage and validate
        -> apply selected regions -> verify -> eject
```

## Initial creation

1. Run `./os1.sh` without sudo.
2. Select **USB > Build and write USB stick**.
3. Select **Create boot + ext2 data image (erase USB)**.
4. Select the target disk after the build completes.
5. Enter your macOS password if sudo requests it.
6. Wait for readback verification and ejection.

Selecting a disk for creation replaces its partition layout and contents.
The tool removes an old backup GPT outside the generated image.
It does not securely erase unused sectors.
There is no typed erase phrase. Press `q` or Escape to cancel a menu.

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

## Boot and use storage

Boot the USB disk through the computer's BIOS or UEFI menu.
The kernel mounts the data partition at `/disk` when the device matches the [USB storage profile](usb-storage.md).
It requires an xHCI-connected root-port device; EHCI-only controllers and external USB hubs remain unsupported.
The BIOS/UEFI firmware's ability to boot a stick does not prove kernel controller support.

```text
ls /disk
/disk/diskutil seed
/disk/diskutil verify
echo saved > /disk/note
/disk/diskutil sync
```

Run `/disk/diskutil sync` before shutdown or removal after writes.
Then reconnect the stick to the development computer for updates.
A dirty or unsupported filesystem prevents update staging; the tool does not repair or format it automatically.

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
After staging succeeds, select **Apply prepared USB update** to write it.
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
The USB build directory defaults to its generated manifest containing `/disk/diskutil`.
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
