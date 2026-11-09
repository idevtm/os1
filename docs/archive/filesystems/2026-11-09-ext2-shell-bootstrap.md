# Ext2 shell startup — 2026-11-09

This record documents the move of the normal shell and bundled resources to ext2.
Initrd retains a small supervisor and an independent rescue console.

```text
kernel -> immutable initrd -> /bin/init -> /disk/bin/sh
                                 |
                                 +-- launch failure -> /bin/rescue -> retry
```

## Accepted behavior

- Initrd contains `/bin/init` and `/bin/rescue`.
- Ext2 contains `/disk/bin/sh`, utilities, providers, and `/disk/etc/motd`.
- The kernel mounts available ext2 storage before it starts init.
- Init restarts the normal shell after its exit.
- A failed shell launch reports its status and starts rescue.
- Rescue provides bounded input, directory listing, file reading, USB snapshots, retry, synchronization, shutdown, and reboot.
- Rescue does not provide the guided shell, scripts, aliases, or storage repair.
- Retry attempts another shell launch. It does not enumerate hardware or mount storage again.
- The typed search path remains `/bin`, then `/disk/bin`.
- Existing images require explicit deployment. Deployment preserves unlisted files.
- Resource deployment installs missing defaults and preserves existing resource files.

## Requirement impact

| Area | Requirements and effects |
| --- | --- |
| Scope | GOALS: Kernel and userland separation. Keep a usable recovery path without duplicating the full shell. |
| Boot and platform | ARCHITECTURE: Build Outputs And Boot Artifacts. Both firmware paths require ext2 for the normal shell. BootInfo and firmware layouts remain unchanged. |
| Memory and isolation | Startup ABI: Namespace and authority. Existing grants and ELF validation apply. Rescue uses bounded buffers and closes opened descriptors. |
| Concurrency | Init waits for one foreground child. Existing process cleanup and shell restart behavior remain unchanged. No kernel locks change. |
| Interfaces | Program paths and deployment manifests change. Syscall records, native protocols, mount names, and typed context layouts remain unchanged. |
| Runtime resources | Measure archive size, program size, boot latency, and allocation observations against commit `4357621`. Disk reads replace shell reads from initrd. |
| Validation and documents | Update packaging, startup paths, deployment tests, and operating guides. Cover normal and failed startup under BIOS and UEFI. |

The Native API index identifies startup and filesystem ABI documents as implemented profiles.
This change does not implement deferred root switching, app bundles, credentials, or userspace drivers.

## Implementation and validation sequence

1. Save baseline images and record comparable boot measurements.
2. Implement init supervision and the rescue console.
3. Move normal programs and resources into the deployment manifest.
4. Update test disks and explicit executable paths.
5. Test absent storage, missing shell, invalid shell, retry, and normal shell restart.
6. Verify preserving deployment, resource retention, and reboot persistence.
7. Run host checks and the affected BIOS, UEFI, and USB tests.
8. Obtain an independent review of the complete change and evidence.

Physical-device startup remains unmeasured until the user tests the new image.
Technical prose receives manual review; the complete ASD-STE100 dictionary is unavailable locally.

## Implemented boundaries

Initrd contains two executable files. The default disk manifest contains 29 executable entries and one resource default.
The compatibility `/disk/diskutil` path remains available.
The normal shell uses `/disk/bin/sh`; the typed search order remains unchanged.
The shared file-reading helpers serve both rescue and the standalone disk utilities.

Version 1 deployment manifests retain their executable-only contract.
Version 2 adds resource defaults with mode `0644`.
Deployment creates missing resource files and preserves existing regular files, including their inode metadata.
Program updates retain mode `0755` and the existing preservation checks.
The default resource is `/disk/etc/motd`.

Rescue uses canonical console input with a 127-byte content limit.
An overlong line returns an error instead of executing a truncated command.
End-of-input exits rescue; init then attempts the normal shell again.
Rescue has no guided editor, scripts, aliases, command history, or arbitrary executable launch.
It does not repair filesystems, remount storage, or support storage hotplug.

## Validation record

The local checks use disposable images. They do not update the interactive QEMU disk or a physical USB stick.

