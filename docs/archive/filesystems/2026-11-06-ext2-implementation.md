# Ext2 persistence implementation plan

This milestone adds persistent QEMU storage and executes a utility from disk.
PR 28 was merged as `46accf8` before this work.

```text
Shell / native File and Directory objects
  -> filesystem operations -> ext2 -> bounded partition -> virtio block -> raw image
Executable file -> stable snapshot -> existing ELF validator -> user process
```

## Accepted scope

- Mount a bounded ext2 profile at `/disk` through the existing native filesystem interfaces.
- Preserve the immutable initrd and volatile `/tmp` backends.
- Implement block Flush and explicit filesystem synchronization.
- Create a persistent QEMU image only when requested or absent. Never recreate it during ordinary builds.
- Install a utility only on disk. Execute it through the existing launch path.
- Test writes across two independent QEMU boots with the same backing image.
- Keep USB mass storage, dynamic kernel modules, and journaling outside this milestone.

## Sequence

1. Add partition bounds, virtio flush, and separation from scratch-disk smoke writes.
2. Add filesystem dispatch and a bounded ext2 implementation with validated on-disk structures.
3. Add stable executable snapshots, native sync, and the disk-only utility.
4. Add image creation and QEMU targets to `os1.sh` through the target menu.
5. Run host tests, both firmware paths, persistence checks, resource measurements, and independent review.
6. Record results, commit, push, and open a separate PR. Do not merge that PR.

## Impact assessment

| Area | Impact and validation |
| --- | --- |
| Scope | Complete QEMU persistence slice; no physical disk writes. |
| Boot/platform | Preserve both boot paths and firmware ceilings. Measure kernel and initrd sizes. |
| Memory/isolation | Validate disk offsets, features, allocation, capability rights, and executable snapshots. |
| Concurrency | Serialize filesystem mutations without holding spinlocks during disk waits. Review DMA completion lifetime. |
| Interfaces | Reuse File/Directory IPC and descriptor cursors. Add an explicit sync member. |
| Runtime resources | Record baseline and final image sizes and comparable QEMU timings. Bound filesystem buffers and files. |
| Validation/docs | Test malformed disks, I/O failures, disk-full behavior, reboot persistence, and disk execution. Update ABI and workflow documents. |

## Durability contract

Ext2 has no journal. Successful synchronization persists preceding completed mutations through a device flush.
Interrupted writes can require offline `e2fsck`; this milestone does not promise power-loss atomicity.
Reject unsupported disk features and inconsistent geometry. Never format a disk after a mount failure.
The image builder uses an explicit feature profile that the kernel validates.

## Evidence

Implementation and code review are complete on `codex/ext2-persistence`.
The second independent pass found no remaining code blockers. Both reviewers verified the final documentation corrections. No unresolved findings remain.
The new PR remains unmerged.

### Checks

| Check | Result |
| --- | --- |
| `cmake --build build -j 4` | Passed; both boot images and layout checks built |
| `ctest --test-dir build-host-tests --output-on-failure --no-tests=error` | 387/387 passed |
| `ctest --test-dir build --output-on-failure --no-tests=error` | 19 passed initially; two local-socket tests were blocked by the sandbox |
| Monitor and no-UART tests outside the sandbox | Both passed; all 21 root cases are covered |
| `ctest --test-dir build -R '^os1_smoke_ext2_' --repeat until-fail:3 -V` | Three passes per firmware path; twelve independent guest boots |
| `e2fsck -fn` | Clean fresh, written, and rebooted partitions on each persistence run |
| Documentation links and `git diff --check` | Passed |

The persistence tests require the utility to be absent from `/bin`.
They run `/disk/diskutil`, verify a 128 KiB pattern after reboot, and verify shell-created file contents.
They also reject writes to the executable and execute it again afterward.
The storage-image contract checks preservation during ordinary builds, explicit reset, active-image locks, and reflective ELF preservation.

Host tests cover malformed features, geometry, allocation counts, reserved inode references, duplicate block ownership, and invalid directory records.
They also cover disk-full behavior, indirect blocks, sparse growth, truncation, failed writes and flushes, and retained-node removal.
Regression tests cover high PCI mappings, timeout-boundary completion, error payloads/cursors, sync authority, and sectionless executable metadata.

