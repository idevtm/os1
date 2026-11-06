# Minimal initrd and disk programs

This task keeps four recovery programs in initrd and moves 23 optional programs to ext2.
The archive shrinks by 141,312 bytes; the normal kernel binary remains unchanged.
The keyboard correction from PR 33 is the implementation baseline.

```text
initrd /bin: init -> sh, ls, cat
shell command -> /bin first -> /disk/bin second
ext2 /disk/bin: utilities, checks, providers, examples
build -> manifest -> offline deployment -> preserve unlisted files
```

## Plan and acceptance

1. Keep `init`, `sh`, `ls`, `cat`, and `/etc/motd` in initrd.
2. Package all other bundled programs under `/disk/bin` through the default deployment manifest.
3. Preserve `/disk/diskutil` for existing workflows; also install it under `/disk/bin`.
4. Search `/bin` before `/disk/bin` for bare shell commands, `exec`, and `call`.
   Explicit paths keep their meaning. Search continues only after NOT_FOUND.
5. Move explicit verification-program and provider references to their disk locations.
6. Seed new QEMU storage with programs. Existing images change only through explicit deployment.
7. Attach disposable program storage to verification smokes that require these programs.
8. Check BIOS and UEFI recovery without storage, disk execution, provider activation, and preserving updates.
9. Measure archive size and comparable boot times. Obtain independent review before opening the PR.

## Requirement impact

