# Laptop hardware catalog retest — 2026-11-10

The new Asus G74SX capture confirms platform descriptions, firmware associations, and previously missing USB presence records.
PCI coverage and the USB storage relationship remain unchanged from the [earlier capture](2026-11-10-laptop-hardware-catalog.md).

```text
CPU + boot memory -> readable platform descriptions
18 PCI functions -> 15 reciprocal ACPI associations
5 identified USB devices + 2 anonymous unsupported devices
USB stick -> 3 partitions -> 128 MiB ext2 -> /disk
```

## Scope and impact

This record documents one physical-device capture set; it changes no executable behavior.
The user saved five utility outputs under `/disk/hw` and returned the USB stick.
The output matches the `be43b91` presentation profile, but the files do not contain a build identifier.
The record does not establish the exact kernel or utility binary revisions.

| Requirement area | Assessment |
| --- | --- |
| Scope | GOALS: Testability and observability. Record physical evidence without expanding implementation scope. |
| Boot/platform | ARCHITECTURE: Passive hardware reflection. Distinguish observed firmware descriptions from device-control capabilities. |
| Memory/isolation | Report boot-usable bytes without claiming free-memory or installed-module capacity. Ownership and allocation behavior do not change. |
| Concurrency | Immutable snapshot and event-lifecycle rules remain unchanged. This capture does not test hotplug or concurrent removal. |
| Interfaces | Hardware reflection ABI: Platform descriptions and firmware associations. Verify exposed values without changing layouts or contracts. |
| Runtime resources | Compare capture byte counts with the previous set. No physical timing, CPU-use, or allocator measurement was collected. |
| Validation/documents | Verify hashes, record counts, canonical relationships, filesystem state, and local document links. Obtain independent review. |

No source, build, dependency, mapping, scheduler, or syscall changes are included in this documentation update.
Kernel builds and runtime tests are not required for this update under AGENTS.md.
The complete local results for the unchanged implementation remain in the [presentation evidence](2026-11-10-hardware-presentation.md).

## Results

| Area | Previous capture | New capture |
| --- | --- | --- |
| PCI coverage | 18 functions | Same addresses, vendor IDs, states, and bridge relationships. |
| CPU | Eight logical processor records | Core i7-2670QM; family 6, model 42, stepping 7; eight logical processors remain enabled. |
| Memory | No description | `usable_bytes=17126330368`, displayed as 15.94 GiB; state is `observed`. |
| USB devices | Five identified devices | The same five identified devices plus two anonymous `unsupported_speed` records. |
| USB ports and interfaces | 16 controller ports; ten interface descriptors | Counts remain unchanged. Webcam alternate settings account for several interface descriptors. |
| Firmware | 102 ACPI records without associations | Same record count; 15 of 18 PCI functions have reciprocal ACPI references. |
| Names and formatting | Generic names and decimal identifiers | Readable vendor/model hints, hexadecimal IDs, USB versions, speeds, and capacities. |
| USB tree | Device-first presentation | Controller-first tree; each physical or logical record appears once. |
| Storage | One USB block device and three primary partitions | Same sizes and relationship chain through ext2 to `/disk`. |

The earlier Linux comparison reported 16 GiB of system memory.
The new boot-usable total is consistent with that result; it is not an installed-memory or current-free-memory measurement.
The `2.20GHz` text belongs to the processor brand string and does not measure current clock frequency.
Controller port counts do not describe the number of external laptop connectors.

### Unsupported USB presence

| Route | New observation |
| --- | --- |
| EHCI `00:1a.0`, root port 1, Intel hub port 1 | Anonymous device at 12 Mbit/s, with `state=unsupported_speed`. |
| EHCI `00:1d.0`, root port 1, Intel hub port 4 | Anonymous device at 1.5 Mbit/s, with `state=unsupported_speed`. |

These observations are consistent with the Bluetooth adapter and mouse missing from the earlier OS1 capture.
Their identities remain unproven because OS1 has not read their descriptors.
No vendor or product ID is fabricated for either device.
Presence reflection does not enable low/full-speed EHCI transfers or functional class drivers.

