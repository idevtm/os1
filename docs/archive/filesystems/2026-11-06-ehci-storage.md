# EHCI storage and high-speed hubs

This plan adds USB 2 high-speed storage through EHCI, including high-speed hubs present during boot.
EHCI storage passes host and BIOS/UEFI QEMU checks.
The subsequent [Asus hardware test](2026-11-06-usb-hardware-validation.md) confirms one hub path and user-reported reboot persistence.
Pending-hardware statements below preserve the earlier implementation-review state.

```text
File / Directory -> ext2 -> BlockDevice -> SCSI / Bulk-Only Transport
                                               |                 |
                                              xHCI              EHCI
                                                                 |
                                                root port -> high-speed hub -> storage
```

## Plan and acceptance criteria

1. Add PCI EHCI discovery, firmware ownership handoff, reset, and bounded DMA schedules.
2. Implement control and bulk transfers with explicit data toggles and schedule retirement.
3. Enumerate high-speed hubs, power their ports, reset attached devices, and traverse bounded hub trees.
4. Reuse the existing mass-storage protocol and publish block devices for `/disk` mounting.
5. Test descriptor contracts, hub requests, errors, persistence, disk execution, and controller coexistence.
6. Review the complete isolated diff independently before completion.

The initial profile permits four controllers, sixteen addressed devices per controller, and five hub levels.
Hub port counts are bounded at fifteen. Configuration descriptors are bounded at 4096 bytes.
Transfers contain at most 4096 payload bytes. One request owns each controller schedule at a time.
Polling services TLB shootdown requests. It does not hold a spinlock across hardware waits.
A failed schedule stop retires the controller and retains DMA memory until reboot.
Published device identities are never recycled. Disconnect or reconnect changes invalidate old storage access.

Low/full-speed transfers, split transactions, UHCI/OHCI, periodic interrupt endpoints, and hot-plug mounting remain outside this profile.
High-speed devices behind high-speed hubs do not require split transactions.
Actual laptop hub topology is not yet captured; the implementation must log root ports, hubs, and storage addresses.

## Requirement impact

| Area | Assessment |
| --- | --- |
| Scope | Extend the kernel driver boundary in GOALS hardware support and Milestone D. Reuse existing storage protocols. |
| Boot/platform | Register EHCI on both boot paths. Claim PCI BARs, handle firmware ownership, and leave boot image layouts unchanged. |
| Memory/isolation | Driver-owned bounded DMA buffers, checked 32-bit addresses, allocation cleanup before publication, quarantine after uncertain retirement. |
| Concurrency | Serialize controller requests with an admission guard; poll bounded waits and service TLB shootdowns. |
| Interfaces | Native API, descriptors, syscalls, ext2 format, and constructor metadata remain unchanged. |
| Resources | Measure kernel size, DMA allocation, and comparable QEMU boot/storage workloads against the saved baseline. |
| Validation | Host tests, BIOS/UEFI four-CPU QEMU persistence and failures, xHCI regression checks, and subsequent physical qualification. |

The Native API index distinguishes the implemented File/Directory ABI from future public driver-service proposals.
This work implements a kernel driver, not a new native interface.

## Sources and validation limits

