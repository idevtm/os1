# CI boot-menu duration

This task removes repeated menu waits from ordinary QEMU tests.
The packaged menu remains unchanged, and dedicated checks retain countdown coverage.

```text
ordinary UEFI check -> observe menu -> select os1 -> existing assertions
menu check -> wait without input / select each entry -> verify logging profile
BIOS and timing measurements -> existing boot behavior
```

## Requirements and impact

| Area | Assessment |
| --- | --- |
| Project scope | GOALS: Runnable vertical slices and simplicity. Keep complete validation while removing repeated waits. |
| Boot and platform | Architecture: Boot profile selection. Preserve both packaged entries, their countdown, and BIOS behavior. |
| Memory and isolation | Bound the test helper buffer. Write only to each test's QEMU input stream. |
| Concurrency | Keep serial QEMU execution and existing artifact locks. Handle explicit guest reboot without sending later shell input. |
| Interfaces | Change test orchestration only. Preserve BootInfo, native ABI profiles, kernel code, and disk formats. |
| Runtime resources | Compare elapsed boot and workload time with identical images, firmware, guest CPUs, and memory. |
| Validation and documents | Add helper boundary tests and countdown coverage. Run affected BIOS/UEFI checks and the complete matrix. |

The Native API index identifies the hardware, logging, and lifecycle profiles as implemented contracts.
This task does not change those contracts or the OS service architecture.
No C or C++ source changes are planned.

## Plan

1. Select the normal entry after the ordinary UEFI menu appears.
2. Preserve explicit diagnostic selection and add a no-input countdown case.
3. Keep natural boot behavior in existing measurement scripts.
4. Test split input, terminal escape sequences, selection bounds, and explicit reboot handling.
5. Measure identical-image baseline and candidate runs.
6. Run the full test matrix and obtain an independent review.

## Baseline evidence

Baseline source: `919dc0d24343c4075ba627e9cd37ed343c0d8cde`.
Its file tree matches the reviewed PR #45 implementation at `8cf3c5a`.

| CI revision | Matrix count | Elapsed time |
| --- | ---: | ---: |
| PR #42, `f118aa9` | 70 | 756.24 seconds |
| PR #43, `11673dd` | 76 | 1313.16 seconds |
| PR #44, `4357621` | 77 | 1319.60 seconds |

