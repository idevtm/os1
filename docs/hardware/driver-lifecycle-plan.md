# Shared driver lifecycle and disk module

This plan implements a shared PCI binding lifecycle and one functional disk-loaded driver.
The first module moves QEMU virtio-net behavior out of the linked kernel.

```text
built-in storage -> mount /disk -> match virtio PCI identity
  -> snapshot package -> verify trusted digest -> map ELF -> bind driver
  -> interrupt-driven packet transfer -> existing ARP acceptance check
```

## Scope and acceptance

1. Record the accepted [driver architecture](driver-architecture.md) and hardware priorities.
2. Route built-in and disk-loaded PCI bindings through the same lifecycle admission rules.
3. Reject duplicate binding, invalid transitions, and unproven resource retirement.
4. Load one bounded, freestanding ELF module from ext2 after storage startup.
5. Match hardware before reading its package; never activate modules during inspection.
6. Validate the package digest from the kernel's trusted build manifest.
7. Keep boot storage and rescue dependencies built in.
8. Preserve virtio-net packet completion, ARP validation, and observability checks.
9. Deploy the module through existing preserving image workflows.
10. Test absent, corrupt, incompatible, and successful package paths on both boot frontends.
11. Run the full local suite and independent adversarial review.

The initial profile has one fixed module address window and no dynamic symbol imports or relocations.
Explicit host function tables provide required kernel services.
Module code remains resident until reboot.
Automatic boot loading is the only loader entry; there is no user-accessible arbitrary module-load syscall.
Runtime USB hotplug, general dependency graphs, native network sessions, and live code unloading remain later slices.
The shared lifecycle must preserve these extension boundaries without claiming their implementation.

## Impact assessment

| Area | Requirement and effect |
| --- | --- |
| Scope | GOALS: Kernel model and Runnable vertical slices. Retain kernel execution with one actual disk-loaded driver. |
| Boot/platform | ARCHITECTURE: Phase 4 and kernel bring-up. Defer optional network binding until ext2 mounts; retain BIOS and UEFI rescue startup. |
| Memory/isolation | Bound file snapshots and module mappings. Validate ELF ranges and trusted hashes; enforce supervisor segment permissions. Retain uncertain DMA. |
| Concurrency | Serialize lifecycle admission without holding registry locks during driver callbacks. Separate IRQ completion from lifecycle ownership. |
| Interfaces | Native API index: hardware reflection remains passive. Add an internal exact-build module ABI; preserve existing public query layouts. |
| Resources | Compare boot time, kernel/module sizes, mapped pages, and allocator use against `10cb464`. Record loading costs separately from packet behavior. |
| Validation/documents | Add source-backed host tests and BIOS/UEFI module checks. Run `./scripts/check-local.sh`; retain first failures and independent review results. |

BootInfo, descriptor encoding, IPC payload layouts, and filesystem write semantics do not change.
The deployment manifest adds the module without removing unlisted files.
Reference captures remain evidence, not executable configuration.

## Validation and evidence

The full local suite and independent review pass.
The implementation uses the [initial module profile](driver-module-abi.md).
Baseline artifacts are retained in `build/driver-lifecycle/baseline/`.
`baseline-build.log` records the initial build from `10cb464` before source changes.
The Linux captures establish hardware targets, not OS1 support.
The complete ASD-STE100 standard and dictionary are unavailable; full language compliance remains unverified.

### Review and corrected failures

An independent agent reviewed the complete diff without editing files or executing tests.
The review found two defects: missing network binding events and silent binding-allocation failure.
Both defects have corrections and regression tests.
The correction review found no remaining blocking defects.
A proposed test-directory collision was retracted after the reviewer verified per-invocation test isolation.

Twenty focused host checks pass, including physical-page exhaustion during binding admission.
Both firmware module checks pass with absent, corrupt, incompatible, unneeded, and successful package cases.
The successful case validates packet exchange and the exported persistent binding event.
The observation ring can wrap during shell startup; persistent-log evidence avoids that retention limit.

Initial compiler errors and one VFS path error remain in `build/driver-lifecycle/`.
The loader now passes the root-relative path required by `vfs_resolve`.
The first full attempt encountered sandbox restrictions on QEMU control sockets.
Its logs remain in `build/driver-lifecycle/first-full/` and `full-local-1.log`.
That development run also observed an intermediate event-test failure before final artifact validation.
It is not final-source acceptance evidence.
The final full run uses permitted local sockets and retains its complete output separately.

Direct execution of every loader allocation-failure branch and physical DMA-quarantine behavior remains unmeasured.
Host allocation exhaustion checks shared binding admission; it does not establish complete loader fault coverage.
Physical acceptance of new hardware drivers remains outside this QEMU virtio-net slice.

### Final checks

