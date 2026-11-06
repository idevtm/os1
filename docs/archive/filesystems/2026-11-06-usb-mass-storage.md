# USB mass-storage implementation plan

This slice connects USB Bulk-Only Transport to the existing ext2 filesystem.
It must preserve the QEMU deployment workflow and fail safely when a device disappears.

```text
native File/Directory -> ext2 -> bounded MBR partition -> BlockDevice
                                                        -> virtio
                                                        -> USB BOT -> xHCI bulk rings
```

## Scope and requirements

Baseline: merged PR 30, `a0694c0`.
The first target is an attached-at-boot, direct-root-port xHCI device.
Select the mass-storage SCSI-transparent interface with Bulk-Only Transport, alternate setting zero, and LUN zero.
Support bounded read, write, and cache synchronization with 512-byte sectors.
Require a writable device and successful synchronization before publishing it.
Keep the current ext2 profile and filesystem authority rules.

A device that disconnects or fails a transport check becomes unavailable until reboot.
Do not reuse its DMA storage or reconnect old filesystem handles to replacement media.
Use bounded waits, serialize endpoint state, and protect shared IRQ/poll event consumption.
Implement status/tag/residue checks and bounded BOT reset recovery without replaying writes.

No physical USB media will be modified during implementation.
USB hubs, EHCI, UAS, multi-LUN selection, live remount, and physical partition-preserving deployment remain later work.
A public native BlockDevice service is not required for this internal driver slice.

## Impact assessment

| Area | Requirement and change |
| --- | --- |
| Scope | GOALS: Storage and filesystems, Real hardware enablement; add a runnable USB transport behind existing filesystem interfaces. |
| Boot/platform | ARCHITECTURE: driver binding and persistent storage; support BIOS and UEFI xHCI enumeration and boot-time mount selection. |
| Memory/isolation | Driver-owned DMA bounce storage; never expose caller memory to hardware. Quarantine failed transfers; check allocation failure and descriptor bounds. |
| Concurrency | Serialize BOT commands and ring publication; synchronize IRQ/poll event consumption and disconnect state. Service TLB work during polling. |
| Interfaces | Native API index and implemented ext2 profile stay authoritative. Internal block reads/writes/Flush gain a USB implementation; no new syscall ABI. |
| Resources | Measure kernel/initrd sizes, DMA allocation, and matching virtio/USB persistence workloads. BIOS staging capacity can require a bounded layout adjustment. |
| Validation | Host protocol and descriptor tests, malformed/stalled/short transfers, read-only and unsupported devices, disconnect tests, both firmware paths, HID coexistence, and independent review. |

## Sequence

1. Implement a testable BOT/SCSI protocol layer with explicit transport callbacks.
2. Add bounded xHCI bulk endpoints, safe completion matching, and failure lifetime handling.
3. Publish USB block devices and select a supported persistent partition at boot.
4. Add USB-storage run targets and persistence/deployment/removal tests.
5. Run host tests and the full relevant QEMU matrix; compare the baseline.
6. Update current guides and roadmap, obtain independent reviews, and open a new PR.

## Acceptance

Boot from BIOS and UEFI with the data image attached through QEMU USB storage.
Execute a utility from disk, write and synchronize files, and verify them after an independent reboot.
Deploy a new program version offline and verify the existing files remain unchanged.
Reject malformed status, unsupported geometry, and missing durability support.
Removal must produce bounded I/O failure while the shell remains usable.
A reconnected device must not restore old handles.
HID keyboard input must continue with storage on the same controller.
Physical-controller qualification remains explicitly unmeasured until a hardware test.

## References