- [EHCI specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf), register and schedule definitions.
- [QEMU USB documentation](https://www.qemu.org/docs/master/system/devices/usb.html), EHCI test configuration.
- [QEMU hub implementation](https://github.com/qemu/qemu/blob/master/hw/usb/dev-hub.c), current emulated hub limitations.

QEMU's standard `usb-hub` emulates a full-speed hub, not the high-speed hub needed for this profile.
High-speed hub request sequencing therefore needs host contract tests and physical qualification; direct storage uses real QEMU EHCI emulation.
The baseline preserves all prior uncommitted USB tooling changes in `/tmp/os1-ehci-baseline`.
No physical USB writes are part of agent validation.

## Review corrections and fault-test limits

The first independent review found two transition errors.
Schedule disable did not wait for an outstanding enable acknowledgment.
Hub enumeration continued after a reset timeout, allowing a late reset to expose a second address-zero device.
The corrections enforce both schedule handshakes and abort unresolved hub resets.
Regression tests cover delayed schedule acknowledgment, stuck transitions, and late hub reset completion without sibling enumeration.

The original EHCI delayed-flush experiment did not reach the shell.
Diagnostics proved that the guest exhausted its transfer wait before the stall.
QEMU then blocked while cancelling the held SCSI request during asynchronous schedule shutdown.
Its monitor also stopped responding, preventing the test from releasing the request after the deadline marker.
This experiment is retained in `build/ehci-evidence/ehci-stall-diagnosis.log` and `ehci-delayed-release.log`.
It is not counted as a passing late-completion test.

The EHCI failure test instead stops the emulated controller through qtest MMIO access.
The next request must time out while enabling the schedule, quarantine DMA, and leave the shell responsive.
Restoring the emulated run bit must not restore access through the retired storage object.
The existing xHCI delayed-flush test remains unchanged.
Physical late completion and high-speed hub behavior remain unmeasured.

## Validation and resource measurements

The scoped host run passed 17 EHCI, hub, and existing BOT tests.
The complete host suite passed 407 tests in 8.08 seconds.
Six EHCI QEMU checks passed in 121.26 seconds with four virtual CPUs and 128 MiB RAM.
They cover BIOS/UEFI persistence, disk execution, combined USB-only boot, preserving updates, removal, reconnect rejection, read-only media, and controller failure.
The complete root CTest regression suite passed 42 tests in 299.45 seconds.
EHCI failure checks also attach an xHCI keyboard and inject keyboard input during EHCI disk I/O.
This checks simultaneous controller binding, USB block-list fallback, and HID delivery across controller types.

Commands:

```sh
cmake --preset default
cmake --build build --target os1_image os1_bios_image os1_usb_image
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
ctest --test-dir build -R '^os1_smoke_ehci_' --output-on-failure
ctest --test-dir build --output-on-failure
```

QEMU monitor and qtest sockets require sandbox escalation on this host.
The initial unprivileged failure run could not bind its local monitor socket; it did not exercise the guest.
Python syntax, local document links, scoped clang-format, and whitespace checks passed.
The complete ASD-STE100 dictionary was unavailable; full dictionary compliance was not verified.

Baseline: saved working tree atop `22e2865`, including prior uncommitted USB image and authentication fixes.
Candidate: the same tree plus the isolated EHCI change.
Environment: macOS 26.6.2 ARM64, GCC cross compiler 11.2.0, QEMU 11.1.3 with software emulation.
Build: kernel size optimization, `OS1_KMEM_DEBUG=ON`, `OS1_QEMU_DEBUGCON=ON`, and `OS1_BOOT_DIAGNOSTICS=OFF`.
Timing workload: BIOS, q35, four virtual CPUs, 128 MiB RAM, and a fresh 32-MiB ext2 partition.
Each repetition boots twice, runs diskutil seed/verify, writes and reads a shell file, and synchronizes storage.
Each configuration has three repetitions. The table uses medians in seconds.

| Phase | Baseline xHCI | Candidate xHCI | Absolute change | Relative change | Candidate EHCI |
| --- | ---: | ---: | ---: | ---: | ---: |
| write boot | 0.548393 | 0.562200 | +0.013807 | +2.52% | 0.737197 |
| write workload | 0.202329 | 0.215614 | +0.013285 | +6.57% | 2.159736 |
| read boot | 0.553052 | 0.561578 | +0.008526 | +1.54% | 0.742788 |
| read workload | 0.191985 | 0.192279 | +0.000294 | +0.15% | 1.254893 |

EHCI uses a different controller and a conservative stop-before-reuse schedule; its measurements are a transport comparison, not a same-path regression.
EHCI write workload change against baseline xHCI: +1.957407 seconds (+967.44%).
EHCI read workload change against baseline xHCI: +1.062908 seconds (+553.64%).

No throughput gate applies to this initial hardware-support slice. QEMU timing does not establish physical USB performance.
Persistent endpoint queues and sleeping completion waits remain potential performance work after hardware qualification.

| Kernel resource | Baseline bytes | Candidate bytes | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| Text section | 327680 | 331776 | +4096 | +1.25% |
| Data section | 32768 | 32768 | 0 | 0% |
| BSS section | 131072 | 159744 | +28672 | +21.88% |
| Section total | 491520 | 524288 | +32768 | +6.67% |

The static EHCI controller pool occupies 26656 bytes within BSS, even when no EHCI controller binds.
Each bound controller allocates 8192 DMA payload bytes; four controllers require at most 32768 bytes plus allocator metadata.
EHCI DMA allocation was zero before this change, so its relative increase is undefined.
The total image payload increase remains within existing BIOS staging limits; both images build and boot successfully.
Peak host memory, CPU utilization, physical throughput, and physical hub timing remain unmeasured.
Raw logs and the timing script are in `build/ehci-evidence/`.

## Review disposition

The first review found the two transition defects described above. Both corrections have regression coverage.
The second independent review found no new blocking code defect.
It confirmed both corrections and requested explicit simultaneous-controller coverage and clearer fault-test attribution.
The tests now cover EHCI storage with an xHCI keyboard, and the USB guide identifies the xHCI-only delayed-flush test.
The third independent read-only review passed with no blocking findings.
It verified the coexistence transcripts, complete regression results, fault-test attribution, and prior finding dispositions.
The added BIOS coexistence check passed in 7.54 seconds; UEFI coexistence passed within the complete regression run.
