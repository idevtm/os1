# Interactive file tools implementation record — 2026-11-06

This record preserves the plan, impact assessment, and validation for interactive file tools.
The plan preceded implementation; completed behavior is specified in the maintained [guide](../../shell/interactive-files.md).
The baseline is `6496424c519c99caac2d2069da30a7c504852b53` on `codex/tmpfs` (PR 28).

```text
Console lines / Ctrl-D -> shell parser -> granted file resources
  -> touch / write / edit, or redirected built-in / child streams
Native File append + descriptor APPEND -> one atomic 256-byte append unit
```

## Scope and decisions

- Implement `touch`, interactive `write`, and `edit` as shell commands to share bounded parsing and file helpers.
- Implement `<`, `>`, `>>`, and `<< delimiter` for built-ins and external commands, with quote-aware operator recognition.
- Preserve raw typed constructor/method arguments, including their existing quote and struct syntax.
- Reject unsupported pipes, duplicate stream redirections, missing operands, and unterminated quotes before changing files.
- Restore shell streams after every command, including failures. Keep diagnostics and prompts outside redirected stdout.
- `>` creates or truncates; `>>` creates or appends; `<` opens for reading. Resolve paths through existing directory grants.
- Add native File `append` and shared descriptor APPEND status. Atomically choose end and write at most 256 bytes under the filesystem lock.
- Collect here-document input before opening output files. Use a bounded unlinked `/tmp` file as child stdin, then release it.
- Add canonical Ctrl-D: flush pending bytes without newline, or enqueue a consumable EOF record when the pending line is empty.
- Support Ctrl-D through serial, PS/2, and USB keyboards. EOF is an input record, not permanent console shutdown.
- Extend `cat` to copy stdin when no path is supplied, and accept `-` as stdin.
- `write path` accepts lines until a lone dot or Ctrl-D; `..` enters a literal dot line. Default creation refuses existing files.
- The line editor supports listing, append/insert/change/delete, explicit save, and guarded quit; it keeps unsaved text in bounded memory.
- Editor saves report partial failure and retain the edit buffer. No atomic replace or concurrent-edit conflict guarantee is claimed.
- Preserve existing filesystem quotas, read-only initrd, and image safety bounds. Measure image growth before changing packaging.
- If required, use compact position-relative userspace code while retaining fixed-address static ELF executables and loader permissions.

## Impact assessment

| Area | Assessment |
| --- | --- |
| Scope | GOALS storage/filesystems and shell usability; Native API Directory/File and console contracts become usable interactively. |
| Boot/platform | More user code affects the nearly full initrd. Preserve staging ceilings; check all executable images and both boot paths. |
| Memory/isolation | Bound parser, here-documents, and editor storage. Retain grant confinement; release temporary resources and restore redirected descriptors. |
| Concurrency | Atomic append at 256-byte units; canonical EOF queue ordering and read reservations; SMP readers/writers retain existing lifetime rules. |
| Interfaces | Extend File members and descriptor status; document console EOF. Keep public members snake_case and existing records compatible. |
| Resources | Capture baseline artifact sizes and repeated boot/command timing; compare final image and static buffer costs. |
| Validation/docs | Host parser/editor/append/EOF/keyboard tests, UEFI/BIOS interactive scenarios, full smokes, scoped formatting, and independent review. |

## Sequence

1. Capture baseline and verify packaging capacity.
2. Implement append and console EOF contracts with boundary tests.
3. Add pure bounded shell parser/editor components, resource helpers, commands, and stream redirection.
4. Exercise complete interactive flows, errors, cleanup, typed CLI compatibility, and boot paths.
5. Record measurements, update maintained contracts, obtain independent review, and publish the reviewed changes to PR 28.

Physical hardware remains unverified until the user tests a fresh image.
The complete ASD-STE100 dictionary is unavailable; this record follows repository prose rules without claiming full dictionary compliance.

## Implemented result

The commands, redirects, console EOF, native append, and line editor are implemented.
The maintained [guide](../../shell/interactive-files.md) defines commands and limits.
The [File ABI](../../native-api/abi/tmpfs-abi.md) and [console ABI](../../native-api/abi/kernel-console-abi.md) define protocol changes.