- [USB-IF Bulk-Only Transport 1.0](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf)
- [Intel xHCI specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf)
- [QEMU USB devices](https://www.qemu.org/docs/master/system/devices/usb.html)
- [Storage roadmap](../../native-api/roadmap.md#milestone-3-persistent-usb-data)

## Implementation result

The kernel now publishes bounded USB block devices behind the existing filesystem API.
BIOS and UEFI tests cover disk execution, persistent files, and offline deployment preservation.
The supported device profile and limitations are in the [current USB storage guide](../../hardware/usb-storage.md).
No syscall, native interface record, `BootInfo`, or disk-layout ABI changes were required.
The BIOS staging bounds and initrd slot remain unchanged.

## Validation

```sh
cmake --build build --target os1_image os1_bios_image -j4
cmake --build build-host-tests -j4
ctest --test-dir build-host-tests --output-on-failure
ctest --test-dir build --output-on-failure
ctest --test-dir build -V -R '^os1_smoke_(ext2|usb_storage)_'
```

Host tests: 398 passed. Logs: `/tmp/os1-usb-host-all.log`.
The USB tests use independent guest processes for reboot persistence and program deployment.
The persistence tests run `e2fsck -fn` on extracted partitions before boot and after both guest runs.
QMP tests cover removal, replacement rejection, read-only rejection, and USB 2 high-speed operation.
They inject USB keyboard input while the disk utility runs on a four-CPU guest.
The test verifies file contents separately because echoed keys can interleave with utility output.

The initialization-timeout test uses QEMU `blkdebug` to suspend a cache flush before block-device publication.
The guest reports a transfer timeout and DMA quarantine, then reaches the shell.
The test resumes the delayed completion and checks continued shell operation.
Host transport mocks cover malformed CSW records, short transfers, stall recovery, and write-failure retirement.
Physical endpoint recovery and controller behavior remain unmeasured.

## Review dispositions

Independent read-only reviewers inspected the BOT protocol and xHCI integration.
The first review found three introduced defects and one pre-existing hardware issue.

| Finding | Correction and evidence |
| --- | --- |
| DMA released after uncertain initialization completion | Retain every DMA-visible device allocation until reboot; injected initialization timeout and late completion test. |
| AP polling calls BSP-only console input | Queue decoded characters; dispatch on the BSP timer outside the controller lock; concurrent HID/storage smoke. |
| Command timeout leaves interrupt sources enabled | Mask controller and interrupter enables under the event lock; MMIO regression test. |
| PORTSC write can disable an enabled USB 2 port | Preserve only ordinary writable fields; test command-bit exclusion and boot with USB 3 ports disabled. |

A second review confirmed the code corrections and requested the initialization-timeout regression.
That test now exercises the real xHCI path, including delayed completion after quarantine.
The third xHCI review accepted the regression test and reported no blocking findings.
The final complete matrix passed all 32 checks in 116.50 seconds.
Log: `/tmp/os1-usb-final-tests.log`.
Both BIOS and UEFI include the delayed initialization completion test.
The final protocol and document review is recorded with the pull request.

The complete ASD-STE100 dictionary was unavailable during prose review.
This report applies the repository sentence, structure, and terminology rules without claiming full dictionary validation.


## Resource and runtime comparison

Baseline: merged PR 30, `a0694c0`. Candidate: this branch's USB implementation.
Both builds use GCC 11.2.0, Debug, `OS1_KMEM_DEBUG=ON`, and `OS1_QEMU_DEBUGCON=ON`.
The host runs macOS 26.6.2 on Apple arm64. Host CPU model retrieval was unavailable in the sandbox.
QEMU 11.1.3 uses TCG, `q35`, four virtual CPUs, and 128 MiB RAM.
UEFI uses Homebrew `edk2-x86_64-code.fd` and a fresh copy of `edk2-i386-vars.fd`.
BIOS uses the raw boot image and QEMU firmware.

The workload seeds a 128 KiB deterministic file, creates and appends a shell file, then synchronizes.
A separate guest verifies those files and immutable executable handling.
Each row totals the write and read guest phases; each configuration ran once for this comparison.
These elapsed host measurements include guest scheduling and emulation overhead. They are not device throughput benchmarks.
Logs: `/tmp/os1-usb-baseline/persistence.log` and `/tmp/os1-usb-measurements.log`.

| Candidate transport | Firmware / measurement | Baseline virtio, seconds | Candidate, seconds | Absolute change, seconds | Relative change |
| --- | --- | ---: | ---: | ---: | ---: |
| virtio | BIOS boot total | 0.936480 | 0.918848 | -0.017632 | -1.88% |
| virtio | BIOS workload total | 0.413930 | 0.424817 | +0.010887 | +2.63% |
| virtio | UEFI boot total | 3.776735 | 3.883949 | +0.107214 | +2.84% |
| virtio | UEFI workload total | 0.362115 | 0.373831 | +0.011716 | +3.24% |
| USB | BIOS boot total | 0.936480 | 1.092760 | +0.156280 | +16.69% |
| USB | BIOS workload total | 0.413930 | 0.389571 | -0.024359 | -5.88% |
| USB | UEFI boot total | 3.776735 | 4.638429 | +0.861694 | +22.82% |
| USB | UEFI workload total | 0.362115 | 0.357522 | -0.004593 | -1.27% |

USB adds enumeration work and an emulated controller. UEFI USB boot takes about 0.86 seconds more across the two boots.
Workload changes are small in this single sample; no speed improvement is claimed.
All configurations meet functional persistence and consistency checks. No numeric latency gate exists for this milestone.

| Artifact or resource | Baseline bytes | Candidate bytes | Change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| UEFI `kernel.elf` file | 487128 | 489128 | +2000 | +0.41% |
| BIOS kernel file | 365056 | 365056 | 0 | 0% |
| Initrd file | 191488 | 191488 | 0 | 0% |
| Kernel text + data + BSS, from `size` | 479232 | 491520 | +12288 | +2.56% |
| USB storage device DMA, allocation accounting | 0 | 36864 | +36864 | Undefined; zero baseline |

Kernel file measurements use `stat`; resident section totals use `x86_64-elf-size`.
Lazy controller construction moves zero-filled state to BSS and avoids increasing the BIOS staging image.
Each USB storage device uses nine page-rounded allocations: contexts, control/HID buffers and rings, two bulk rings, and one bounce buffer.
This first implementation retains unused HID allocations on a storage device for a common bounded allocation path.
The xHCI controller itself still uses four pages plus firmware-reported scratchpads; a HID device uses six pages.
The single BOT command uses at most 4096 data bytes plus a 31-byte CBW and 13-byte CSW.
The driver copies payload bytes through its DMA buffer. This is separate from native IPC Buffer ownership transfer.
The backing image size and ext2 layout are unchanged. Failure quarantine retains the bounded allocations until reboot.
