# Quiet command completion — 2026-11-06

Remove routine shell spawn and resume messages after foreground commands.
Preserve launch errors, wait failures, and nonzero child exit diagnostics.

```text
Foreground child -> waitpid -> success: next prompt
                           -> failure: diagnostic, then next prompt
```

## Impact and validation plan

Baseline: `febd78657a781e832a2af4eb62dbfec0028278bf`.
GOALS Milestone C requires a serial-drivable shell with process-control checks.
The architecture's Interactive file tools section and shell guide define output separation.
This change affects shell presentation and smoke synchronization, not Native API records or accepted designs.

| Area | Impact |
| --- | --- |
| Scope | Reduce routine console output while preserving useful errors. |
| Boot/platform | No loader or driver changes; check UEFI and BIOS shell workflows. |
| Memory/isolation | No ownership, allocation, grant, or user-copy changes. |
| Concurrency | Preserve foreground waiting and scheduling; only completion output changes. |
| Interfaces | No syscall or IPC changes; successful commands no longer print PID/status or resume text. |
| Runtime | Compare console bytes for the same command, image sizes, and three runs per boot path. |
| Validation | Update obsolete smoke markers without dropping program-result checks; run affected guest checks and independent review. |

Host tests need no rerun for this output-only change in freestanding shell code.
Physical hardware and full ASD-STE100 dictionary verification remain unavailable.

## Validation result

`cmake --build build` passed.
`ctest --test-dir build -R '^os1_smoke_(files_|spawn|observe)' --output-on-failure` passed all six checks.
These cover UEFI and BIOS command launch, fault reporting, observation, interactive files, and nested shells.
Removed messages are reject markers; program-result checks and expected fault diagnostics remain required.
The shell source was formatted with clang-format; local document links and `git diff --check` passed.

## Comparable measurements