Logs: `/tmp/os1-ext2-host-final.log`, `/tmp/os1-ext2-full.log`, `/tmp/os1-ext2-repeat.log`, and `build/artifacts/smoke-ext2-*`.
The socket rerun logs are `/tmp/os1-ext2-monitor.log` and `/tmp/os1-ext2-no-uart.log`.
CI #170 passed all host tests, then detected a compiler-specific BIOS kernel overrun.
Its GCC 16.2 artifact required 369152 bytes, exceeding the initial 368640-byte slot by 512 bytes.
The corrected split reserves 372736 bytes for the kernel and 192512 bytes for initrd, preserving the same total span.
Explicit image targets now also depend on the disk-only utility; CI does not build the default ALL target.
The follow-up CI run validates both corrections.

### Measurements

Baseline: merged PR 28 content at `46accf8`, with artifacts built before the ext2 edits.
Final: the working implementation on this branch, before its first commit.
Host: macOS 26.6.2, arm64; cross compiler GCC 11.2.0; QEMU 11.1.3.
Kernel configuration enables `OS1_KMEM_DEBUG` and `OS1_QEMU_DEBUGCON`; kernel optimization is `-Os`.
The persistence guests use q35, four CPUs, 128 MiB RAM, and a 32 MiB ext2 partition.
UEFI uses Homebrew's `edk2-x86_64-code.fd` and a fresh copy of `edk2-i386-vars.fd`.
The BIOS path uses QEMU's default firmware. Guest acceleration is QEMU's default TCG on this arm64 host.

| Artifact | Baseline bytes | Final bytes | Difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| Kernel ELF | 471128 | 487128 | +16000 | +3.40% |
| Stripped BIOS kernel | 352768 | 365056 | +12288 | +3.48% |
| Initrd | 201728 | 191488 | -10240 | -5.08% |

The BIOS staging span remains unchanged: `[0x10000,0x9A000)`.
Its split changes to 364 KiB for the kernel and 188 KiB for initrd.
Local artifact slack is 7680 bytes and 1024 bytes respectively; compiler variation remains a CI concern.
The kernel's reserved physical window remains unchanged.

The unchanged BIOS interactive-file smoke took 2.40 seconds before and 2.19 seconds after this change.
These are single samples: -0.21 seconds (-8.75%). They do not establish a performance improvement.
The new persistence workload has no implemented baseline, so relative performance is undefined.

| Repeated persistence workload | Sample 1 seconds | Sample 2 seconds | Sample 3 seconds |
| --- | ---: | ---: | ---: |
| BIOS write commands, including sync | 0.215608 | 0.211680 | 0.213545 |
| BIOS verification commands | 0.193664 | 0.198256 | 0.195796 |
| UEFI write commands, including sync | 0.193625 | 0.191844 | 0.189676 |
| UEFI verification commands | 0.170117 | 0.171069 | 0.170555 |

These host wall-clock samples include command handling, IPC, disk access, and synchronization; they are not device throughput measurements.
The write workload creates the patterned file and a shell note. Verification reads both and checks executable write rejection.
The ext2 volume structure occupies 83048 bytes, plus namespace nodes and an executable snapshot when loading.
Snapshots are bounded by the 4243456-byte file limit. Filesystem contents are not preloaded into RAM.
Dedicated guest CPU utilization, high-contention latency, and physical hardware behavior remain unmeasured.
This phase validates the bounded QEMU slice; those measurements require separate workloads and hardware.

### Review and corrective work

The first independent filesystem review found two defects.
Growth could expose nonzero bytes beyond the old EOF in an imported block.
Ordinary directory entries could reference reserved inodes.
Both now have explicit rejection or zero-fill logic and regression tests.

The first independent driver/build review found two defects.
A completion could race with timeout reset, and the ELF packer lacked a build dependency.
The timeout path now drains and rechecks completion under the queue lock before disabling the device.
The initrd target now depends on the packer script.

UEFI validation separately exposed an existing mapping assumption.
User roots inherited only the first direct-map slot, excluding high PCI BAR mappings selected by OVMF.
User roots now inherit all supervisor-half entries, with user access cleared at the top level.
The private user half remains unchanged. A host regression and both firmware smokes validate the correction.

The initial compaction integration exposed a metadata-reader assumption about mandatory section tables.
The packer now preserves all `.os*` sections; valid sectionless executables report no metadata.
Malformed partial section tables still fail validation.

Full ASD-STE100 dictionary verification is unavailable; prose receives a manual review against repository rules.

The second pass identified stale block-completion prose and BIOS sector ranges in maintained documentation.
Those statements now match implemented Flush, polling, the new staging split, and persistent VFS support.
No physical hardware validation or full language-dictionary verification is claimed.

The CI layout correction passed an explicit local image build and all four interactive-file/ext2 firmware smokes.
A separate reviewer verified the corrected sector arithmetic, unchanged firmware ceiling, image dependencies, and archived-record links.