Writer and editor insertion errors drain through their terminator.
Here-document errors drain through the delimiter or EOF before returning failure.
Finite editor input exits at EOF; it reports unsaved discard instead of waiting for commands that cannot arrive.
Interactive console EOF keeps the guarded quit rule.

## Validation

| Check | Command or evidence | Result |
| --- | --- | --- |
| Images | `cmake --build build` | Passed unchanged BIOS staging and slot guards |
| Host build | `cmake --build build-host-tests` | Passed |
| Host tests | `ctest --test-dir build-host-tests --output-on-failure` | 370 passed; baseline had 355 |
| Complete integration | `ctest --test-dir build --output-on-failure` | 18 passed; final rerun recorded below |
| Interactive flows | `ctest --test-dir build -R '^os1_smoke_files_' --output-on-failure` | UEFI and BIOS passed |
| ELF packaging | Parse all 26 packaged ELF headers, segments, and sections | All fixed `ET_EXEC`; no dynamic segments or allocated relocations |
| ABI example | Cross-compiler `-fsyntax-only` on documented tmpfs example | Passed |
| Documents | Resolve changed local Markdown links | No broken local file links |
| Formatting | Explicit 27 owned C/C++ paths; changed ranges for pre-existing drift | Applied; no dependency formatting |

The interactive tests check exact file contents, quoting, redirection, append, EOF, here-documents, writer modes, and editor operations.
They cover dirty quit, failed insertion, finite-input EOF, diagnostics separation, and stream restoration.
Seventy repeated redirections check bounded descriptor cleanup; temporary here-document names never remain linked.
Host tests cover parser boundaries, transactional edits, concurrent append, EOF rollback, retained native results, and keyboard modifiers.
Existing smoke workloads cover typed CLI conversion and interface metadata after code-generation changes.

Local logs are under ignored `build/interactive-evidence/`.
`final-host-tests.log` and `full-smokes.log` contain the complete check results.
`smoke-files-bios.log` and `smoke-files-uefi.log` under `build/artifacts/` contain guest transcripts.
Physical hardware remains unverified. Keyboard Ctrl-D has decoder tests; the complete new interactive scenario uses serial input.

## Artifact and memory measurements

Baseline revision: `6496424c519c99caac2d2069da30a7c504852b53`.
Final artifact identities below identify the measured working tree before its publication commit.
Both builds used macOS 26.6.2 arm64, GCC 11.2.0, and the same Debug configuration.
Kernel C++ retained `-Os`; ACPICA C retained `-O1`; kmem debugging and QEMU debugcon remained enabled.
Userspace changed to `-Os -flto -mcmodel=small -fpie -fno-plt`, with static linking and disabled relaxation.
This reduces code size while retaining fixed-address executables and existing loader permissions.
Packaging also removes the nonallocated compiler `.comment` section.

| Artifact | Baseline bytes | Final bytes | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| `kernel.elf` | 470,928 | 471,128 | +200 | +0.04% |
| `kernel_bios.elf` | 352,768 | 352,768 | 0 | 0.00% |
| `initrd.cpio` | 203,776 | 201,728 | -2,048 | -1.01% |
| `os1.iso` | 4,401,152 | 4,401,152 | 0 | 0.00% |
| `os1.raw` | 1,474,560 | 1,474,560 | 0 | 0.00% |
| Packaged `/bin/sh` | 39,928 | 46,072 | +6,144 | +15.39% |
| Shell load-segment memory | 126,831 | 267,709 | +140,878 | +111.08% |
| Shell zero-filled segment bytes | 87,592 | 222,448 | +134,856 | +153.96% |

The shell memory figures sum ELF `PT_LOAD` sizes before page rounding and allocator overhead.
The editor accounts for two 64 KiB static text buffers; other growth includes state, code, and alignment.
The console adds eight `long` status entries, totaling 64 bytes, plus bounded modifier/overflow state.
Internal callback signatures change without adding function-pointer slots to native objects.
Here-documents use existing tmpfs quota and allocator accounting, with a 64 KiB logical file limit.
The editor buffers exist in shell BSS even when no editing session is active.