| Area | Requirements and effects |
| --- | --- |
| Scope | [GOALS: Runnable vertical slices](../../../GOALS.md#4-runnable-vertical-slices) and [Storage and filesystems](../../../GOALS.md#storage-and-filesystems). Recovery remains usable without optional programs. |
| Boot/platform | [Architecture: Initrd And ELF Loader](../../ARCHITECTURE.md#initrd-and-elf-loader) and [BIOS image layout](../../ARCHITECTURE.md#build-outputs-and-boot-artifacts). Same boot contract and capacity; smaller archive. Both firmware paths require checks. |
| Memory/isolation | [Ext2: Native API and concurrency](../../filesystems/ext2.md#native-api-and-concurrency) retains snapshot bounds and authority checks. No new kernel allocation mechanism. |
| Concurrency | [Ext2: Native API and concurrency](../../filesystems/ext2.md#native-api-and-concurrency) retains admission and lifetime rules. Deployment remains offline with image locking. |
| Interfaces | [VFS ABI: Launch and startup](../../native-api/abi/initrd-vfs-abi.md#launch-and-startup) stays unchanged. Installed paths and shell lookup change. |
| Runtime resources | [Deployment: Bounds and limitations](../../filesystems/program-deployment.md#bounds-and-limitations): less initrd data; more ext2 files and executable reads. Measure archive and program bytes and boot latency. |
| Validation/docs | [Deployment: Validation](../../filesystems/program-deployment.md#validation) requires execution of disk programs. Add recovery and lookup coverage; retain preservation checks. |

Hardware drivers remain in the kernel. Dynamic drivers and filesystem profile expansion are outside this task.
Custom manifests remain explicit deployment policy. They can omit optional programs.
Full ASD-STE100 dictionary compliance is unverified because the complete dictionary is unavailable.

## Keyboard hardware acceptance

On 2026-11-06, the user confirmed that keyboard input works on the Gigabyte H81M-S1 desktop.
The keyboard is a Lenovo Calliope USB G2, USB ID `17ef:6190`, in the previously failing rear port.
PR 33 at `5bbfc37` passed CI run 184 before merge.
The exact retested image hash was not supplied. This result confirms user-observed keyboard recovery, not general controller qualification.
PR 33 was squash-merged as `cb2311e`; issue 27 is closed.

## Results

The default manifest contains 24 entries: 23 programs under `/disk/bin` and the compatibility copy at `/disk/diskutil`.
New QEMU images receive these programs before publication. Existing images require explicit `storage_deploy`.
USB creation and preserving program updates use the same manifest.
The normal `run` targets now attach persistent storage instead of the block smoke fixture.
Verification suites use a separate generated image with private QEMU overlays.

### Checks

| Check | Result |
| --- | --- |
| Host suite | 442 passed, including lookup precedence, explicit paths, bounds, and error handling |
| Normal build/tooling/QEMU matrix | All 46 distinct checks passed across the matrix and targeted reruns |
| Recovery file suites | BIOS and UEFI, each with and without program storage, passed |
| Diagnostic image | Three capture tests passed: UEFI keyboard/no-keyboard and BIOS keyboard |
| Deployment | Distinct program versions run after updates; user files survive updates and reboot |
| Packaging | Exactly four initrd executables; all manifest entries match complete source ELF hashes |
| Independent review | Implementation findings corrected; final evidence review recorded below |

Commands used:

```sh
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
cmake --preset default -B build/minimal-initrd -DOS1_REQUIRE_UEFI_SMOKE=ON -DOS1_KMEM_DEBUG=ON -DOS1_QEMU_DEBUGCON=ON
cmake --build build/minimal-initrd --target os1_image os1_bios_image os1_usb_image os1_deploy_probe_1 os1_deploy_probe_2
ctest --test-dir build/minimal-initrd --output-on-failure
ctest --test-dir build/minimal-initrd --output-on-failure -R '^os1_smoke_files(_disk)?_(bios|uefi)$|^os1_program_packaging_contract$'
cmake --preset default -B build/minimal-diagnostic -DOS1_REQUIRE_UEFI_SMOKE=ON -DOS1_KMEM_DEBUG=ON -DOS1_QEMU_DEBUGCON=OFF -DOS1_USB_KEYBOARD_DIAGNOSTICS=ON
cmake --build build/minimal-diagnostic --target os1_usb_image
ctest --test-dir build/minimal-diagnostic --output-on-failure -R '^os1_smoke_usb_keyboard_'
```

Initial validation found a missing VFS header include, an allocator diagnostic in a new assertion, and a test assuming disk programs remained in initrd.
These were corrected. The interactive suite now runs with and without program storage, retaining the original parameter-error assertions.
One early deployment test raced the agent's rebuild and encountered a temporarily missing ELF file.
A later run used fixed artifacts and passed all deployment checks.
The 44-test run passed 43 checks; its remaining BIOS file test passed in the final five-test rerun.
That rerun also added two disk-backed file suites, for 46 distinct normal checks.
Logs: `/tmp/minimal-final-host-tests.log`, `/tmp/minimal-final-tests.log`, `/tmp/minimal-recovery-tests.log`, and `/tmp/minimal-diagnostic-tests.log`.
No physical disk was written. The new packaging still requires physical upgrade acceptance.

### Resource comparison

Baseline: merged PR 33, `cb2311e`, with artifacts from its unchanged tree at `5bbfc37`.
Candidate: this task's final source tree before commit.
Both normal builds use GCC 11.2.0, Debug configuration, kernel size optimization, `OS1_KMEM_DEBUG=ON`, and `OS1_QEMU_DEBUGCON=OFF`.
The host is macOS ARM64. QEMU 11.1.3 uses TCG, q35, four CPUs, and 128 MiB RAM.
UEFI uses Homebrew `edk2-x86_64-code.fd` with a fresh `edk2-i386-vars.fd` copy; BIOS uses QEMU SeaBIOS.

| Property | Baseline | Candidate | Difference |
| --- | ---: | ---: | ---: |
| Initrd | 192,000 B | 50,688 B | -141,312 B (-73.6%) |
| UEFI ISO | 4,421,632 B | 4,280,320 B | -141,312 B (-3.196%) |
| BIOS raw image | 1,474,560 B | 1,474,560 B | 0 B (0%) |
| Stripped kernel | 372,992 B | 372,992 B | 0 B (0%) |
| Kernel text/read-only data | 339,968 B | 339,968 B | 0 B (0%) |
| Kernel data / BSS | 32,768 / 180,224 B | 32,768 / 180,224 B | 0 B (0%) |
| Slab pages after workload | 170 | 167 | -3 pages (-1.765%) |
| Live allocator objects after workload | 2,353 | 2,331 | -22 objects (-0.935%) |

The stripped kernel SHA-256 is identical: `02e4672e2ee3b84c3610f3d6f5ff0be57ccaf1ded0ded526b71dabe64d1194bb`.
The manifest carries 181,864 ELF bytes. The standalone test disk occupies 34,603,008 host bytes after deployment.
Existing 32 MiB QEMU and 128 MiB USB partition capacities remain unchanged.
Optional programs require disk I/O; full ELF bytes preserve interface metadata and debugging sections.
No kernel ABI, allocator, driver, or filesystem bound changed.

Each firmware/profile pair ran three times, alternating baseline and candidate without concurrent QEMU tests.
Both images used the same ext2 program disk.
Boot time ends at the first shell prompt. The timed workload launches Counter ten times and reads one allocator snapshot.
Baseline bare Counter commands use initrd; candidate commands use `/disk/bin`.
The measurements include host process and console overhead; they are not storage throughput benchmarks.

| Median | Baseline | Candidate | Difference |
| --- | ---: | ---: | ---: |
| BIOS boot | 0.464389 s | 0.417609 s | -0.046780 s (-10.073%) |
| BIOS workload | 0.383353 s | 0.407850 s | +0.024497 s (+6.390%) |
| UEFI boot | 1.939104 s | 1.951264 s | +0.012160 s (+0.627%) |
| UEFI workload | 0.150904 s | 0.126019 s | -0.024885 s (-16.491%) |

Raw boot samples, in seconds:

- BIOS baseline: 0.464389, 0.459226, 0.469979; candidate: 0.412382, 0.418070, 0.417609.
- UEFI baseline: 1.920249, 1.939104, 1.984701; candidate: 1.937629, 2.005154, 1.951264.

Raw workload samples, in seconds:

- BIOS baseline: 0.382892, 0.383353, 0.386058; candidate: 0.398024, 0.407850, 0.410773.
- UEFI baseline: 0.150874, 0.151628, 0.150904; candidate: 0.130410, 0.126019, 0.124554.

Logs and the harness remain under `build/minimal-evidence/`; baseline artifacts remain under `build/initrd-baseline/`.
Size reduction and functional acceptance pass. Timing results are observations, without a general performance claim.

### Review dispositions

The independent reviewer checked packaging, lookup, build dependencies, preservation, tests, and current documentation.
The review found a silent `call` failure and stale usage/documentation paths.
The implementation now reports missing-image errors; guest tests cover the diagnostic and redirected output.
The usage text, prerequisites, archive contents, and ABI examples now match disk deployment.
A second review found no remaining implementation defects and requested complete impact references and measurement evidence.
The third review verified the final evidence and found no blocking findings.
Physical upgrade acceptance and full dictionary compliance remain unverified.
