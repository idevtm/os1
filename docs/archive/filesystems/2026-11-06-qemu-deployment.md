# QEMU program deployment and target navigation

This plan adds offline program updates that preserve existing QEMU files.
It also groups the `os1.sh` target menu by task.

```text
build programs -> explicit manifest -> lock stopped QEMU image
 -> copy image and extract partition -> e2fsprogs update
 -> profile, content, and preservation checks -> atomic image replacement

os1 -> Run QEMU / Storage / USB / Checks -> category -> target
```

## Scope and decisions

- Start from merged PR 29, revision `a3f2bde`.
- Use e2fsprogs `debugfs` for filesystem writes and `e2fsck` for consistency checks.
- Keep the current one-group ext2 profile and immutable guest executables.
- Deploy explicitly listed executable paths. Preserve every other existing file and directory.
- Reject replacement of an existing nonexecutable data file.
- Create missing destination directories within the existing filesystem limits.
- Reject active, dirty, unsupported, or malformed images before publication.
- Stage updates beside the image. Publish only after validation through atomic file replacement.
- Preserve the partition table and bytes outside the data partition.
- Do not format, reset, uninstall, or write physical devices during deployment.
- Keep direct target commands and noninteractive default behavior.
- Add bounded terminal menus with categories, Back, cancellation, and unavailable-target explanations.

## Impact assessment

| Area | Requirements and impact |
| --- | --- |
| Project scope | GOALS: Storage and filesystems; runnable deployment slice without a new guest installer. |
| Boot and platform | ARCHITECTURE: Persistent QEMU storage; existing BIOS/UEFI payloads remain unchanged. Test both guest paths after deployment. |
| Memory and isolation | Host staging uses bounded image and file sizes. No new user-pointer, DMA, or kernel-memory behavior. |
| Concurrency | Lock the regular image against QEMU and concurrent writers. Check path identity before replacement. |
| Interfaces | Ext2 profile and manifest become tooling contracts. Native API profiles, descriptors, and syscalls remain unchanged. |
| Runtime resources | Measure deployment time, temporary disk use, and unchanged artifact sizes against the merged baseline. |
| Validation and documents | Test updates, preserved file hashes, failure rollback, locks, CLI paths, terminal navigation, and both firmware boots. |

The Native API index separates implemented profiles from future service designs.
This task does not introduce public BlockDevice IPC, a guest package manager, USB transport, or physical partition updates.

## Implementation sequence

1. Add a versioned program manifest and staged deployment tool.
2. Add CMake deployment targets and actionable dependency errors.
3. Replace the flat selector with category navigation using the existing menu behavior.
4. Add two separately built program versions and reboot-based update checks.
5. Run host tooling contracts, existing affected checks, and BIOS/UEFI deployment tests.
6. Obtain independent read-only reviews. Correct findings and repeat affected checks.
7. Record measurements and results, archive this completed plan, and open a PR.

## Acceptance

An existing QEMU image contains guest-created files and a first program version.
After offline deployment, the next boot executes the second version from the same guest path.
The original file names, bytes, and file metadata remain unchanged outside the listed program paths.
Failed validation, missing input, full media, and active-image locks leave the original image unchanged.
Menus restore the terminal after selection, cancellation, and interruption.

## Evidence

All 26 root CTest cases have passing results across the full run and focused reruns.
The full run initially found a menu regression and two sandbox socket restrictions.
The menu regression is corrected. Both socket checks passed outside the sandbox.
The new configuration contract and all affected tests passed after corrections.

| Check | Result and local evidence |
| --- | --- |
| Deployment contracts | Seven tests passed: updates, indirect files, nested paths, preservation, idempotence, invalid media, full disk, failed tools, locks, and publication failure. `/tmp/os1-deploy-contract.log` |
| Target menu | Four tests passed, including seven PTY navigation cases and frame bounds down to 1x1. `/tmp/os1-target-menu.log` |
| USB regression | Eighteen tests passed. `/tmp/os1-usb-menu.log` |
| Fresh configuration | Default and BUILD_TESTING=OFF passed. `/tmp/os1-deploy-config-contract.log` |
| Clean build | Both image targets built from a fresh directory. `/tmp/os1-deploy-clean-build.log` |
| Clean-build deployment | BIOS and UEFI passed. `/tmp/os1-deploy-clean-boot.log` |
| Broad root suite | Initial run covered 25 cases; 22 passed. `/tmp/os1-deploy-all.log` |
| Corrected and new cases | All six selected tests passed. `/tmp/os1-deploy-final-focused.log` |
| Socket-dependent cases | Monitor exchange and no-UART passed outside the sandbox. `/tmp/os1-deploy-socket-checks.log` |
| Repeated deployment | BIOS and UEFI each passed three consecutive runs, with three independent boots per run. `/tmp/os1-deploy-repeat.log` |

