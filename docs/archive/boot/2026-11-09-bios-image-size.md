# BIOS ELF size correction

This correction must fit the BIOS kernel into its existing staging slot without reducing memory or image safety checks.
CI with GCC 16.2.0 produced 434432 bytes against a 434176-byte slot, a 256-byte overflow.

```text
Initialized .data bytes -> ELF file -> loader copy
Alignment + .bss       -> ELF memory range -> loader zero-fill
```

## Plan and impact

Move final data alignment outside the file-backed linker section.
Preserve page-aligned section boundary symbols and the existing zero-fill contract.
Reproduce the overflow with a linker fixture before changing the script.
Verify both boot paths, memory permissions, image limits, and CI after the correction.

| Area | Assessment |
| --- | --- |
| Project scope | Changes kernel packaging only; preserves the shared-kernel boot model |
| Boot and platform | Changes ELF file extent; preserves segment addresses, permissions, BIOS slots, and firmware bounds |
| Memory and isolation | Requires zero-fill of alignment space and unchanged page boundaries; no allocator or user-pointer changes |
| Concurrency | No scheduling, locking, interrupt, or object-lifetime change |
| Interfaces | Preserves BootInfo, UAPI, and Native API layouts; changes only kernel ELF packaging |
| Runtime resources | Measure file size, loaded extent, and representative boot behavior against the previous revision |
| Validation and documents | Linker regression, BIOS/UEFI checks, full CI, updated architecture, and independent review |

Requirements come from GOALS `Boot strategy` and architecture `Build Outputs And Boot Artifacts` and `The Shared Kernel Contract: BootInfo`.
The [Native API index](../../native-api/README.md) remains authoritative for implemented profiles; this correction changes none of them.

## Validation

The linker fixture failed before the correction because `.data` contained 512 alignment bytes in the file.
Its original ELF size was 434432 bytes, matching the CI overflow boundary.
After correction, its size is 433920 bytes and the unchanged boot-contract check passes.
The fixture verifies initialized bytes, zero-fill extent, segment flags, virtual/physical addresses, and page-boundary symbols.

The first affected run passed thirteen checks and exposed two validation issues.
The UEFI spawn check exhausted its deadline; the linker check used a stale generated command after its new `--nm` argument was added.
Reconfiguration resolves the stale test command; the linker regression passes with its final invocation.
The independent spawn-timeout reproduction and correction are described below.
The final spawn and linker recheck passes both tests; all fifteen affected checks now have passing results.
Three additional diagnostic-image checks pass, including BIOS and UEFI capture.

Affected guest checks cover BIOS/UEFI boot, SMP observation, spawn/exec, log export, system lifecycle, menu profiles, and interrupt handoff.
No owned C/C++ source changed, so clang-format was not applicable.
The existing host suite does not link the kernel script; CI still runs it alongside the new linker regression.

Commands:

```sh
cmake --build build --target os1_image os1_bios_image os1_usb_image
ctest --test-dir build --output-on-failure --no-tests=error -R '^os1_(kernel_elf_padding_contract|smoke(_bios|_observe|_observe_bios|_spawn|_spawn_bios|_exec|_exec_bios)?|smoke_log_export_(bios|uefi)|smoke_system_lifecycle_(bios|uefi)|smoke_boot_menu_uefi|interrupt_handoff_contract)$'
ctest --test-dir build --output-on-failure --no-tests=error -R '^os1_(smoke_spawn|kernel_elf_padding_contract)$'
```

Local logs are under `build/bios-size-evidence/`.
CI provides separate GCC 16.2.0 validation of the committed revision.

## Menu timeout correction

PR 43 introduced a ten-second menu wait while retaining the old thirty-second UEFI smoke deadline.
The pre-fix image also times out in the spawn workload at thirty seconds.
The same image passes all 56 markers with a forty-second total deadline.
Thus, this timeout predates the linker correction.
The corrected UEFI deadline includes ten menu seconds plus the original thirty-second test allowance.
BIOS timing and required markers remain unchanged.

