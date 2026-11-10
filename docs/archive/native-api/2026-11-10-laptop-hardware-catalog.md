# Laptop hardware catalog comparison — 2026-11-10

The Asus G74SX captures confirm PCI coverage and a complete USB storage relationship chain.
They also identify unsupported USB speeds and utility presentation gaps.

```text
PCI 00:1d.0 -> EHCI root port 1 -> Intel hub 8087:0024
  -> USB stick 0951:1666 -> mass-storage interface -> block_0
  -> partition_2 -> ext2 filesystem_0 -> /disk mount_0
```

## Evidence and scope

The user captured five utilities under `/disk/hw/laptop` during an OS1 boot.
The comparison uses the supplied Linux captures from the same laptop.
The OS1 output matches the `edd3db4` catalog profile; the text files do not embed a build identifier.
The captures validate observed data, not every command or interactive navigation behavior.
The desktop was not tested in this report.

The captures contain different attached devices. Linux shows a USB mouse but no boot stick.
OS1 shows the boot stick but no mouse.
USB addresses and bus numbers are boot-local; comparisons use PCI addresses, device IDs, and topology.

## Comparison

| Area | Linux evidence | OS1 result |
| --- | --- | --- |
| PCI | 18 functions | All 18 addresses match; no extra or missing functions. |
| PCI topology | GPU, Wi-Fi, USB 3, and Ethernet behind bridges | All four parent relationships match. |
| USB controllers | Intel EHCI at `00:1a.0` and `00:1d.0`; Fresco xHCI at `04:00.0` | All three drivers report `started`. |
| Intel hubs | Two `8087:0024` hubs | Both configured with `usb-hub`. |
| Webcam | `13d3:5134`, video interfaces | Device described; six interface descriptors remain unbound. |
| Card reader | `0bda:0139`, vendor-specific interface | Device described; interface remains unbound. |
| Bluetooth | `0cf3:3005`, 12 Mbit/s behind EHCI | Missing; the high-speed-only hub path does not enumerate this transport. |
| Mouse | `1c4f:0034`, 1.5 Mbit/s behind EHCI | Missing. If connected during OS1 boot, the same transport restriction applies. |
| USB stick | Absent from supplied Linux captures | `0951:1666`, configured with `ehci-bot`; `/disk` mounted. |
| Internal storage | 476.9 GiB disk, 698.6 GiB disk, and optical drive | SATA controller discovered; no AHCI driver or corresponding block devices. |
| CPU | Core i7-2670QM identity, capabilities, and frequency | Eight enabled logical processor records; no model or frequency fields. |
| Memory | 16 GiB system memory | No memory inventory in this catalog profile. |
| Firmware | Selected platform devices | 102 ACPI namespace records; these are not 102 additional physical devices. |

The webcam has two interface numbers. Its additional descriptors represent alternate streaming settings.
OS1 records 16 controller ports, five USB devices, and ten interface descriptors.
Controller ports do not correspond one-for-one with external laptop connectors.
Every nonzero parent and backing ID in the OS1 hardware output resolves to another captured record.
No capture reports lost records, traversal limits, or incomplete inspection.

## Storage verification

The block device reports 30218842 sectors of 512 bytes, or 15472047104 bytes.
Its three primary MBR entries describe BIOS, EFI, and Linux data areas.
The Linux data area starts at sector 69632 and contains 262144 sectors.
macOS independently reports offset 35651584 bytes and size 134217728 bytes.
These values match the OS1 capture.

The filesystem reports 4096-byte blocks and 32768 blocks.
Its UUID is `c3fa8ae0-056c-403d-9ef4-275fb906b012`.
`dumpe2fs` confirms these values on a read-only local partition copy.
`e2fsck -fn` completes all five passes without errors; the filesystem state is clean.
The utility captures survived transport back to the Mac and were extracted byte-for-byte.

The stick still has approximately 15.3 GB of unallocated space.
The 128 MiB data partition size reflects the current image layout, not a catalog capacity error.

## Follow-up

The [catalog plan](../../native-api/hardware-catalog-plan.md#laptop-comparison-and-presentation-follow-up--2026-11-10) records the accepted implementation.
Utilities need useful default fields, hexadecimal bus identifiers, readable capacities, and a controller-first USB tree.
Unsupported connected devices need anonymous records instead of disappearing from the view.
Unknown device IDs and root-port speeds must remain unknown.
New capture behavior requires another physical-device test; these files predate that implementation.
The presentation follow-up adds CPU identity and boot memory totals after these captures.
Low/full-speed transfers, AHCI, class drivers, and memory-module inventory remain separate work.

## Capture fingerprints

Original OS1 files remain under `build/hardware-comparison-2026-11-10/os1/laptop/`.
The local partition copy is `build/hardware-comparison-2026-11-10/data.ext2`.
Linux inputs came from the user-provided `drive-download-20261110T062301Z-1-001` folder.
The table preserves their identities without publishing unrelated network configuration from Linux `lshw`.

| Capture | File | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| OS1 | `lsblk.txt` | 1339 | `22da4ba353451be059d6efee22702121b6ff136d519661b95b2118f9dd56660f` |
| OS1 | `lshw.txt` | 29934 | `c4059bbd9da609d390f35d3b782f6541e61404b13eff4e8fe178dfecd3b85f9a` |
| OS1 | `lspci.txt` | 2965 | `ab98b3a372b264336e46e5042fc9c9aba2c63f5a372be169cb9d07a2269f922d` |
| OS1 | `lsusb-t.txt` | 7011 | `45e7710ccab589b54314f212df417c1ad0314c06f7449d13d04af5134dd8906b` |
| OS1 | `lsusb.txt` | 2945 | `e5230841f13ca715078d019824991598bbedde3f7c9f1a53c442eaf8d7220134` |
| Linux | `lsblk.txt` | 1451 | `fdb6c579d668d76bf1f64050302b0abc7efcbaed6a05b8f53d0a150bea7c852b` |
| Linux | `lshw.txt` | 15154 | `bca46072cc1745b28ec5a1c1f808d81b653f8bdf5b84135a7ed084886eee23a9` |
| Linux | `lspci.txt` | 1908 | `6fe5df39c315244318539f63fe952a207ba7ce31e7f33072ad123f9efd50f47b` |
| Linux | `lsusb-t.txt` | 878 | `c00bea1f5d773711785878aea00a508d9faf2ff3d761f59ace95625e1f5bab0b` |
| Linux | `lsusb.txt` | 710 | `cff9bff92fbfcf1be2b5e0c8377e530c96392af8e3553c5e91b50a39d1b75edb` |
