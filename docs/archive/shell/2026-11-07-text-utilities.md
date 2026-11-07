# Text utility implementation — 2026-11-07

This stage adds `head`, `tail`, `grep`, `wc`, and `file` with their accepted object-method bindings.
PR #36 was merged at `0c3f75386efc3eab176172bba08fe5b6439f973e` before this work.

```mermaid
flowchart LR
    A[Aliases and typed arguments] --> B[Local text and fs helpers]
    B --> C[Bounded streaming algorithms]
    C --> D[Existing File and stream handles]
```

## Scope and decisions

| Alias | Method | Parameters |
| --- | --- | --- |
| `head` | `text.head` | Optional positional `source`; named Integer `lines`, default 10 |
| `tail` | `text.tail` | Optional positional `source`; named Integer `lines`, default 10 |
| `grep` | `text.search` | Required positional `pattern`; optional positional `source` |
| `wc` | `text.count` | Optional positional `source` |
| `file` | `fs.identify` | Required positional String `path` |

A source is a path or readable standard-stream reference.
An omitted source uses `process.streams.input`.
The alias argument `-` also selects standard input.
Arguments use the existing mixed positional/named binder and strict script rules.
Line counts must be nonnegative Integers.
This stage adds no short options, regular expressions, general glob expansion, or pipelines.

LF separates lines; an unterminated final fragment counts as a line for selection.
`wc` counts LF bytes, ASCII-whitespace-separated words, and all bytes.
`grep` performs case-sensitive literal matching within each line and preserves matched bytes.
Its pattern can be empty but cannot contain LF.
No match returns status 1 without an error diagnostic; scripts stop as with other nonzero commands.
I/O and validation failures retain the shell's existing failure behavior.

The algorithms process bounded chunks.
Seekable `tail` scans backward from a captured end offset.
Seekable `grep` scans lines and rereads matched ranges through the retained handle.
Non-seekable inputs use one 64 KiB workspace.
Stream `tail` retains the suffix and fails if the requested output exceeds that workspace.
Stream `grep` fails on an unmatched line prefix that exceeds the workspace before a match can be determined.
Earlier emitted lines remain after errors.
These operations do not promise a content snapshot during concurrent writes.

`file` uses metadata and at most 4096 content bytes.
It reports directory, empty, known ELF/CPIO signatures, ASCII text, or unknown binary data.
Classification does not validate executable safety or an entire file format.
Input handles are retained before redirection; same-object input/output is rejected before truncation.
Display output contains no unescaped filename or control bytes; content output preserves bytes.

## Impact assessment

This work supports GOALS “Terminal-first operating system” and “Kernel and userland separation.”
It follows ARCHITECTURE “Typed process context and shell bindings” and the accepted utility profile.
The Native API index keeps implemented protocols separate from future reflection and language proposals.

| Area | Assessment |
| --- | --- |
| Scope | Add five small local helpers and aliases; keep kernel services and shell language separate |
| Boot/platform | No firmware, BootInfo, mappings, or driver changes; larger shell affects initrd size and both boot images |
| Memory/isolation | Bounded chunks/workspace, retained inputs, checked arguments, handle rights, and partial-I/O handling |
| Concurrency | No new locks or kernel lifetime rules; existing handles protect identity, not a content snapshot |
| Interfaces | Reuse native interfaces; preserve seek rights for file redirection; extend local methods, aliases, and no-match status handling |
| Runtime resources | Compare shell/initrd sizes, common workload timing, allocator state, and bounded workspace costs |
| Validation/documents | Host algorithm tests, BIOS/UEFI integration, redirection regressions, strict scripts, contract documentation, independent review |

Pager behavior, timestamps, permissions, links, credentials, and general language expressions remain outside this stage.

## Validation plan

Preserve `build/filesystem-traversal` as the baseline; its source tree matches the merged PR #36 tree.
Build the candidate in `build/text-utilities` with Debug, allocator diagnostics, and debugcon.
Use q35, four CPUs, 128 MiB RAM, and both BIOS and UEFI.
Measure three alternating baseline/candidate trials per firmware with the existing common shell workload.

Test empty input, zero lines, missing final LF, chunk boundaries, long lines, literal patterns, ASCII whitespace, and binary bytes.
Test short reads/writes, I/O failures, count limits, current stream positions, and non-seekable buffer limits.
Test aliases, canonical methods, mixed arguments, redirected input/output, source preservation, and strict scripts.
Check known file signatures and bounded unknown-file classification.
Run affected shell regressions and independent review before publication.