To repeat the control, obtain the `os1_smoke_spawn` command from `ctest --test-dir build --show-only=json-v1`.
Set `ISO_IMAGE` to the saved pre-fix ISO.
Use separate log files for the two runs.
Run it with `SMOKE_TIMEOUT_SECONDS=30`, then `SMOKE_TIMEOUT_SECONDS=40`.
The local transcripts are `baseline-spawn-30.txt` and `baseline-spawn-40.txt`.

## Measurements

The baseline code is revision `7fd618a`; the saved artifacts precede this correction.
That revision changed only documentation after the hardware-tested kernel build.
The candidate is the current working tree, identified by its generated source fingerprint.
The [measurement manifest](2026-11-09-bios-size-measurements.json) records artifact hashes, sizes, and loaded-segment hashes.
Each artifact was measured once; file size is deterministic for that build.

| Artifact | Before | After | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| BIOS kernel ELF | 430336 bytes | 429192 bytes | -1144 bytes | -0.266% |
| Kernel ELF with symbols | 576400 bytes | 575256 bytes | -1144 bytes | -0.198% |
| UEFI ISO | 4448256 bytes | 4446208 bytes | -2048 bytes | -0.046% |
| BIOS raw image | 1474560 bytes | 1474560 bytes | 0 bytes | 0% |
| Writable segment memory | 229376 bytes | 229376 bytes | 0 bytes | 0% |

Loaded `.text` and writable-segment hashes match before and after the correction.
The read-only segment changes only for generated build-identity strings.
All segment addresses, flags, and memory extents remain unchanged.
The BIOS loader still reads the fixed slot; its disk-read volume does not change.
Both loaders already zero the full memory extent, so the correction only removes redundant initialized padding bytes.
No runtime latency improvement is claimed or measured.
The deterministic memory footprint and file-size comparison establish the affected resource changes.

The local compiler is x86_64-elf GCC 11.2.0 on Apple M4 Max with macOS 26.6.2.
Build settings are Debug, `OS1_KMEM_DEBUG=ON`, and `OS1_QEMU_DEBUGCON=ON`.
Guest checks use QEMU 11.1.3, TCG, q35, four CPUs, and 128 MiB RAM.
UEFI uses the configured QEMU EDK2 firmware; BIOS uses the raw-image path.

## Review and limits

Independent review checked both loaders, the linker script, memory boundaries, and the regression fixture.
The correction review also checked the timeout controls and final evidence; it found no blockers.
Physical machines have not retested this packaging correction.
The preceding hardware record remains evidence for the original implementation, not this revised artifact.
The complete ASD-STE100 dictionary was unavailable; prose received active-voice, terminology, sentence-length, and link review.

## CI spawn follow-up

CI run `42600866845` passes the image build, all 583 host tests, and 75 of 76 matrix checks.
The new linker regression passes with the CI toolchain.
The UEFI spawn workload reaches 39 of 56 markers before its forty-second deadline.
Its last completed program is `activationcheck`; the remaining commands have not run.
The same BIOS workload passes in 30.92 seconds.

```text
Menu appears -> select default os1 -> run unchanged spawn workload -> check all 56 markers
```

The follow-up selects normal boot explicitly for the UEFI spawn check.
It sends Enter when the second menu label appears; the default selection remains the first entry.
The automatic-boot baseline and separate two-entry menu test retain boot-menu coverage.
The workload, rejection markers, settlement period, and forty-second deadline remain unchanged.

This test-only change affects validation timing, not kernel behavior, memory, concurrency, interfaces, or image layout.
The existing impact assessment still applies to the linker correction.
Validate the generated CTest command, both spawn paths, the packaged menu, and the linker regression.
Obtain independent review before pushing the correction.

The first selection trial passed its markers but took 38.07 seconds, versus 38.11 seconds without selection.
That result did not establish successful menu selection.
Independent review identified the line-buffered runner: it could not dispatch input until the menu produced a newline.
A new subprocess regression reproduces this failure with a menu that waits for Enter without a newline.
The runner now reads bounded 4096-byte chunks and preserves split UTF-8 characters with an incremental decoder.
The new menu regression and the existing settlement checks pass after this correction.
A second regression checks a menu label with a UTF-8 character split across writes.

