# Completion redraw correction

The local console must replace completion choices in place.
Serial output can retain sequential lists when cursor movement is unavailable.

```text
completion state -> bounded rows -> Console.redraw_line -> local cell buffer
                                                     -> serial text lines
```

## Cause and acceptance

Baseline `3387fe5` starts each completion display with a newline and prints another list.
Selection and object descent therefore scroll the local console on every update.
The baseline serial transcript and framebuffer capture reproduce this behavior.

Reserve one bounded display area when completion opens.
Reuse that area for selection, descent, shorter lists, and member details.
Clear stale rows when the list closes and restore the command line.
Keep input ownership, command acceptance, scripts, and discovery semantics unchanged.
Keep serial output readable without requiring cursor escape sequences.

## Impact assessment

| Area | Requirements and change |
| --- | --- |
| Scope | GOALS: Terminal-first operating system and Native system interface; correct completion presentation without expanding discovery |
| Boot/platform | Architecture: Live context exploration and console; support both VGA/BIOS and framebuffer/UEFI |
| Memory/isolation | Bounded copied row request; validate movement, text length, and printable bytes before mutation; require Console WRITE |
| Concurrency | Serialize each row under the existing console output lock; retain existing input-session lifetime; no new display lease |
| Interfaces | Append Console `redraw_line`; preserve existing member layouts; serial output remains sequential |
| Resources | Compare image size, physical memory, redraw latency, and row changes against `3387fe5`; no storage format changes |
| Validation/docs | Host cell-buffer and IPC boundary checks; UEFI/BIOS completion, pager, input, and full regressions; independent review |

