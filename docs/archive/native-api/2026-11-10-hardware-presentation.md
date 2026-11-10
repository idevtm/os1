# Hardware presentation and unsupported devices — 2026-11-10

This change improves hardware summaries and preserves unsupported connected USB devices in the boot catalog.
The [laptop comparison](2026-11-10-laptop-hardware-catalog.md) supplies the hardware evidence and limitations.

```text
existing status reads -> anonymous presence records -> context and utility summaries
known IDs -> hexadecimal labels; missing IDs -> unknown; storage bytes -> binary units
```

## Scope and impact

The [plan and impact assessment](../../native-api/hardware-catalog-plan.md#laptop-comparison-and-presentation-follow-up--2026-11-10) define this change.
The baseline is `edd3db4ee0cf165aee0b19bfb30010d71cf08a88` on PR 49.
The update preserves hardware query layouts, version 1 collections, and version 2 integer fields.
PCI name text becomes more descriptive in both versions.
USB and storage capture remains bounded to 256 records and freezes before user execution.
Three additional fixed records hold platform descriptions.
No device-control authority, hotplug support, new class drivers, or low/full-speed transfers are added.

## Implementation

Default utility summaries select fields by record type.
PCI output includes conventional addresses, vendor/device IDs, class, state, and driver.
USB output includes known identity, USB version, speed, port, and interface details.
Storage output includes exact byte counts with binary-unit labels.
Verbose output retains all available scalar fields.

USB trees start at controllers and follow physical and logical relationships.
Each canonical record prints once; records with unknown parents remain visible.
Recursive traversal remains bounded to 16 levels and 4096 records.
The renderer keeps its large buffers outside recursive traversal frames.
This limits stack growth when the tree contains several hub levels.

The hub callback receives only stable connected low/full-speed children after reset.
Capture records their speed, parent, and port without assigning addresses or requesting descriptors.
The existing isolation sequence then disables each unsupported child.
Connected unusable root ports retain anonymous records without an assumed speed.
These observations cannot distinguish every reset failure from an unsupported transport.

## Checks and failure evidence

The affected host checks passed 28 tests in 0.83 seconds.
They cover malformed and changing status, absent identity fields, capture freeze, formatting, and unchanged hub transfer counts.
The kernel and user program build passed.
Four initial virtio/xHCI catalog smokes passed; the two EHCI fixtures failed before boot.

| Evidence under `build/hardware-presentation/` | Result |
| --- | --- |
| `catalog-tests-1.log` | QEMU rejected a full-speed hub on the generic high-speed EHCI bus. |
| `catalog-ehci-2.log` | The generic EHCI model rejected companion-controller attachment. |
| `catalog-ehci-3.log` | The ICH9 EHCI/companion fixture passed BIOS and UEFI, including anonymous-device assertions. |
| `host-build-1.log`, `host-tests-1.log` | Rebuilt host tests passed. |
| `build-1.log` | Kernel and utility build passed. |

The fixture uses QEMU's [documented USB companion model](https://www.qemu.org/docs/master/system/devices/usb.html).
This is a test configuration correction; production controller behavior was not changed to satisfy QEMU.
The original failure logs remain available.
The initial presentation scope passed 609 host tests, 86 normal-image checks, and three diagnostic checks.
The final expanded run passed 615 host tests, 86 normal-image checks, and three diagnostic checks.
`full-local-3.log` records respective times of 20.15, 387.71, and 5.85 seconds.
No checks were disabled or skipped.

## Measurements

The comparison uses saved `edd3db4` artifacts and the final source tree for this follow-up.
Each revision boots its own kernel, initrd, and program disk.
The host is an Apple M4 Max with 16 logical CPUs and 64 GiB RAM, running macOS 26.6.2.
QEMU 11.1.3 uses TCG, `q35`, four guest CPUs, 128 MiB RAM, xHCI storage, and a USB keyboard.
The compiler is `x86_64-elf-g++` 11.2.0.
The build uses Debug, `-Os`, `OS1_KMEM_DEBUG=ON`, and `OS1_QEMU_DEBUGCON=ON`; both diagnostic options are off.

Run three alternating baseline/candidate repetitions per firmware path, with no concurrent QEMU tests.
The workload runs `lspci`, `lsusb -t`, `lsblk`, and `lshw` between `sys` and `kmem` observations.
Timing includes those fixed observation commands.
The following values are medians; differences compare candidate against baseline.

| Firmware | Metric | Baseline | Candidate | Difference | Relative difference |
| --- | --- | ---: | ---: | ---: | ---: |
| BIOS | Boot seconds | 0.553881 | 0.555890 | +0.002009 | +0.36% |
| BIOS | Workload seconds | 3.701903 | 2.188540 | -1.513363 | -40.88% |
| BIOS | QEMU CPU seconds | 4.502830 | 2.891906 | -1.610924 | -35.78% |
| UEFI | Boot seconds | 2.394053 | 2.391760 | -0.002293 | -0.10% |
| UEFI | Workload seconds | 2.028254 | 0.926608 | -1.101646 | -54.31% |
| UEFI | QEMU CPU seconds | 4.567937 | 3.401409 | -1.166528 | -25.54% |

Three samples do not establish a general performance guarantee.
Boot changes below 3 ms are too small to distinguish from run variation here.
The shorter summaries reduce captured serial output and observed workload time.

| Command response | Baseline bytes | Candidate bytes | Difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| `lspci` | 1204 | 1057 | -147 | -12.21% |
| `lsusb -t` | 2958 | 1559 | -1399 | -47.30% |
| `lsblk` | 922 | 442 | -480 | -52.06% |
| `lshw` | 11719 | 5188 | -6531 | -55.73% |

Response sizes include the command echo and prompt. All repetitions produced these same sizes.

BIOS free frames before the workload changed from 31617 to 31585: 32 fewer frames, or 128 KiB.
This is a 0.101% decrease in free frames.
UEFI free frames changed from 19147 to 19114: 33 fewer frames, or 132 KiB, a 0.172% decrease.
The reserved kernel window accounts for 128 KiB of this cost.
All samples reported zero allocator failures.
Before the workload, both revisions reported 13 caches, 171 slab pages, and 2355 live allocator objects.
After-workload frame counts varied by at most one frame in either revision.
The report retains these variations; it does not claim a leak-free lifecycle from one workload.

| Artifact | Baseline bytes | Candidate bytes | Difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| `kernel.elf` | 586440 | 593256 | +6816 | +1.16% |
| `initrd.cpio` | 7680 | 7680 | +0 | +0.00% |
| `os1.raw` | 1474560 | 1474560 | +0 | +0.00% |
| `os1.iso` | 4321280 | 4327424 | +6144 | +0.14% |
| `program-test-disk.raw` | 34603008 | 34603008 | +0 | +0.00% |
| `lspci.elf` | 4248 | 8128 | +3880 | +91.34% |
| `lsusb.elf` | 4248 | 8160 | +3912 | +92.09% |
| `lsblk.elf` | 4248 | 8128 | +3880 | +91.34% |
| `lshw.elf` | 4280 | 8160 | +3880 | +90.65% |

The 256-record catalog remains 151616 bytes. Three fixed platform records and firmware references add 2776 bytes of static data.
The kernel window change accommodates these records and additional code while preserving the 16 KiB post-image reserve.
No fixed performance threshold applies to this slice; correctness, bounds, and passive discovery requirements passed.
Physical timing remains unmeasured. The later laptop retest validates summary descriptions without timing claims.

Raw samples and hashes are in `build/hardware-presentation/measurements/measurements.json`.
Individual serial logs and timing profiles are beside that file.
`environment.json` records host settings and firmware hashes in the parent directory.
`measurements-2.log` records all 12 successful samples.
An initial `python3 -I` invocation could not import the local harness; no guest started in that attempt.
`measurements.log` preserves that invocation error.
The documented invocation below completed successfully.

```sh
python3 -B tests/scripts/measure_hardware_catalog.py \
  --baseline build/hardware-presentation/baseline \
  --candidate build/hardware-presentation/candidate \
  --output build/hardware-presentation/measurements \
  --qemu /opt/homebrew/bin/qemu-system-x86_64 \
  --code /opt/homebrew/share/qemu/edk2-x86_64-code.fd \
  --vars /opt/homebrew/share/qemu/edk2-i386-vars.fd \
  --workload utilities --transport usb
```

### Artifact fingerprints

| Revision | Artifact | SHA-256 |
| --- | --- | --- |
| baseline | `kernel.elf` | `5dbde298649bd3f5e699484f7b103674d6468a3e2f855fecaa3869bc0d00c674` |
| baseline | `initrd.cpio` | `25cc31284679a78dc5dd07935f257d32f1e7313f54f50fe8899836d8a0650dce` |
| baseline | `os1.raw` | `8eca1713c53cf955b87aeab79fc0e461592ba245e43609d6dbbaa9fac5cd5e6d` |
| baseline | `os1.iso` | `265ac0e8af36f6c5ceb6323e6ecef030d5a77bc4d675d67dd1a7c5c10d18299c` |
| baseline | `program-test-disk.raw` | `5e59a8f7a8842fb7d6a86413d83bff04195b8cb6da3b8fba96640bd5ae82cf79` |
| candidate | `kernel.elf` | `e3ac80ad16bb36a622412f407f2665ee5831a35567600e829918f8749c32a505` |
| candidate | `initrd.cpio` | `9369741e712dafb5a2c34eb2a3f09d29b206c8cd44e88b6442292f8179c3ee4f` |
| candidate | `os1.raw` | `d18c461adfcefcab4bff279e52c598e9d2e54f0f687535b9083497c722ed5785` |
| candidate | `os1.iso` | `f837a8d5c78dde4aeb12f3a36273ab0c5aab9886c8d3ce7f4184478237eb83fb` |
| candidate | `program-test-disk.raw` | `72f99d78d1a96ead8a5ec1b2f158569b701ec62e65de52e494c2b2c501e9dd3f` |
| shared firmware | `edk2-x86_64-code.fd` | `33090cc07675baa5190d9f1e84bf5176b33bcbfa9bacac522961150cdb6dbb2a` |
| shared firmware | `edk2-i386-vars.fd` | `5d2ac383371b408398accee7ec27c8c09ea5b74a0de0ceea6513388b15be5d1e` |
| shared firmware | `bios-256k.bin` | `ae6f6aa973aaccc143f57aa960fb035fd9de4daee4ad0cd713322f8c259e7650` |

## Review and physical acceptance

Independent source review found no concrete defects in the initial implementation diff.
The reviewer checked formatting, paging, traversal bounds, capture ordering, unknown fields, and unchanged transfer sequences.
A separate platform review found no concrete source defects.
It covered CPU leaves, memory bounds, firmware matching, ABI gating, capture ordering, and kernel layout.
A further review confirmed that exact `Intel Bridge` assertions preserve completion and field-value coverage.
Final evidence review verified the full-suite totals, timing arithmetic, artifact hashes, and laptop capture hashes.
Three documentation findings clarified name compatibility, fixed platform-record capacity, and firmware table-presence flags.
A separate correction pass verified all three changes; no findings remain open.
Changed Python syntax, local document targets, whitespace, and secret checks passed.
The captured hardware data contains identifiers and evidence hashes, not credentials.

The laptop captures describe the earlier catalog implementation.
The later [laptop retest](2026-11-10-laptop-hardware-catalog-retest.md) confirms presentation and unsupported-device records on physical hardware.
Desktop acceptance and complete verbose platform-field coverage remain pending.
On the laptop, inspect anonymous 12 Mbit/s and 1.5 Mbit/s records where those devices remain connected.
Do not infer vendor or product identity from an anonymous record alone.
The complete ASD-STE100 standard and dictionary were unavailable; full compliance remains unverified.

## Platform extension

The user added CPU identity, memory information, readable names, and discoverable firmware associations to this task.
The extension adds `os.system.platform`, vendor/class names, selected USB model names, and bidirectional ACPI/PCI relationships.
The [ABI](../../native-api/abi/hardware-reflection-abi.md#platform-descriptions-and-firmware-associations) defines limitations and field semantics.

The first expanded build failed its reserved execution-window check.
The required range reached `0x1A2000`, including the existing 16 KiB post-image reserve.
`build-2.log` preserves this failure.
The window now ends at `0x1C0000`, and the bitmap starts at that generated boundary.
This costs 128 KiB of reserved physical memory while retaining staging, disk-slot, and post-image checks.

The first host rebuild also exposed an include-order change from automatic formatting.
Restore ACPICA's required include order and exclude unrelated formatting changes.
`host-build-2.log` retains the failure; `host-build-3.log` records the corrected build.
The expanded affected host run passed 34 tests in 1.12 seconds.
This includes CPU leaf bounds, memory-map rejection, address matching, and ACPICA root-location defaults.

CPU decoding follows the Intel architectural [CPUID definition](https://www.intel.com/content/www/us/en/support/articles/000006831/processors/intel-processor.html).
The complete CPUID leaf definitions are in the Intel Software Developer's Manual.
The laptop retest confirms summary descriptions; verbose-field and desktop acceptance remain pending.

The expanded full run exposed stale context-completion assertions for the old `PCI function` label.
The actual QEMU host-bridge name is now `Intel Bridge`, as required by the readable-name change.
Update both exact string assertions without reducing field-value or completion coverage.
`full-local-2.log` preserves all four stale-name failures.
The complete repeat, `full-local-3.log`, passed after this correction.