| Check | Evidence |
| --- | --- |
| Host unit suite | 590 tests passed; `build/ext2-bootstrap-evidence/host-tests.log` |
| Full integration and contract matrix | 77 checks passed; `build/ext2-bootstrap-evidence/full-tests.log` |
| Final correction checks | All seven affected checks passed; `build/ext2-bootstrap-evidence/correction-tests.log` |
| Fresh diagnostic build | Three checks passed after the correction; `build/ext2-bootstrap-evidence/diagnostic-tests-final.log` |
| Packaging | Exactly `init` and `rescue` in initrd; complete ELF bytes and resource hashes verified on ext2. |
| Startup failures | Missing storage, missing shell, and invalid shell reach rescue under BIOS and UEFI. |
| Preserving repair | Offline deployment restores the shell and preserves other file contents and inode metadata. |
| Storage transports | Virtio, xHCI, and EHCI persistence, removal, timeout, and preserving-update checks pass. |
| Recovery controls | Rescue input overflow, file reads, retry, sync, reboot, and shutdown receive direct guest coverage. |
| Physical hardware | Unmeasured. Test normal boot, rescue, and preserving updates on both machines. |

The full matrix preceded the final EOF correction and the neutral `motd` wording.
The correction tests rerun the affected bootstrap, spawn, packaging, deployment, and configuration checks.
The EOF negative control timed out with repeated rescue prompts on the earlier image.
Its log is `build/ext2-bootstrap-evidence/eof-negative-result.log`.
The final regression requires rescue to return after a redirected input file reaches EOF.

Initial development checks found a stale startup argument offset and an overly strict restart-output assertion.
Both were corrected before the full matrix passed.
No pre-existing test failure was observed in this task.
Existing LTO `main` signature warnings remain outside this change.

Commands:

```sh
cmake --preset default
cmake --build build --target os1_image os1_bios_image os1_usb_image
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
ctest --test-dir build --output-on-failure --no-tests=error
cmake --preset default -B build/ext2-bootstrap-diagnostics \
  -DOS1_REQUIRE_UEFI_SMOKE=ON -DOS1_KMEM_DEBUG=ON \
  -DOS1_QEMU_DEBUGCON=OFF -DOS1_USB_KEYBOARD_DIAGNOSTICS=ON
cmake --build build/ext2-bootstrap-diagnostics --target os1_usb_image -j 8
ctest --test-dir build/ext2-bootstrap-diagnostics --output-on-failure \
  --no-tests=error -R '^os1_smoke_usb_keyboard_'
```

## Review record

A separate read-only reviewer assessed the complete task diff and its corrections.
The first review found the incorrect documented rescue input limit. The second review accepted its correction and regression coverage.
The reviewer also identified an inherited `sh events` documentation error. The guide now describes `events` at the prompt.

The implementer found the redirected-input EOF loop and reproduced it on the earlier binary.
Rescue now exits at EOF, and the bootstrap smoke covers this behavior.
The final independent review accepted this correction and verified the test results, artifact hashes, source digest, and measurement medians.
No unresolved substantive finding remains.

Source formatting uses clang-format 23.1.4 on the seven changed project-owned C++ files.
Third-party and generated files are excluded.
Local document links and new navigation anchors receive a separate check.
Full ASD-STE100 dictionary compliance is not claimed.

The new PR is stacked on PR 44.
The CI workflow selects PRs targeting `main`; it must run after retargeting.

## Reproduction metadata

Baseline source revision: `43576217b48bea7e79747d351100661015852e43`.
Candidate source digest: `19068eecd0cc1107da710ffa1b6b50ff29604566c6b5f82e38d5aac2bebeb04b`.
The digest covers sorted changed non-Markdown paths and contents, with NUL separators.
It excludes generated Python cache files.
Artifact hashes are recorded in `build/ext2-bootstrap-evidence/artifacts-final.json`.