This correction affects the shared smoke harness, so validation includes all direct smoke workloads and monitor input.
It preserves expected markers, rejected markers, process cleanup, and settlement checks.
It does not modify production software, boot profiles, or resource limits.

Review also identified rejected markers split across output chunks.
A subprocess regression reproduced acceptance of a split `BROKEN` marker, including ANSI and prompt noise.
Rejection matching now uses the bounded accumulated buffer before required-marker noise removal.
All ten settlement and streaming tests pass after this correction.
The monitor contract passes with local socket permission; its sandbox denial was an environment limitation.

The same local image completes all 56 spawn markers in 29.22 seconds with streaming menu selection.
The line-buffered selection trial took 38.07 seconds.
The difference is -8.85 seconds, or -23.25%, from one sample per configuration.
Both samples use the compiler, firmware, CPU count, and memory settings recorded above.
These measurements describe test-harness timing; they do not establish a production performance improvement.

Accumulated rejection also exposed a stale exec assertion.
The old line reader never matched its multiline `after-exec` rejection marker.
The corrected reader detects that output from the replacement shell, which init must restart after the program exits.
Both exec checks retain the `shell exec returned` rejection and remove the obsolete echo probe.
The original shell emits that diagnostic after any return from its replacement call.
A multiline rejection regression prevents silent loss of this detection behavior.
The architecture now describes the accepted restart contract accurately.

Review rejected a replacement-shell echo requirement because command echo could satisfy its output marker.
The final exec check keeps the existing direct no-return diagnostic instead of adding that redundant probe.
Separate lifecycle checks verify shell restart; the exec check verifies replacement execution and rejects return to the original shell.

The permitted shared-harness run passes all sixteen checks in 153.52 seconds.
After removal of the echo probe, all five final streaming, exec, and lifecycle checks pass in 57.95 seconds.
The streaming contract includes eleven subprocess cases.
The final independent correction review finds no blockers.
Local file links and whitespace checks pass; the credential-pattern scan finds no matches.
Full CI must run again on the follow-up commit.

```sh
ctest --test-dir build --output-on-failure --no-tests=error -R '^os1_(smoke(_bios|_observe|_observe_bios|_spawn|_spawn_bios|_exec|_exec_bios|_balance|_balance_bios|_no_uart)?|smoke_(settle|monitor)_contract|boot_menu_contract|smoke_boot_menu_uefi|kernel_elf_padding_contract)$'
ctest --test-dir build --output-on-failure --no-tests=error -R '^os1_(smoke_exec(_bios)?|smoke_system_lifecycle_(bios|uefi)|smoke_settle_contract)$'
```

The transcripts are `menu-final-checks.txt` and `exec-final-checks.txt` in the local evidence directory.
These tests use the same production images as the preceding comparison; the follow-up changes only harness behavior and documentation.

## Explicit output-line matching

A final harness check reproduced partial command echo satisfying a required output-line marker.
The whitespace-compacted fallback discarded the marker's explicit newline boundaries.
Markers with leading or trailing newlines now require those boundaries in raw or normalized output.
Other markers retain their existing whitespace-tolerant behavior.
A regression rejects echoed input without command output; another accepts a real output line split across chunks.
This correction strengthens test assertions and does not change production images or test deadlines.

All thirteen subprocess cases and sixteen shared-harness checks pass with explicit output-line matching.
The shared checks take 152.67 seconds versus 153.52 seconds before this assertion correction.
The difference is -0.85 seconds (-0.55%), with one sample per configuration and unchanged production images.
This difference does not establish a performance improvement.

## Lifecycle fixture isolation

CI run `42606930354` passes the corrected spawn check but fails the BIOS lifecycle clean-at-prompt assertion.
All 583 host tests and 75 of 76 matrix checks pass.
The lifecycle test assumes no writer modifies storage after a shell command completes.
The accepted export ABI permits background writes with a five-second normal synchronization interval.
Thus, a dirty superblock at an arbitrary prompt snapshot does not establish failed command-boundary synchronization.