The baseline and changed builds ran `cat /etc/motd` after boot, three times per boot path.
The measurement counts output from command echo through the next prompt, including unchanged kernel allocation logging.
Host: macOS 26.6.2 arm64; GCC 11.2.0; Debug configuration; userspace size optimization and LTO.
Kernel kmem/debugcon settings remain enabled. QEMU 11.1.3 uses q35, four CPUs, software emulation, and default 128 MiB RAM.
UEFI uses the same Homebrew OVMF code and variables template as the [preceding measurement](2026-11-06-interactive-files.md#comparable-runtime-measurements).
BIOS uses the same QEMU default firmware.

| Boot path | Baseline command time samples, ms | Final samples, ms | Median change | Console bytes |
| --- | --- | --- | --- | --- |
| BIOS | 24.833, 27.171, 24.999 | 15.802, 15.602, 15.762 | -9.237 ms (-36.95%) | 130 -> 75 |
| UEFI | 8.152, 9.833, 8.141 | 6.662, 6.456, 6.471 | -1.681 ms (-20.62%) | 130 -> 75 |

Each sample removes one spawn line and one resume line: 55 fewer console bytes (-42.31%) for this command.
Timing samples are short and noisy; they do not establish a performance improvement.
The kernel ELF remains 471,128 bytes and the initrd remains 201,728 bytes; both artifact-size deltas are zero.
Allocation, quotas, ABI sizes, and wait semantics are unchanged.
Raw samples and logs are in ignored `build/quiet-shell-evidence/`.

## Review and final identity

Independent review found no concrete defects in completion output, failure diagnostics, smoke sequencing, or stream routing.
The reviewer did not edit files or run tests.
Final source is the working-tree change on the baseline revision above.
Shell source SHA-256: `f8815aca55e83a115be0a76968fdddd88db4d43e1c38b612bc7c6ec7b391e308`.

## Echo follow-up plan — 2026-11-06

Print only echo arguments separated by one space, followed by a newline.
An argument-free command prints one newline. Remove the prefix and its extra leading space.
This follows the quiet-completion change above; its baseline is that working tree, not the original committed shell.

| Area | Assessment |
| --- | --- |
| Scope/interfaces | Correct shell text output, including redirection; no Native API or syscall changes. |
| Boot/platform | Update serial and keyboard smoke expectations; validate UEFI, BIOS, and no-UART input. |
| Memory/concurrency | No allocation, ownership, grant, wait, or scheduling changes. |
| Resources/validation | Compare console byte counts and artifacts; preserve exact output assertions and distinguish output from input echo. |

The GOALS Milestone C, architecture Interactive file tools section, and shell guide remain the relevant contracts.

## Echo follow-up result

`echo hello world` now prints `hello world` and a newline.
Empty echo, multiple arguments, quoted spaces, file replacement/append, and nested-shell output have exact-content checks.
Quoted fragments make smoke output markers distinct from input echo, including PS/2 keyboard input without a UART.

`cmake --build build` passed.
`ctest --test-dir build -R '^os1_smoke(_bios|_spawn(_bios)?|_exec(_bios)?|_files_(bios|uefi)|_no_uart)?$' --output-on-failure` passed all nine checks.
Elapsed suite time was 37.48 seconds. The formatted source and document links passed checks.
Host tests were not repeated because host-tested code did not change.

The same host, toolchain, firmware, four-CPU QEMU settings, and 128 MiB RAM described above were used.
One baseline and one final run per boot path executed `echo hello world` after the initial prompt.
Counts include command input echo and the next prompt; the command output alone changes from 18 to 12 bytes.
The output-byte reduction meets the requested behavior; these samples make no latency or CPU-use claim.

| Boot | Baseline console bytes | Final console bytes | Change |
| --- | ---: | ---: | ---: |
| BIOS | 40 | 34 | -6 (-15.00%) |
| UEFI | 40 | 34 | -6 (-15.00%) |

| Artifact | Baseline bytes | Final bytes | Change |
| --- | ---: | ---: | ---: |
| `kernel.elf` | 471,128 | 471,128 | +0 (+0.00%) |
| `initrd.cpio` | 201,728 | 201,728 | +0 (+0.00%) |
| `user/sh.elf` | 54,192 | 54,192 | +0 (+0.00%) |

Samples, logs, task-specific snapshots, and the isolated diff are in ignored `build/echo-evidence/`.
Final shell source SHA-256: `d636a565f1e9b59bb9a24e20a0247282a26f9084e4d40626a9be9dcb943d3b07`.
Independent review found no code or test defects; the reviewer did not edit files or run tests.
Hardware and full ASD-STE100 dictionary verification remain unavailable.

## Silent here-document follow-up plan — 2026-11-06

Remove the `heredoc>` prompt; preserve input echo, delimiter handling, errors, and file contents.
The baseline includes both preceding quiet-output changes in this working tree.
The scope and GOALS Milestone C assessment above still apply: only shell presentation and test synchronization change.
There are no kernel, ABI, grant, allocation, concurrency, or boot-contract changes.
Update the guide and existing UEFI/BIOS interactive checks; measure console bytes and artifact sizes against this baseline.

## Silent here-document result

Collection prints no `heredoc>` prompt. Console input echo and failure diagnostics remain unchanged.
`cmake --build build` and both `os1_smoke_files_` checks passed; suite time was 5.14 seconds.
Checks reject the removed prompt and retain exact file-content, delimiter, EOF, overflow, and nested-shell assertions.
The test batches contain at most six input records, below the eight-record console queue limit.
No host-tested code changed, so host tests were not repeated.

One baseline and one final run per boot path entered `cat << END > /tmp/probe`, `hello`, and `END`.
Configuration, host, toolchain, firmware, and memory limits match the preceding measurements.
Counts include input echo, unchanged kernel logging, and the next shell prompt.

| Boot | Baseline console bytes | Final console bytes | Change | Here-document prompts |
| --- | ---: | ---: | ---: | ---: |
| BIOS | 77 | 59 | -18 (-23.38%) | 2 -> 0 |
| UEFI | 77 | 59 | -18 (-23.38%) | 2 -> 0 |

| Artifact | Baseline bytes | Final bytes | Change |
| --- | ---: | ---: | ---: |
| `kernel.elf` | 471,128 | 471,128 | +0 (+0.00%) |
| `initrd.cpio` | 201,728 | 201,728 | +0 (+0.00%) |
| `user/sh.elf` | 54,192 | 54,240 | +48 (+0.09%) |

Raw samples, logs, snapshots, and isolated diff are in ignored `build/heredoc-evidence/`.
Final file helper SHA-256: `7349630580524e9abecead1338e5199b05646b5c337fe5226e63bda91ccadae1`.
The measured output reduction meets the request; no timing or CPU-use improvement is claimed.
Hardware and full ASD-STE100 dictionary verification remain unavailable.
Independent read-only review found no defects or weakened assertions in the code, tests, guide, and final measurement record.
