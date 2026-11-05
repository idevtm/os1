# UEFI USB layout investigation

The format checks found no explanation for the reported firmware-menu failure.
This is a limited format audit, not complete UEFI conformance certification.

| Evidence | Result | Meaning |
| --- | --- | --- |
| Working round 6 versus failed round 7 | Identical MBR and FAT boot sector | Partition headers alone do not explain the difference. |
| EFI loaders | Identical bytes; different starting clusters | File placement remains a testable hypothesis. |
| FAT and directory checks | All three inspected images pass | No detected chain, name, or partition-boundary corruption. |
| Physical firmware | No new run | The root cause remains unknown. |

## Scope and requirements

This investigation supports GOALS: Educational value through source code and Runnable vertical slices.
It checks Architecture: Build Outputs And Boot Artifacts against captured images and published UEFI requirements.
It does not modify image generation, firmware, kernel code, or the attached USB disk.
Memory isolation, UAPI, concurrency, and guest resource use are unchanged.
The host menu changes and their measurements are recorded in the [USB workflow](usb-image-workflow.md#menu-revision-and-firmware-investigation).

## Standards and firmware age

The computer's age makes the implemented firmware revision relevant.
The manufacturing date does not establish that revision or its compliance.
UEFI 2.1 was released on January 23, 2007. See the [official revision history](https://uefi.org/specs/UEFI/2.10/Frontmatter/Revision_History.html).

The Unified EFI Forum's January 2006 UEFI 2.0 specification already describes these mechanisms:

| Mechanism | UEFI 2.0 reference |
| --- | --- |
| FAT12, FAT16, or FAT32 on removable media | Section 12.2.3.1; printed page 440 |
| An MBR partition with type `0xEF` | Section 5.2.1; printed pages 88–90 |
| The x64 fallback path `\EFI\BOOT\BOOTx64.EFI` | Section 3.4.1.1; printed page 63 |

These mechanisms are not recent additions.
The source is the [UEFI 2.0 specification, mirrored PDF](https://elhacker.info/manuales/Hardware/UEFI_2_Specification.pdf).
The official historical PDF endpoints returned HTTP 403 during this investigation.
The accessible document identifies the Unified EFI Forum as its author and January 31, 2006 as its publication date.

Current requirements also describe [removable FAT media](https://uefi.org/specs/UEFI/2.10/13_Protocols_Media_Access.html#removable-media) and [MBR system partitions](https://uefi.org/specs/UEFI/2.10/05_GUID_Partition_Table_Format.html#legacy-master-boot-record-mbr).
Neither the old nor current clauses establish that this particular firmware implements every requirement correctly.
They also do not prove that every field in our image is correct.

The installed BIOS version and reported UEFI revision remain unrecorded here.
Capture the BIOS version and date from setup.
Capture `EFI_SYSTEM_TABLE.Hdr.Revision`, `FirmwareVendor`, and `FirmwareRevision` through a diagnostic EFI application.
Do not change the working loader merely to collect this information during routine USB updates.

## Rechecked evidence

The read-only audit compared these local files:

| Image | SHA-256 | Bytes |
| --- | --- | ---: |
| Round 6 capture | `5a0643210bf7fdb2f5a95c741329dfd9d88f5c589e3473df1e124d28d17298ae` | 41,943,040 |
| Round 7 candidate | `32abefcc32b0aaf8423f7f8768dfdb3f09640b244158fcea1fdafe0b378a9976` | 35,651,584 |
| Current fresh image | `e59f35ec375da95ec62c8cdc431abb401f3d886ed97e6a188ed0eaa409a8978a` | 35,651,584 |

All three contain one `0xEF` partition starting at LBA 4096, with 65,536 sectors of 512 bytes.
The FAT16 volume has 16,343 data clusters.
The audit checked partition bounds and overlap, FAT copies, ownership, chain lengths, long-name checksums, and directory parent references.
It found the fallback executable at `\EFI\BOOT\BOOTX64.EFI` in each image.
Each executable has x64 machine type `0x8664`, PE32+ format, and EFI application subsystem `10`.
The audit did not validate every PE section or every firmware-loading requirement.

`fsck_msdos -n` passed on extracted ESP files from all three images.
The checks accessed regular files; no USB device was mounted or written.
Results and the audit script are in `build/usb-menu-v2-evidence/`.
Relevant files are `layout-audit.json`, `audit_layouts.py`, and the three `fsck-*.log` files.

The round-6 and round-7 MBR sectors match byte for byte.
Their FAT boot sectors also match.
The fallback executable moved from cluster 361 to cluster 362 without a content change.
Its SHA-256 is `db069df23cb9d002e2ead69f322f9cf3db556ddc0ab94f913114e09b76187ff0` in both images.
The Limine executable and kernel shim also retain identical contents.

Other differences prevent a single-cause conclusion.
The kernel grew from 650,952 to 651,472 bytes.
The initrd, configuration files, BIOS payloads, and captured log files also differ.
The EFI directories and subsequent files moved when the kernel required another cluster.
The current fresh image also replaces the diagnostic pre-loader with direct Limine boot, so it is not a controlled round-7 replay.

## Controlled hardware test plan

Retain the exact working capture as the baseline.
Use the same stick, USB port, firmware settings, and cold-boot procedure for each trial.
Verify the written bytes before each boot.
Record whether firmware lists the device, starts the EFI application, reaches Limine, and reaches the shell.

1. Record the installed BIOS and reported UEFI revisions.
2. Recheck the exact working and failed captures under identical conditions.
3. Change only the fallback executable's cluster allocation, with its contents unchanged.
4. Test changes to the EFI directory allocation separately.
5. Test configuration and non-EFI payload changes separately, retaining working EFI placement.
6. If these tests do not isolate the failure, compare a standard formatter with the custom FAT builder.
7. Then compare FAT32 or a single-partition image as separate compatibility experiments.

Each trial must record its checksum and precise byte differences.
A successful FAT32 or GPT experiment would show compatibility, not prove that FAT16 or MBR violates the older standard.
UEFI locates the boot executable through filesystem paths; the examined clauses do not assign it a fixed cluster.
The existing working image also demonstrates that this firmware can boot our MBR/FAT16 layout.

No new physical boot result is claimed.
The hardware failure remains unresolved; no packaging change is presented as its fix.
Independent review checked the evidence and historical specification clauses and found no blocking defects.