The full ASD-STE100 dictionary is unavailable.
Manual prose review follows project rules but does not certify full standard compliance.

## Results

The branch is `codex/text-utilities`.
The baseline commit is `0c3f75386efc3eab176172bba08fe5b6439f973e`.
Its tree matches the tested PR #36 head `db34bf655b4221ba0b73530aadf3380cfc3aeafb`.
The candidate is the commit that contains this record.
The [current guide](../../shell/text-utilities.md) defines implemented behavior and limits.

### Checks

| Check | Result |
| --- | --- |
| Host tests | 472/472 pass |
| Initial full configured matrix | 48/52 pass; four interactive-file checks expose the copy-diagnostic regression |
| Corrected affected checks | 13/13 pass, including all four failures, both text tests, typed shell, traversal, packaging, and exec |
| Distinct configured checks | All 52 have passing evidence across the full and corrected runs |
| Formatting and whitespace | Scoped clang-format and `git diff --check` pass |
| Local document targets | 255 checked; none missing |
| Build and deployment contracts | 3/3 pass after the new fixture and explicit header dependencies |
| Hardware | Unmeasured; no physical-hardware claim |

Both firmware paths pass the 81927-byte redirected-line regression after the seek correction.
The full suite took 320.58 seconds; the affected correction suite took 41.99 seconds.
No pre-existing failure was observed or test assertion weakened.

Exact commands:

```sh
cmake -S . -B build/text-utilities -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-elf.cmake \
  -DCMAKE_BUILD_TYPE=Debug -DOS1_KMEM_DEBUG=ON -DOS1_QEMU_DEBUGCON=ON
cmake --build build/text-utilities -j 8
cmake --build build-host-tests -j 8
ctest --test-dir build-host-tests --output-on-failure -j 8
ctest --test-dir build/text-utilities --output-on-failure -j 4
ctest --test-dir build/text-utilities --output-on-failure \
  -R 'text_tools|typed_shell|tree_tools|smoke_files|program_packaging|smoke_exec'
```

The full matrix uses local socket access for QEMU monitor tests.
Guest transcripts and test-owned disks remain under `build/text-utilities/artifacts/`.
Host, full-suite, final correction, and measurement evidence resides under `build/text-utilities-evidence/`.
No physical-hardware test was performed for this stage.
Boot the candidate image and exercise these commands on the hardware to close that validation gap.

### Review and corrections

The first independent review covered algorithms, rights, redirection, argument binding, scripts, tests, and contracts.
It reported two P2 findings:

| Finding | Correction and evidence |
| --- | --- |
| Redirected files lost SEEK and selected bounded stream algorithms | Preserve SEEK in file and here-document input grants; add an 81927-byte line test for path and redirected sources |
| Shared input preparation changed the existing copy diagnostic | Restore `cat/copy failed to open source`; retain the existing interactive-file assertion |

The redirected long-line test reproduced `OS1_ERR_LIMIT` before the correction.
An earlier implementation review found an exact-capacity stream-tail boundary error.
A regression test reproduced that error; the corrected ring retains its preceding byte.
The host suite passes with that correction.
The first count expectation used 36 bytes for a 33-byte fixture; the expectation was corrected without changing the fixture.
A large-line test initially used tmpfs output beyond its documented 64 KiB file limit; its output now uses ext2.

The second independent review confirmed both preceding corrections.
It identified a P2 readable-stdout retention defect and a P3 measurement-table formatting defect.
A native parent can grant READ on descriptor 1; output redirection then replaces that descriptor.
The new disk-resident `textcheck` fixture reproduced `OS1_ERR_ACCESS` after destination truncation.
Execution now uses the saved source handle when output redirection replaces descriptor 1.
The fixture covers `head`, `tail`, `grep`, `wc`, `cat`, and same-object rejection.
Copy prevalidation now distinguishes its input reference from the redirected output slot.
The table spacing is corrected.

Build inspection also found missing explicit dependencies for the new headers.
Both text headers now appear in the user-program build dependencies, confirmed with `ninja -t query`.
The third independent review reported no findings.
It covered the full diff, corrections, copy interactions, tests, build dependencies, contracts, and measured evidence.
All substantive review findings are resolved.
The reviewer made no edits and ran no tests; validation conclusions use the supplied logs.

### Resource comparison

