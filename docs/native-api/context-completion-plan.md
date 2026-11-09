# Live context exploration and completion

This slice exposes available runtime bindings and typed environment fields without activation.
The shell uses the same bounded discovery API for text inspection and Tab completion.

```text
descriptor queries + copied environment -> bounded runtime snapshot
snapshot -> describe / resolve / enumerate -> context command / prompt completion
Console input session -> prompt editor -> release with typeahead handoff -> command
```

## Scope and contract

The baseline is `cfc0ab8e4f3b407bc1c8190a129f59a9638c6b82`.
Implement the second slice of [context discovery](context-diagnostics.md#proposed-slices).
Keep discovery in the shared user runtime; the kernel supplies existing resource queries and input sessions.
This first fixed registry describes implemented bindings, not arbitrary remote providers or unactivated classes.
Expose `os`, `process`, and available short runtime bindings, with member kinds, types, signatures, and authority requirements.
Enumerate typed records and lists, including nested environment data.
Inspectable descriptors and current environment data determine visibility; metadata never grants authority.
Existing QUERY and FILE_INFO require INSPECT; invocation-only grants remain hidden.
Do not invoke getters, read file contents, probe devices, or create objects during discovery.
Snapshot refresh invalidates old cursors; bound nodes, paths, page size, and traversal depth.

Tab opens completion; Up/Down selects; Tab descends into objects or describes a leaf.
Enter accepts an open selection without execution; a later Enter submits the line.
Escape cancels completion. Ctrl-C cancels the input line.
The initial renderer used bounded scrolling lists on all consoles.
The [redraw correction](../shell/completion-redraw-plan.md) replaces local rows while keeping serial output sequential.
Keep script input, redirection, heredocs, and foreground input ownership compatible.
Reuse the Console input-session protocol, with explicit typeahead handoff for shell sessions only.
Existing pager sessions retain discard-on-release behavior.

## Impact assessment

| Area | Requirements and effects |
| --- | --- |
| Scope | GOALS: Native system interface, terminal-first interaction, observability; separate discovery from shell presentation |
| Boot/platform | Preserve both boot paths and image bounds; PS/2 and USB navigation keys need decoding |
| Memory/isolation | Bounded user snapshot and completion state; validate environment data, names, cursors, and descriptor rights |
| Concurrency | Preserve exclusive console ownership, waiter readiness, cleanup, bounded queues, and typeahead handoff |
| Interfaces | Extend Console acquisition with optional handoff flags; preserve empty requests and existing discard semantics |
| Resources | Measure image size, physical memory, command latency, and discovery work against the baseline |
| Validation/docs | Host discovery/editor/input tests; serial and hardware-input QEMU checks; full regressions; independent adversarial review |

## Implementation sequence

1. Capture baseline artifacts and measurements.
2. Implement bounded reflection over live runtime bindings and environment fields.
3. Add text inspection and interactive completion over the same descriptions.
4. Add prompt-session handoff and PS/2/USB navigation decoding.
5. Validate ownership, cancellation, authority filtering, stale cursors, and bounded input.
6. Measure resources, update contracts, and obtain independent review.
7. Commit, push, and open a new PR without merging it.

## Validation evidence

### Checks

The initial implementation passed 535 host tests and four focused context checks.
The corrected input-overflow implementation passed 536 host tests in 12.65 seconds.
The first full guest run passed 63 of 66 checks.
Two observation checks read boot events after extra prompt IPC activity had overwritten those records.
The observation sequence now reads the bounded event ring first; all original event assertions remain unchanged.
The new CRLF regression also reproduced the reviewed defect against the earlier image during that run.
After corrections, all 16 affected guest checks passed in 66.33 seconds.
Both serial context checks also passed without a data disk.
A QEMU framebuffer capture confirmed readable selection markers and object descent; see `build/context-evidence/completion.png`.
Logs use `corrected-focused.txt`, `context-nodisk-bios.log`, and `context-nodisk-uefi.log`.

```sh
cmake --build build
cmake --build build-host-tests -j 4
ctest --test-dir build-host-tests --output-on-failure
ctest --test-dir build --output-on-failure
ctest --test-dir build -R 'context|observe|memory_logging|pager|files_' --output-on-failure
```

Local evidence uses `build/context-evidence/host-tests.txt`, `full-smoke.txt`, and `context-tests.txt`.
The full suite covers UEFI, BIOS, four-CPU SMP, copied IPC, storage, lifecycle, text tools, and interactive input.
Focused context tests cover serial navigation, PS/2 keys, USB keys, CRLF acceptance, cancellation, nested environment fields, and typeahead.
Host tests cover bounded pages, stale generations, hostile names, aliases, authority filtering, queue loss, and acquisition options.
Existing pager tests retain default discard semantics.

Changed document links resolve locally. Source formatting uses the root configuration and explicit changed files.
The secret scan found code identifiers and existing explanatory terms, with no credentials.

### Independent review

The first independent review found two input defects.
Both findings were verified against their source paths.

| Finding | Correction and regression |
| --- | --- |
| Consumed RANGE lost the handoff overflow marker | Keep a separate loss flag until release; test consumed RANGE followed by newline and truncated queued input |
| CRLF accepted and executed a completion from one Enter | Coalesce CRLF during prompt editing; wait after acceptance and verify no command output before the second Enter |
| Lookup-only roots appeared to lose discovery authority | QUERY and FILE_INFO already require INSPECT; document inspectable grants explicitly and preserve that boundary |

The reviewer withdrew the third implementation finding after checking QUERY authorization.
Invocation-only grants remain hidden; discovery does not add inspection rights.
The second read-only review found no remaining code blocker.
The third review verified the observation sequence and validation evidence without finding a substantive blocker.
Its command-record correction is included in the commands above.

### Resource measurements

The baseline is `cfc0ab8e4f3b407bc1c8190a129f59a9638c6b82`.
The candidate is this branch's implementation; artifact hashes below identify the measured binaries.
Both builds use GCC 11.2.0, Debug configuration, `-Os`, LTO, and enabled kmem checks.
The host runs macOS 26.6.2 on ARM64; QEMU 11.1.3 uses TCG, q35, four CPUs, and 128 MiB RAM.
UEFI uses Homebrew `edk2-x86_64-code.fd`, copied `edk2-i386-vars.fd`, and Limine 11.4.0.
BIOS uses the repository raw image and QEMU firmware.
The workload runs three groups of 20 echo commands, followed by `sys` and `kmem`.
Host monotonic time measures command latency; the guest allocator reports free physical pages.

```sh
python3 tests/scripts/test_memory_logging.py --baseline \
  --qemu /opt/homebrew/bin/qemu-system-x86_64 \
  --image build/artifacts/os1.raw \
  --program-disk build/artifacts/program-test-disk.raw \
  --log build/context-evidence/candidate-bios.log
```

For UEFI, use `os1.iso` and the firmware paths above through `--code` and `--vars`.
Baseline runs use saved artifacts under `build/context-evidence/baseline` with the same workload and settings.
Text results use `baseline-bios.txt`, `baseline-uefi.txt`, `candidate-bios.txt`, and `candidate-uefi.txt` in the evidence directory.

The snapshot occupies 27,816 bytes; each member occupies 192 bytes; completion state occupies 680 bytes.
The kernel input backing array increases from 64 to 256 bytes: 192 additional bytes, or 300%.
The shell now uses retained IPC Operations for key input and ownership changes.
This increases allocation traffic and command latency while preserving bounded live storage.
Physical interrupt latency, hardware typing latency, and hardware CPU cost remain unmeasured.
The next hardware check must include rapid typing, pasted input, completion cancellation, and foreground pager ownership.

Physical keyboard behavior and physical interrupt latency require separate hardware qualification.
The complete ASD-STE100 Issue 9 dictionary is unavailable; language review uses the repository rules.

### Measured values

| Metric | Baseline | Candidate | Absolute difference | Relative difference |
| --- | --- | --- | --- | --- |
| BIOS median, 20 echo commands | 147.158 ms | 200.852 ms | +53.694 ms | +36.49% |
| BIOS used physical pages | 927 | 945 | +18 pages | +1.94% |
| UEFI median, 20 echo commands | 115.101 ms | 161.947 ms | +46.846 ms | +40.70% |
| UEFI used physical pages | 5285 | 5303 | +18 pages | +0.34% |
| `kernel.elf` | 555,064 bytes | 555,216 bytes | +152 bytes | +0.03% |
| `initrd.cpio` | 96,256 bytes | 132,608 bytes | +36,352 bytes | +37.77% |
| `os1.raw` | 1,474,560 bytes | 1,474,560 bytes | +0 bytes | +0.00% |
| `os1.iso` | 4,378,624 bytes | 4,415,488 bytes | +36,864 bytes | +0.84% |
| Slab pages after workload | 168 | 168 | 0 | 0% |
| Live slab objects after workload | 2335 | 2335 | 0 | 0% |
| Discovery View | 0 | 27,816 bytes | +27,816 bytes | Undefined: zero baseline |

BIOS baseline: 152.192, 147.158, 146.902 ms.
BIOS candidate: 200.852, 203.625, 194.772 ms.
UEFI baseline: 62.395, 115.101, 115.425 ms.
UEFI candidate: 118.166, 161.947, 164.789 ms.

The initrd remains below its 147,456-byte BIOS slot limit.
Additional physical memory is 73,728 bytes in both firmware paths.
These three trials measure emulator workload cost, not physical keyboard latency or isolated method execution.

| Candidate artifact | SHA-256 |
| --- | --- |
| `kernel.elf` | `29b6bacf93480c9aff2e9e502dcbbb3b6a2f77a02924fc57436aa2b1ef4b7253` |
| `initrd.cpio` | `47bf7e0d90ded12e40c7014150fa2eaad1c09c12025dc183e3dc08673116709c` |
| `os1.raw` | `9ac6e14043a2747a3ef8e9ebef67fa520da3a43f28b6b0b6b8a3e664239345af` |
| `os1.iso` | `0129f1f43a46e6c9d75fdd5e014f42d8fd904b475004d8744632d47c5ec4c977` |