### Firmware associations

All 15 nonzero PCI firmware references resolve to ACPI records with matching reverse device references.
Verification uses the canonical ID scheme and IDs from the previous capture; new default summaries omit each record's own ID.
Examples include the GPU at `01:00.0`, Wi-Fi at `03:00.0`, xHCI at `04:00.0`, and Ethernet at `05:00.0`.
Both EHCI controllers also have matching firmware associations.

The host bridge at `00:00.0`, management interface at `00:16.0`, and GPU audio function at `01:00.1` remain unmatched.
Zero references identify missing verified associations; they do not identify a driver failure.
The record does not infer additional links from similar names.

### Storage and filesystem checks

The USB device reports 15472047104 bytes, displayed as 14.40 GiB.
Its partitions report 1474048, 33554432, and 134217728 bytes.
The third partition backs a 128 MiB ext2 filesystem mounted at `/disk`.
The local copy retains UUID `c3fa8ae0-056c-403d-9ef4-275fb906b012`, 4096-byte blocks, and 32768 blocks.

`e2fsck -fn` completed all five passes without errors.
`dumpe2fs -h` reports a clean filesystem.
These commands read the local partition copy and do not modify the stick.
The capture contains no dropped-record, incomplete-inspection, traversal-limit, or duplicate-reference markers.
This check does not establish sudden-power-loss durability or preserving-update behavior.

## Output measurements

Each row compares one saved output from each capture set.
These values measure presentation volume, not execution speed, CPU use, or memory consumption.
The captures use the same utility modes, with `-t` for the USB tree and default summaries elsewhere.

| Capture | Previous bytes | New bytes | Difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| `lshw.txt` | 29934 | 13684 | -16250 | -54.29% |
| `lspci.txt` | 2965 | 2507 | -458 | -15.45% |
| `lsusb.txt` | 2945 | 1726 | -1219 | -41.39% |
| `lsusb-t.txt` | 7011 | 3861 | -3150 | -44.93% |
| `lsblk.txt` | 1339 | 605 | -734 | -54.82% |

## Evidence and remaining acceptance

The local partition copy is `build/hardware-comparison-2026-11-10-retest/data.ext2`.
Extracted files are in its adjacent `capture/` directory.
`e2fsck.log` and `filesystem-header.log` retain the read-only filesystem checks.
The earlier Linux comparison uses its archived report; the original Linux files were unavailable during this retest review.

| Capture | Bytes | SHA-256 |
| --- | ---: | --- |
| `lsblk.txt` | 605 | `6c7f6f4c8ccf45c1a991a4ebf055e1fe75fac9122fbdd8c3f72ae25a483cf05e` |
| `lshw.txt` | 13684 | `d78502d58c6c8ab78cb5f9078c4855fde7b721e827613a1790b6fe346087ba44` |
| `lspci.txt` | 2507 | `c4390fee6a05eafabea853c6725cd2e96584d9805e0611511dd47f627815fdbb` |
| `lsusb-t.txt` | 3861 | `30296c5e2566214ac15c141ade6917ce007797fd2b6d33ba34fec00210a48f64` |
| `lsusb.txt` | 1726 | `71143d2208aa0c9c5fda8c7409b4f2f62fa9cb33113a7a23bd8bf7f6258ae495` |

This set validates laptop summary output, anonymous presence, names, and firmware-reference consistency.
It does not exercise interactive Tab navigation or every verbose platform field.
CPU address widths, individual memory categories, and boot table-presence flags need separate captures for physical acceptance.
Desktop acceptance of the new catalog remains pending.
Runtime hotplug, AHCI storage, functional input services, and broader device support remain planned work.

The preceding comparison received independent read-only review with no blocking contradictions.
The separate merge review found no source defect or merge blocker in the complete PR and this documentation update.
It found one incorrect alias count in the ABI document; the corrected count is six.
The complete ASD-STE100 standard and dictionary were unavailable; full language compliance remains unverified.