```text
Lifecycle fixture: /disk/log is a regular file -> export cannot write -> check immediate command synchronization
Export fixture:    /disk/log is a directory   -> background writes   -> check persistence and final shutdown drain
```

The lifecycle fixture now reserves `/disk/log` as a regular file before boot.
It uses the existing deployment manifest and utility payload to create that fixture entry.
The exporter returns `NOT_DIRECTORY` before identity or frame writes; memory capture continues.
All immediate clean-state assertions remain unchanged, including the pre-parent-sync check and abrupt-stop check.
Separate BIOS and UEFI export checks retain active background writes, command traffic, reboot persistence, and shutdown-drain coverage.
The final shutdown path must still synchronize storage when log export fails.

This correction affects test isolation and fixture disk use, not production storage policy, kernel code, or ABI.
Validate both lifecycle paths and both active-export paths before committing.

Both lifecycle and both active-export checks pass: four checks in 103.09 seconds.
Their transcript is `lifecycle-export-checks.txt` in the local evidence directory.
The fixture comparison uses the same program manifest and local toolchain, with one sample per configuration.
The additional 5264-byte utility copy consumes 8192 allocated bytes and one inode.
Allocated blocks increase from 82 to 84 (+2 blocks, +2.44%).
Allocated inodes increase from 38 to 39 (+1 inode, +2.63%).
Both disk image files remain 34603008 bytes (0 bytes, 0%).
The 32 MiB filesystem capacity and guest CPU/memory settings remain unchanged.
The measurement transcript is `lifecycle-fixture-measurements.txt`.

```sh
ctest --test-dir build --output-on-failure --no-tests=error -R '^os1_smoke_(system_lifecycle|log_export)_(bios|uefi)$'
```

## Compound spawn watchdog

The compound spawn workload needs additional headroom on shared TCG runners.
A separate sixty-second watchdog preserves all functional assertions.

| Spawn watchdog | Before | After | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| BIOS | 30 seconds | 60 seconds | +30 seconds | +100% |
| UEFI | 40 seconds | 60 seconds | +20 seconds | +50% |

CI run `42611284503` passes all 583 host tests and 75 of 76 matrix checks, including lifecycle isolation.
The BIOS spawn workload reaches 54 of 55 markers before its thirty-second watchdog stops the final interface listing.
The same run completes the UEFI spawn workload in 40.16 seconds, including settlement.
Earlier successful BIOS spawn runs took 26.23 and 30.92 seconds, including settlement.

The configured smoke limits are watchdog ceilings for shared TCG runners, not product latency requirements.
The long spawn workload combines descriptor, filesystem, IPC, buffer, activation, and parameter checks in one boot.
Sixty seconds provides about 50% headroom above the observed forty-second successful run.

All commands, required markers, rejected markers, and the one-second settlement check remain unchanged.
The other BIOS and UEFI watchdogs remain unchanged.
The test still exits immediately after successful validation and settlement.
This change affects only the maximum failure wait for two tests; it changes no production code or resource limits.

Both local spawn checks pass with the unchanged production images and full marker sets.
The baseline is the preceding `dd571a1` harness run; the candidate changes only the configured watchdog.
Each configuration has one sample with the same toolchain, firmware, four CPUs, and 128 MiB RAM.

| Workload | Baseline | Candidate | Absolute difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| UEFI spawn | 27.65 seconds | 31.40 seconds | +3.75 seconds | +13.56% |
| BIOS spawn | 21.68 seconds | 20.71 seconds | -0.97 seconds | -4.47% |

These single samples show runtime variation; they do not establish a production performance change.
The watchdog does not delay early completion.
The transcript is `spawn-watchdog-checks.txt`; the baseline is `line-boundary-checks.txt`.

```sh
ctest --test-dir build --output-on-failure --no-tests=error -R '^os1_smoke_spawn(_bios)?$'
```