The host runs Darwin 25.6.0 on arm64.
The cross compiler is GCC 11.2.0; CMake is 4.4.5; QEMU is 11.1.3 using TCG.
Both builds use Debug, `OS1_KMEM_DEBUG=ON`, and `OS1_QEMU_DEBUGCON=ON`.
Each guest uses q35, four CPUs, 128 MiB RAM, and a fresh program disk.
BIOS uses QEMU's supplied firmware.
UEFI uses `/opt/homebrew/share/qemu/edk2-x86_64-code.fd` and fresh `edk2-i386-vars.fd` copies.

Three alternating baseline/candidate trials ran for each firmware path after all build and test processes stopped.
The common workload performs ten echo/cat/list cycles, synchronization, and two allocator snapshots.
CPU time includes boot, workload, and QEMU shutdown.
The table gives medians in seconds.

| Measurement | Baseline | Candidate | Absolute difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| BIOS boot | 0.417054 | 0.423555 | +0.006501 | +1.56% |
| BIOS workload | 0.666217 | 0.674977 | +0.008760 | +1.31% |
| BIOS QEMU CPU | 1.203123 | 1.217451 | +0.014328 | +1.19% |
| UEFI boot | 1.874178 | 1.866382 | -0.007796 | -0.42% |
| UEFI workload | 0.258395 | 0.259527 | +0.001132 | +0.44% |
| UEFI QEMU CPU | 2.187202 | 2.175260 | -0.011942 | -0.55% |

These short trials measure common-workload overhead, not text-helper throughput; the baseline has no text helpers.
They do not establish physical-hardware timing or a performance improvement.
This stage has no numerical latency threshold.
BIOS workload time changed +1.31%; UEFI workload time changed +0.44% in these samples.

| Artifact | Baseline bytes | Candidate bytes | Absolute difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| `kernel_bios.elf` | 389376 | 389376 | +0 | +0.00% |
| `kernel.elf` | 520640 | 520640 | +0 | +0.00% |
| `initrd.cpio` | 80896 | 86528 | +5632 | +6.96% |
| `os1.iso` | 4329472 | 4335616 | +6144 | +0.14% |
| `os1.raw` | 1474560 | 1474560 | +0 | +0.00% |
| `user/sh.elf` | 88960 | 95184 | +6224 | +7.00% |
| `user/textcheck.elf` | 0 | 21360 | +21360 | undefined |
| Shell text/data/BSS total | 321237 | 392078 | +70841 | +22.05% |
| Shell BSS | 246224 | 311760 | +65536 | +26.62% |

Both kernel artifacts remain byte-identical to the baseline.
Kernel text/data/BSS remains 569344 bytes: zero absolute change and 0% relative change.
The shell adds one 65536-byte workspace; chunk buffers and the pattern table use bounded stack storage.
This trades memory in every shell instance for bounded non-seekable input handling without dynamic allocation.
The BIOS kernel and initrd remain below their 409600-byte and 163840-byte staging limits.

Both builds use 166 slab pages before the workload and 168 afterward: zero difference and 0% relative change.
Baseline live objects increase from 2331 to 2332; candidate live objects increase from 2332 to 2333.
The candidate has one additional live object at each endpoint, approximately +0.04%.
Free slots decrease by one: 482 versus 481 before, and 486 versus 485 afterward, approximately -0.21%.
Allocation failures remain zero; their relative change is undefined because the baseline is zero.
These kernel allocator counters do not include the shell's additional user BSS pages.

The new disk-only `textcheck` fixture has 36060 bytes of text/data/BSS.
Its baseline is absent; the relative footprint change is undefined.
The program-test image remains fixed in size.
Allocated ext2 blocks grow from 294912 to 319488 bytes: +24576 bytes (+8.33%).

Reproduce the comparison with:

```sh
python3 -B tests/scripts/measure_shell_context.py \
  --baseline build/filesystem-traversal/artifacts \
  --candidate build/text-utilities/artifacts \
  --output build/text-utilities-evidence/measurements \
  --qemu /opt/homebrew/bin/qemu-system-x86_64 \
  --code /opt/homebrew/share/qemu/edk2-x86_64-code.fd \
  --vars /opt/homebrew/share/qemu/edk2-i386-vars.fd
```

`artifacts.json` records image sizes and SHA-256 hashes beside the local logs.
`measurements/measurements.json` records every trial, and serial logs preserve both allocator snapshots.