Each deployment smoke verifies version 1 before editing, then version 2 on two subsequent boots.
It verifies the 128 KiB deterministic data file and a guest-created note inside `/disk/bin`.
Host snapshots compare all unlisted file hashes and inode metadata before and after deployment.
`e2fsck -fn` reports success after each stage.
A live-QEMU check rejects deployment while the guest holds the image lock.
The image bytes remain unchanged after that rejected operation.

The kernel and native ABI did not change. The existing kernel host-unit suite was not rerun locally for this tooling change.
CI runs that suite and the root suite on the PR head.
Physical USB media remain untested and unchanged.

Reproduction commands:

```sh
cmake --preset default
cmake --build build --target os1_image os1_bios_image
ctest --test-dir build --output-on-failure
ctest --test-dir build -R '^os1_smoke_deploy_' --repeat until-fail:3 --output-on-failure
python3 tests/scripts/test_build_configuration.py
```

## Measurements

Baseline: merged revision `a3f2bde`. Current: this branch before its first commit.
Host: macOS 26.6.2 arm64, GCC 11.2.0, QEMU 11.1.3, e2fsprogs 1.47.4.
The existing build uses KMEM_DEBUG and DEBUGCON; user programs use `-Os`.
Guest tests use q35, four CPUs, 128 MiB RAM, virtio-blk, and BIOS or OVMF firmware.

| Artifact | Baseline bytes | Current bytes | Absolute / relative change |
| --- | ---: | ---: | --- |
| Kernel ELF | 487128 | 487128 | 0 / 0% |
| BIOS kernel ELF | 365056 | 365056 | 0 / 0% |
| Initrd | 191488 | 191488 | 0 / 0% |

Kernel ELF hashes also match the baseline. Initrd archive timestamps differ; its size remains unchanged.
Two disk-only test programs add 2768 bytes to build artifacts and zero bytes to initrd.
No new guest runtime allocation or syscall path was added.

A 32 MiB image update replaced a 1384-byte version-1 program with version 2.
Three wall-time samples were 44.059, 39.387, and 43.000 ms; the mean was 42.148 ms.
This includes validation, e2fsprogs, image copying, and synchronization, excluding Python startup.
The previous revision had no preserving deployment operation; its relative timing difference is undefined.

A separate process sample took 78.287 ms including startup, with 39.407 ms user CPU and 33.333 ms system CPU.
Child-process resource accounting reported a maximum resident set of 26,574,848 bytes on macOS.
This is one sample, not a worst-case bound for 64 large program inputs.
The source list bounds payload memory; image copies use 1 MiB buffers.
Logs: `/tmp/os1-deploy-upgrade-measurements.json` and `/tmp/os1-deploy-resources.json`.

Staging used at most 68,158,824 bytes for the sample, in addition to the original image.
The staged copy materializes sparse regions.
Host allocation rose from 1,097,728 to 34,603,008 bytes: an increase of 33,505,280 bytes, approximately 3052%.
Logical image capacity stayed 34,603,008 bytes. Sparse-copy optimization remains a measured follow-up.
The previous reset operation was faster because it discarded existing data; it is not an equivalent deployment baseline.

The old flat menu displayed 22 targets and approximately 29 lines at normal width.
The new root displays four categories and Quit in 10 lines; submenus scroll within the terminal height.
This trades extra category selections for bounded, readable navigation.

## Review dispositions

Independent read-only reviews found these issues during implementation:

- P1: deeply nested destinations could pass host checks but fail guest mount. Enforce the kernel depth bound and test rejection.
- P2: host profile checks omitted inode flags. Reject unsupported flags and test that the original image remains unchanged.
- P1: fresh configuration defined BUILD_TESTING after test-program creation. Initialize CTest earlier and test clean configurations.
- P2: menu help wrapped beyond small terminals. Clip every row and bound the full frame; test small dimensions.

Local testing also found two setup issues.
The original deployment test used an unavailable shell `mkdir`; it now uses a deployment-created directory and checks immediate readback.
A new PTY can report zero dimensions; the renderer uses 80x24 only when dimensions are unspecified.
Submenu transitions now retain queued keystrokes instead of flushing them.

Both independent correction reviews found no unresolved code findings.
Their remaining documentation finding was a stale test-count paragraph; the architecture now lists the new smokes and correct totals.
Full ASD-STE100 dictionary compliance remains unverified because the complete dictionary was unavailable.