No scheduler, address-space, firmware, device enumeration, or filesystem contract changes are planned.
The [Console ABI](../native-api/abi/kernel-console-abi.md#local-row-redraw) defines the new row operation.
The [discovery profile](../native-api/context-discovery.md) defines completion behavior.
The [Native API index](../native-api/README.md#authority-and-reading-order) separates implemented profiles from proposals.
General reflection remains a proposal.

## Sequence

1. Preserve baseline images and reproduce repeated scrolling.
2. Add a bounded native row redraw operation with local replacement and serial text output.
3. Replace the completion renderer and handle long lines without wrapping its display area.
4. Check cell contents, cursor bounds, stale-row removal, cancellation, and input compatibility.
5. Measure resource changes and obtain an independent review.
6. Commit, push, and update PR #42.

## Evidence

The first independent review found stale menus after ignored or incomplete escape sequences.
The same cleanup gap affected input overflow.
A shared cancellation path now clears reserved rows before input continues.
The second review found nested input-error exits that bypassed cleanup.
A scope guard now restores the command row before the input session closes.
The third review found incorrect cursor assumptions after a partial output failure.
The presenter now tracks each successful row write.
A regression injects failure at each row and verifies that cleanup preserves earlier history.
The fourth read-only review found no further substantive defect.
The new guest regression failed against the earlier correction image and passed after this change.
The regression checks ignored Delete sequences and incomplete CSI sequences with an open menu.

Host checks cover cell contents, cursor bounds, short-row cleanup, page limits, invalid requests, authority, and retained results.
QEMU screen captures compare history pixels before and after selection and object descent.
The baseline capture fails with `selection scrolled console history`.
The corrected VGA and framebuffer captures preserve history and clear unused rows.
Serial checks retain sequential output.

### Commands and configuration

```sh
cmake --build build
cmake --build build-host-tests -j 4
ctest --test-dir build-host-tests --output-on-failure
ctest --test-dir build -R context --output-on-failure
ctest --test-dir build --output-on-failure
ctest --test-dir build -R 'context|observe|memory_logging|pager|files_|typed_shell|text_tools|tree_tools|timestamps|system_lifecycle' --output-on-failure
```

Local logs and screen captures are in `build/redraw-evidence/` and `build/artifacts/smoke-context-*`.
These generated files are not versioned.
The measurement script is `build/redraw-evidence/benchmark.py`.
It uses the context smoke workload with 20 additional Down selections and 20 matching Up selections.
It times the Down selections through serial prompt completion, then records `sys` and `kmem`.

Both builds use GCC 11.2.0, Debug configuration, and enabled kmem checks.
User programs use `-Os` and LTO; kernel optimization settings vary by target.
The host runs macOS 26.6.2 on arm64.
QEMU 11.1.3 uses TCG, q35, four CPUs, and 128 MiB of guest memory.
BIOS uses the raw image; UEFI uses Limine 11.4.0 and Homebrew OVMF code and variable images.
Each guest receives the same program disk with QEMU snapshot mode.
Trials run sequentially, with three repetitions per revision and boot path.
These timings include terminal rendering, serial output, IPC, scheduling, and host harness overhead.
They do not measure isolated method latency or physical hardware performance.

### Results

The final host suite passed 545 of 545 tests in 13.58 seconds.
The full guest suite passed 66 of 66 checks in 402.66 seconds before the final error-cleanup changes.
Those later changes affect prompt cleanup and cursor tracking, not kernel or storage behavior.
The final affected guest suite passed 26 of 26 checks in 108.72 seconds.
It covers completion, observation, memory logging, pager, lifecycle, typed commands, text tools, traversal, timestamps, and file operations.
Both boot paths and four-CPU guests are covered.
The earlier partial full run was stopped before rebuilding; it is not counted as a completed suite.

The candidate is the correction commit that introduces this record.
The baseline is `3387fe55767133ebf299e164bcfa7aea9b5f9036`.
The following table records three trials per revision and boot path, in milliseconds for 20 selection updates.

| Boot path | Baseline trials | Candidate trials | Median difference |
| --- | --- | --- | --- |
| BIOS/VGA | 753.124, 737.951, 741.043 | 55.461, 55.322, 55.543 | 741.043 to 55.461; -685.583 ms (-92.52%) |
| UEFI/framebuffer | 460.700, 467.410, 465.229 | 33.367, 88.358, 41.460 | 465.229 to 41.460; -423.768 ms (-91.09%) |

UEFI timings vary across trials; these measurements establish this workload result, not a general latency guarantee.
The reported retained-resource metrics match across all three trials.
Transient allocation counters differ, as expected from the additional row requests.

| Resource | Baseline | Candidate | Difference |
| --- | ---: | ---: | ---: |
| BIOS used physical pages | 945 | 945 | 0 pages (0%) |
| UEFI used physical pages | 5,303 | 5,303 | 0 pages (0%) |
| Slab pages | 168 | 168 | 0 pages (0%) |
| Live slab objects | 2,335 | 2,335 | 0 objects (0%) |
| Kernel ELF | 555,216 bytes | 555,328 bytes | +112 bytes (+0.0202%) |
| Initrd | 132,608 bytes | 134,144 bytes | +1,536 bytes (+1.1583%) |
| ISO | 4,415,488 bytes | 4,417,536 bytes | +2,048 bytes (+0.0464%) |
| Raw BIOS image | 1,474,560 bytes | 1,474,560 bytes | 0 bytes (0%) |

The initrd remains below the 147,456-byte slot limit.
The host failure-injection test preserves history at all ten possible row-write failures.
The guest pixel tests enforce unchanged history during selection and descent.
These checks satisfy the local redraw requirements for the tested QEMU configurations.

Artifact SHA-256 values:

```text
baseline kernel.elf  29b6bacf93480c9aff2e9e502dcbbb3b6a2f77a02924fc57436aa2b1ef4b7253
candidate kernel.elf aa3833481abc4120306a666a31cd7436b7b95159e3367466a4e2923bb820419d
candidate initrd.cpio 51384ec528a550cd442e953fe1252d817de2ca08c2f631ef077c11842fbc69e1
```

Formatting checks, changed-document links, and credential scans pass.
Independent review covered the complete diff and each correction.
All three reported defects are fixed; the fourth review found no substantive blocker.

### Limits

Each redraw request holds the existing output lock for one row.
The operation does not reserve the whole display against concurrent output.
Opening the menu can scroll once to reserve space; navigation reuses that space.
The presenter clips long command lines for display and retains the complete input value.
The copied row request adds transient IPC allocation traffic.
The bounded row operation reduces per-character display updates.
Physical hardware remains a separate qualification step.
The complete ASD-STE100 Issue 9 dictionary is unavailable; prose review uses the repository rules.