These runs used separate hosted runners; they do not isolate kernel performance.
PR #43 added six checks that consumed 78.10 seconds in its recorded run.
It also changed the packaged menu countdown from zero to ten seconds.
The prior controlled menu measurement showed a 10.230104-second boot increase with identical kernel and initrd bytes.
See the [boot-menu record](../boot/2026-11-09-boot-menu.md#comparable-measurements).

The BIOS spawn test also increased across those runs.
This task does not attribute or correct that separate workload change.
Log-export contention and harness processing remain profiling candidates.

## Implementation

The shared helper selects an entry once after it observes the menu text.
It scans incoming bytes before retaining the final 4096 bytes.
Kernel-start output disables selection, including split markers and combined menu/kernel chunks.
Explicit reboot tests reset the helper before they send the reboot command.
The CMake runner uses the same helper through `--boot-menu-entry`.
It records output before selection and preserves diagnostics when the input pipe closes.

The dedicated menu test covers unattended boot and explicit selection of both entries.
Static configuration checks retain the exact ten-second countdown requirement.
The unattended guest case checks the normal profile without sending menu input.
Historical measurement scripts explicitly disable automatic selection.
No-UART and keyboard-independent diagnostic capture retain their existing paths.

## Matched measurements

[Raw samples and artifact identities](2026-11-09-ci-duration-measurements.json) contain twelve runs and the candidate source digest.
The digest hashes each sorted relative path, a NUL byte, its complete file bytes, and another NUL byte.
The JSON record lists all seventeen code and build files in that sequence.
The baseline helper is copied exactly from `919dc0d`; the candidate uses the reviewed helper.
Each pair uses identical kernel, initrd, boot image, program disk, and firmware bytes.
Each run receives a fresh program-disk copy and UEFI variable file.
Three alternating pairs run per firmware, without another local QEMU workload.
The workload boots, lists `/bin`, and prints one line.

Host: Apple M4 Max, sixteen physical cores, 64 GiB RAM, macOS 26.6.2.
Tools: GCC 11.2.0, GNU ld 2.37, Python 3.15.2, QEMU 11.1.3.
Guest: q35, TCG, four CPUs, 128 MiB RAM.
Build: Debug, `OS1_KMEM_DEBUG=ON`, `OS1_QEMU_DEBUGCON=ON`.
UEFI uses the recorded EDK2 images; BIOS uses QEMU's default firmware.

| Median property | Baseline | Candidate | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| UEFI boot, seconds | 12.162774 | 1.977469 | -10.185305 | -83.74% |
| BIOS boot, seconds | 0.436296 | 0.427378 | -0.008918 | -2.04% |
| UEFI QEMU CPU, seconds | 2.572982 | 2.027778 | -0.545204 | -21.19% |
| BIOS QEMU CPU, seconds | 0.485184 | 0.476165 | -0.009019 | -1.86% |
| UEFI command, seconds | 0.006239 | 0.003401 | -0.002838 | -45.49% |
| BIOS command, seconds | 0.006173 | 0.006369 | +0.000196 | +3.18% |

The removed menu wait explains the UEFI boot reduction.
Short command timings vary; these samples do not establish a kernel performance improvement.
QEMU CPU measurements include boot and the complete process lifetime.
Kernel memory, image sizes, guest disk layout, and packaged configuration remain unchanged.
The test helper retains at most 4096 bytes plus the current read during matching.
Physical-device behavior is unchanged by this host-only work and was not tested.

Reproduce the comparison with these commands:

```sh
git show 919dc0d:tests/scripts/test_ext2_persistence.py > build/baseline_boot.py
python3 -B tests/scripts/measure_ci_boot.py \
  --baseline-helper build/baseline_boot.py --artifacts build/artifacts \
  --output build/ci-duration-measure --qemu /opt/homebrew/bin/qemu-system-x86_64 \
  --code /opt/homebrew/share/qemu/edk2-x86_64-code.fd \
  --vars /opt/homebrew/share/qemu/edk2-i386-vars.fd
```

## Validation and review

Six helper tests and sixteen runner tests pass after the corrections.
An initial six-case guest selection passed before the boundary corrections.
The complete 78-case matrix passes on the corrected implementation in 618.17 seconds.
All three diagnostic-image capture checks pass in 10.01 seconds.
The earlier local PR #45 matrix passed 77 checks in 1210.07 seconds.
The observed reduction is 591.90 seconds, or 48.91 percent, despite the additional helper contract and countdown case.
These full-suite runs occurred at different times; only the twelve paired boots form a controlled comparison.
The earlier full matrix preceded the final PR #45 rescue EOF correction, whose affected checks passed separately.
No kernel or C++ source changed, so the C++ host suite was not repeated for this task.
The independent review found three P2 defects across its first two cycles.
Each defect failed a regression before its correction.
The third correction review found no remaining substantive code defects.
The final evidence review verifies artifact hashes, source identity, all twelve samples, and all six comparison rows.

The first matrix was interrupted to avoid reporting mixed-source validation.
Its orphaned test QEMU held the BIOS image lock and prevented one measurement start.
The test process was stopped; the complete matched measurement then passed.
These interrupted results are not completion evidence.

Evidence remains under `build/ci-duration-evidence/`.
Negative controls are `boundary-negative.log`, `runner-negative.log`, and `closed-input-negative.log`.
Final unit logs are `helper-tests-final.log` and `runner-tests-final.log`.
Final measurements are under `measure-final/`; the final matrix log is `full-tests-final.log`.
The diagnostic results are in `diagnostic-tests.log`.
The earlier matrix log is `build/ext2-bootstrap-evidence/full-tests.log`.

Validation commands:

```sh
cmake --preset default
cmake --build build -j 6
python3 -I -B -m unittest discover -s tests/scripts -p test_boot_menu_helper.py
python3 -I -B -m unittest discover -s tests/scripts -p test_smoke_settle.py
ctest --test-dir build --output-on-failure --no-tests=error
ctest --test-dir build/ext2-bootstrap-diagnostics --output-on-failure --no-tests=error -R '^os1_smoke_usb_keyboard_'
git diff --check
```

The diagnostic checks reuse the existing PR #45 diagnostic images; this task changes no guest code or capture script.
Changed Python files pass syntax checks, and changed Markdown links resolve locally.
The secret scan finds only an existing README instruction about a sudo password; it finds no credential.
The complete ASD-STE100 Issue 9 dictionary is unavailable; full language compliance is not established.

## Separate PR #45 CI failure

The unchanged PR #45 head, `919dc0d`, passed 590 host tests and 76 of 77 integration checks.
[Run 42668031097](https://github.com/idevtm/os1/actions/runs/42668031097) failed in `os1_smoke_tree_tools_uefi`.
Recursive copy returned `OS1_ERR_CHANGED` (`-24`) after partial progress.
This failure predates these test-orchestration changes and remains a separate investigation requirement.
The test assertion remains unchanged; no retry or error suppression was added.