The 352 KiB kernel and 200 KiB initrd slots are unchanged.
Local remaining capacity is 7,680 kernel bytes and 3,072 initrd bytes.
All limits pass; larger future features still need an explicit packaging decision.

| Artifact | SHA-256 |
| --- | --- |
| `kernel.elf` | `1d1d37f71b5e26048b7b0e0153399e3f377c4a654ad801cdf877a449b78942e7` |
| `kernel_bios.elf` | `a07e6f36e3539fc67f2d6180eed19b448767f8562bd44f22d68e4bd8d7cd5997` |
| `initrd.cpio` | `500dd5fdaf69a963f8bee339472f0806bd17af1ae599d0e49104eb656411fe37` |

`baseline.json`, `artifacts.json`, and `formatting.json` preserve local details.
The benchmark and review sections below complete this evidence record.

## Independent review

A separate agent reviewed the complete diff and new files without editing or running tests.

| Cycle | Finding | Correction and regression coverage |
| --- | --- | --- |
| 1 | Overlong console input became a successful blank line. | Queue RANGE records; test descriptor/native delivery and writer/editor/here-document rejection. |
| 1 | Dirty editor input from a finite file looped at EOF. | Detect finite input; report unsaved discard and exit. Test redirected editor scripts and here-documents. |
| 1 | Extended PS/2 keypad Enter and slash stopped working. | Preserve those keys and shifted behavior. Add modifier regression tests. |
| 1 | Native call diagnostics entered redirected output. | Send diagnostics to stderr; test empty output files on call and parameter errors. |
| 2 | A child shell looped at finite-input EOF. | Exit cleanly at finite EOF; test empty input and scripts without `exit`. |
| 2 | Child shell prompts entered redirected output. | Send prompts and startup messages to stderr; compare redirected script output exactly. |
| 3 | No additional concrete defects. | Reviewer accepted the corrections and their interactions. |

All confirmed findings have corrections and regression coverage.
The final local integration rerun passed all 18 checks in 61.02 seconds.
The final host suite passed 370 tests in 3.47 seconds before the shell-only EOF correction.
That correction changes no host-tested code; the expanded guest tests passed afterward.
Secret scanning found no credentials; keyword matches were parser variable names.
Full ASD-STE100 dictionary compliance and physical hardware behavior remain unverified.

## Comparable runtime measurements

Each revision ran the unchanged spawn smoke workload three times per boot path.
The final run used the final initrd because this change modifies the shell and userspace code generation.
No test expectations were removed or weakened.
Elapsed time includes CTest startup, boot, commands, and settle time; it is not per-call latency.

Both configurations used QEMU 11.1.3, q35, four CPUs, software emulation, and 128 MiB default RAM.
UEFI used `/opt/homebrew/share/qemu/edk2-x86_64-code.fd` and the adjacent `edk2-i386-vars.fd` template.
BIOS used the same QEMU installation and default BIOS.
The toolchain and build settings are recorded above.

| Workload | Baseline samples, seconds | Final samples, seconds | Median change |
| --- | --- | --- | --- |
| `os1_smoke_spawn` | 10.115500, 9.986560, 9.979963 | 9.933517, 9.864944, 9.876601 | -0.109959 s (-1.10%) |
| `os1_smoke_spawn_bios` | 8.233711, 8.263351, 8.287048 | 8.266688, 8.235216, 8.265206 | +0.001855 s (+0.02%) |

edk2-x86_64-code.fd SHA-256: `33090cc07675baa5190d9f1e84bf5176b33bcbfa9bacac522961150cdb6dbb2a`.
edk2-i386-vars.fd SHA-256: `5d2ac383371b408398accee7ec27c8c09ea5b74a0de0ceea6513388b15be5d1e`.

All six final workloads passed. The small wall-time differences do not establish a performance improvement.
The shell uses single-byte descriptor reads to avoid consuming input intended for a foreground command.
That choice increases syscall count for long file scripts; throughput and CPU utilization remain unmeasured.
Dedicated script, editor, and concurrent-append benchmarks remain future work; these features had no equivalent baseline workflow.
Physical-device latency remains unmeasured for this revision.
Local `baseline-runs.json`, `final-runs.json`, and per-run logs preserve the samples.