The host is an Apple M4 Max with 16 CPUs and 64 GiB RAM, running macOS 26.6.2.
The compiler is x86_64-elf GCC 11.2.0; the linker is GNU ld 2.37.
The guest build uses Debug, `OS1_KMEM_DEBUG=ON`, and `OS1_QEMU_DEBUGCON=ON`.
Diagnostic checks use `OS1_QEMU_DEBUGCON=OFF` and `OS1_USB_KEYBOARD_DIAGNOSTICS=ON`.

QEMU 11.1.3 uses q35, TCG, four guest CPUs, 128 MiB RAM, and virtio storage for the comparison.
UEFI uses Homebrew edk2 code firmware and a fresh copy of its variable template for each boot.
BIOS uses the firmware supplied with the same QEMU installation.

Each revision runs three times per firmware, in alternating order.
Each run uses a fresh copy of its populated 32 MiB filesystem fixture.
The workload reads `sys` and `kmem`, inspects one PCI object 20 times, then reads both observers again.
Startup timing ends at the first shell prompt.
QEMU CPU time includes boot and workload execution.
Background desktop applications are not controlled. Timing differences are descriptive, not performance guarantees.

## Resource comparison

Values are medians of three runs per revision and firmware.
All allocation-failure counters remained zero.

| Measurement | Baseline | Candidate | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| Initrd bytes | 147,456 | 7,680 | -139,776 | -94.79% |
| UEFI ISO bytes | 4,450,304 | 4,311,040 | -139,264 | -3.13% |
| BIOS image bytes | 1,474,560 | 1,474,560 | +0 | +0.00% |
| Fresh ext2 allocated bytes | 335,872 | 524,288 | +188,416 | +56.10% |
| Fresh ext2 allocated inodes | 38 | 43 | +5 | +13.16% |
| BIOS Startup seconds | 0.462314 | 0.422619 | -0.039695 | -8.59% |
| BIOS Workload seconds | 5.872381 | 5.851923 | -0.020458 | -0.35% |
| BIOS QEMU CPU seconds | 6.709850 | 6.650764 | -0.059086 | -0.88% |
| BIOS free pages at prompt | 31,634 | 31,667 | +33 | +0.10% |
| BIOS total allocator pages | 32,732 | 32,732 | +0 | +0.00% |
| BIOS slab pages at prompt | 168 | 169 | +1 | +0.60% |
| BIOS live slab objects at prompt | 2,335 | 2,336 | +1 | +0.04% |
| UEFI Startup seconds | 12.125818 | 12.136825 | +0.011007 | +0.09% |
| UEFI Workload seconds | 2.734408 | 2.720554 | -0.013854 | -0.51% |
| UEFI QEMU CPU seconds | 5.473324 | 5.482001 | +0.008677 | +0.16% |
| UEFI free pages at prompt | 19,212 | 19,245 | +33 | +0.17% |
| UEFI total allocator pages | 24,676 | 24,676 | +0 | +0.00% |
| UEFI slab pages at prompt | 168 | 169 | +1 | +0.60% |
| UEFI live slab objects at prompt | 2,335 | 2,336 | +1 | +0.04% |

Each physical page contains 4,096 bytes. Both firmware paths expose 33 more free pages at the prompt.
That difference is 135,168 bytes. Slab use increases by one page and one live object.

The fixed BIOS staging limits and raw-image size remain unchanged.
The new rescue ELF occupies 6,936 bytes; its previous size was zero, so its relative change is undefined.
The shell ELF remains 165,640 bytes.
The ext2 fixture uses 184 KiB more space and five more inodes.

The change satisfies the existing image and 128 MiB guest limits.
No timing improvement is required or claimed from these three-run samples.

Raw samples and serial transcripts are in `build/ext2-bootstrap-evidence/measure/`.
Use the retained runner with separately built baseline and candidate artifact directories:

```sh
python3 -B tests/scripts/measure_shell_bootstrap.py \
  --baseline build/ext2-bootstrap-evidence/baseline \
  --candidate build/artifacts --output build/ext2-bootstrap-evidence/measure \
  --qemu /opt/homebrew/bin/qemu-system-x86_64 \
  --code /opt/homebrew/share/qemu/edk2-x86_64-code.fd \
  --vars /opt/homebrew/share/qemu/edk2-i386-vars.fd
```