The final source tree is based on `10cb464876619959bf5dc84c69cebb1a3613465e`.
The normal build identity is `e41197f22ea01483eae157025a9022647581cbd39c896cc21e67063f1137cfb7`.
The diagnostic build identity is `9dd41123c7c643d04b0f9cbb468b3a78f8d3225cb715f0d766ecbddc13945b39`.

| Check | Result | Duration |
| --- | --- | --- |
| `./scripts/check-local.sh`: host tests | 629 passed | 21.49 s |
| Same command: normal-image suite | 88 passed | 394.21 s |
| Same command: diagnostic images | 3 passed | 5.87 s |
| Focused admission and ELF checks | 20 passed | 0.67 s |
| Focused BIOS and UEFI module checks | Both passed | 15.12 s |

The full output is `build/driver-lifecycle/full-local-final.log`.
Per-test transcripts remain under `build/artifacts/test-runs/` and the diagnostic build's equivalent directory.
Python syntax checks pass for six affected scripts.
The staged whitespace check passes outside the unchanged raw reference captures.
Raw Linux output retains its original trailing spaces.
New local document links resolve.
Two missing historical `doc/shell-language/` links predate this change in the May 3 archived review.
No runtime checks were skipped in the final local command.
Source formatting used explicit owned-file paths and excluded dependencies and generated files.

### Baseline comparison

This comparison measures boot and the four hardware utilities with virtio-net present.
The [sample record](2026-11-10-driver-lifecycle-measurements.json) preserves artifact hashes, configuration, and all twelve runs.
Each firmware/revision combination ran three times, alternating baseline and candidate.
Each revision used its own boot and ext2 images.

The host was an Apple M4 Max with 64 GiB RAM and macOS 26.6.2.
QEMU 11.1.3 used TCG, q35, four virtual CPUs, 128 MiB RAM, and one guest at a time.
The compiler was GCC 11.2.0 for `x86_64-elf`; CMake was 4.4.5.
Both builds used Debug, kernel C++ `-Os`, kernel C `-O1`, `KMEM_DEBUG=ON`, and `QEMU_DEBUGCON=ON`.
Normal boot diagnostics were off; the new module used `-Os`.
The Limine frontend used its existing `-O0` setting.
The sample record identifies both OVMF files and their SHA-256 digests.

```sh
python3 -B tests/scripts/measure_hardware_catalog.py \
  --baseline build/driver-lifecycle/baseline \
  --candidate build/driver-lifecycle/candidate \
  --output build/driver-lifecycle/measurements \
  --qemu /opt/homebrew/bin/qemu-system-x86_64 \
  --code /opt/homebrew/share/qemu/edk2-x86_64-code.fd \
  --vars /opt/homebrew/share/qemu/edk2-i386-vars.fd \
  --workload utilities --driver-network
```

| Median measurement | Baseline | Candidate | Absolute change | Relative change |
| --- | --- | --- | --- | --- |
| BIOS boot | 0.472041 s | 0.473982 s | +0.001941 s | +0.41% |
| UEFI boot | 1.942407 s | 1.951379 s | +0.008972 s | +0.46% |
| BIOS utility workload | 1.857924 s | 1.850786 s | -0.007138 s | -0.38% |
| UEFI utility workload | 0.733261 s | 0.722295 s | -0.010966 s | -1.50% |
| BIOS child CPU time | 2.500635 s | 2.507240 s | +0.006605 s | +0.26% |
| UEFI child CPU time | 2.756223 s | 2.753752 s | -0.002471 s | -0.09% |
| BIOS allocated pages before workload | 1146 | 1151 | +5 pages / 20 KiB | +0.44% |
| UEFI allocated pages before workload | 5516 | 5522 | +6 pages / 24 KiB | +0.11% |
| Slab pages before workload | 171 | 172 | +1 page / 4 KiB | +0.58% |
| Linked kernel ELF | 593256 bytes | 595816 bytes | +2560 bytes | +0.43% |
| UEFI ISO | 4327424 bytes | 4329472 bytes | +2048 bytes | +0.05% |
| Ext2 allocated blocks | 136 | 144 | +8 blocks / 32 KiB | +5.88% |
| Ext2 allocated inodes | 47 | 53 | +6 | +12.77% |
| Separate module ELF | 0 bytes | 10280 bytes | +10280 bytes | Undefined |

The BIOS image remains 1474560 bytes; initrd remains 7680 bytes.
The fixed-size ext2 image remains 34603008 bytes.
The module maps three pages, below its 128 KiB address-window limit.
All samples report zero allocator failures and unchanged utility output sizes.
The loader and host adapter add kernel code while packet logic moves to disk.
These measurements establish a bounded cost; they do not establish a performance improvement or physical-hardware behavior.
Old versioned package retention remains a separate disk-space cost during preserving updates.
