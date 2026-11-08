# Guided commands and object lifetimes

This plan connects live discovery to typed invocation and an editable command line.
The accepted design keeps browsing passive and makes each exposed method callable.

```text
live bindings + executable parameter definitions
  -> object choices -> expanded editable arguments -> expression evaluation
  -> invocation -> displayed temporary result or retained named binding
history -> editable draft -> the same evaluator
exclusive result -> scope/foreground activity -> release -> prompt
```

## Accepted interaction

Tab explores objects and accepts leaves, then closes the list.
Method selection expands all parameters, including defaults and explicit missing values.
Tab and Shift-Tab move between parameter values when no list is open.
An open list takes precedence; Tab accepts its choice without advancing another parameter.
Ctrl-Space opens contextual choices inside a parameter.
A dot opens children outside strings and numeric literals.
Enter accepts an open choice; Enter on the command line validates and executes.
Escape closes choices or leaves parameter focus without discarding the command.
Ctrl-C cancels the command.

Strings start with editable quotes.
Arguments can contain fields, properties, and nested method calls with parentheses.
Nested calls use `name:value` arguments.
Completion does not execute calls, read getters, or instantiate objects.
Return-type metadata can describe children without constructing a result.
Dynamic result contents require explicit evaluation and a binding.

Left/Right and Home/End move within the focused value or whole line.
Typing inserts; Backspace/Delete remove text around the cursor.
Up/Down recall history only outside choices and parameter focus.
History navigation preserves the unfinished draft and never changes recorded commands.
The command buffer holds at most 1023 bytes plus a terminator.
Local display uses a bounded horizontal viewport; serial retains sequential snapshots.

Required placeholders use `<?>`, with editable quotes for String parameters.
Optional omission uses `unset`; a declared default then applies.
Placeholders prevent execution.
Enums and variants use declared choices; variant branches use dotted names.
Completed command text has the same meaning when typed, recalled, or used in scripts.

## Invocation and ownership

Evaluate nested arguments once, from left to right.
Stop on the first error and release temporary references.
Validate argument types before invoking the enclosing method.
Ordinary command calls wait for completion; explicit background Operations remain later work.
Bare fields and properties display their values interactively.
Scripts use explicit output commands for value expressions.

`name = expression` retains a shell-local binding.
`process.shell.bindings.remove name:"name"`, with alias `drop`, removes that reference.
Bindings appear in live discovery but do not enter the inherited plain-data environment.
The names `true`, `false`, `unset`, `with`, and `drop` are reserved.
Unassigned object results are displayed and released.
Replacing a binding releases its previous reference after the new result succeeds.

Exclusive sessions require a `with` scope or foreground handoff.
Reject persistent exclusive assignment before acquisition.
A scope releases exclusivity on success and error; release is idempotent.
A scope-local session cannot escape into a persistent binding.
A scope owns its binding name until cleanup; commands cannot remove or replace that name.
Surviving references become inactive after release.

The prompt resumes only after it can acquire input again.
Foreground applications keep their existing input ownership and process-cleanup contracts.
Forced cancellation of a foreground input owner requires separate supervision and is not implied by Ctrl-C.

## Impact assessment

| Area | Requirements and assessment |
| --- | --- |
| Scope | GOALS: Terminal-first operating system, Native system interface, Simplicity first; shared bounded definitions connect discovery and execution |
| Boot/platform | Architecture: Live context exploration; validate BIOS/VGA and UEFI/framebuffer; no firmware or BootInfo changes |
| Memory/isolation | Bound expressions, bindings, line length, nesting, and result storage; preserve descriptor rights; release all owned results |
| Concurrency | Preserve Console input ownership and row serialization; scope cleanup precedes prompt acquisition; no new scheduler policy |
| Interfaces | Extend local shell grammar and discovery; add bounded cursor positioning if needed; preserve copied IPC and existing method identities |
| Resources | Measure images, retained memory, and command latency against 88b5556; verify initrd slot and user stack limits |
| Validation/docs | Host parser/editor/schema/lifetime tests; guest invocation, focus, history, ownership, and both display paths; independent review |

The Native API index distinguishes implemented ABI profiles from accepted proposals.
Relevant contracts are context discovery, kernel Console, memory logs, typed context, and rich parameters.
Storage formats, device drivers, process isolation, and provider activation policy remain unchanged.

## Implementation sequence

1. Record executable parameter definitions and bounded expression/binding contracts.
2. Connect all exposed native and local methods to typed invocation.
3. Add scoped exclusive results, cleanup, and discovery of bindings.
4. Implement cursor editing, guided arguments, recursive choices, and history recall.
5. Update contracts and regression checks.
6. Measure resource changes, obtain independent review, commit, push, and update PR 42.

## Validation evidence

The final host suite passed 560 tests in 13.67 seconds.
The full guest suite passed 70 checks in 466.50 seconds before the final argument-role and reserved-name corrections.
The final affected suite passed 24 checks in 136.58 seconds on the corrected binaries.
It covers both boot paths, four CPUs, serial input, PS/2 keys, USB keys, logs, lifecycle, pager, and file tools.
The final independent correction review found no remaining defects.
All confirmed findings from the five review cycles have corrections and affected validation.

Exact validation commands:

```sh
cmake --build build
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
ctest --test-dir build --output-on-failure
ctest --test-dir build -R 'os1_smoke_(guided|context|typed_shell|text_tools|pager|memory_logging|system_lifecycle|traversal|files_)' --output-on-failure
```

The earlier partial full run stopped before rebuilding a property-traversal correction.
It is not counted as a completed suite.
No known pre-existing failure affected the completed local suites.
Changed source files were explicitly selected for `clang-format`.
Local document targets, Python syntax, diff whitespace, and credential checks passed.

### Runtime limits

| Resource | Bound |
| --- | --- |
| Command text | 1023 bytes plus NUL |
| Expression nodes | 96, including the reserved root slot |
| Expression depth | Eight |
| Scope depth | Eight |
| Shell bindings | 16, subject to the 256-member discovery budget |
| Guided parameter ranges | 64 |
| Parameters per method | Eight |
| Enum or variant choices | Eight |
| History | Existing 32 records; no second history ring |
| Typed data | Existing 48 nodes and 2048 text bytes; individual parsed strings retain their 255-byte limit |

The prompt length does not increase the canonical reader's 127-byte text limit.
The prompt uses raw input; heredoc and other canonical readers retain their existing contract.
Fast input can exceed the 256-byte prompt queue and returns RANGE without executing a truncated command.
Local stream adapters preserve existing file and text-tool limits.
Console `write` accepts a String and sends its bytes; it does not introduce a general binary literal syntax.

### Comparable measurements

The baseline revision is `88b55566e57f5a22041e36514abd66df653a3e70`.
The candidate is the implementation in this record's commit; artifact hashes identify the measured binaries.

The host uses an Apple M4 Max, 16 CPU cores, 64 GiB RAM, and macOS 26.6.2.
QEMU 11.1.3 uses TCG, q35, four guest CPUs, and 128 MiB guest RAM.
Both revisions use GCC 11.2.0, the Debug CMake configuration, and the existing user-program `-Os -flto` flags.
BIOS uses the project loader and VGA.
UEFI uses the installed QEMU EDK2 firmware and Limine 11.4.0 framebuffer boot.
Both attach the same program-test disk through virtio-blk with snapshot writes.

Each boot performs ten warm-up echo commands, then three trials of 20 echo commands and three trials of 20 completion selections.
Times measure host-observed round trips through serial input and output.
They include scheduling, formatting, IPC, QEMU, and host overhead; they are not isolated syscall timings.

| Workload | Baseline trials, ms | Candidate trials, ms | Median difference |
| --- | --- | --- | --- |
| BIOS, 20 echo commands | 155.600, 183.023, 156.830 | 147.221, 147.991, 145.958 | -9.609 ms (-6.13%) |
| UEFI, 20 echo commands | 85.596, 110.384, 110.252 | 111.772, 117.632, 116.052 | +5.800 ms (+5.26%) |
| BIOS, 20 selections | 56.121, 61.371, 56.095 | 69.104, 75.452, 66.030 | +12.983 ms (+23.13%) |
| UEFI, 20 selections | 72.612, 84.050, 75.624 | 70.043, 49.078, 56.300 | -19.324 ms (-25.55%) |

Timing variation prevents a general performance claim.
The measured selection median remains below five milliseconds per update in each configuration.

| Resource after the workload | Baseline | Candidate | Difference |
| --- | ---: | ---: | ---: |
| BIOS QEMU CPU time | 1.498327 s | 1.498768 s | +0.000441 s (+0.03%) |
| UEFI QEMU CPU time | 2.551989 s | 2.572161 s | +0.020172 s (+0.79%) |
| BIOS used physical pages | 945 | 1070 | +125 pages (+13.23%) |
| UEFI used physical pages | 5303 | 5428 | +125 pages (+2.36%) |
| Slab pages, both paths | 168 | 168 | 0 (0%) |
| Live slab objects, both paths | 2335 | 2335 | 0 (0%) |
| Kernel ELF | 555328 bytes | 555424 bytes | +96 bytes (+0.0173%) |
| Initrd | 134144 bytes | 143360 bytes | +9216 bytes (+6.87%) |
| UEFI ISO | 4417536 bytes | 4425728 bytes | +8192 bytes (+0.1854%) |
| BIOS raw image | 1474560 bytes | 1474560 bytes | 0 bytes (0%) |

The additional retained memory is 512000 bytes, or 500 KiB, in each guest.
Bounded evaluator, binding, and discovery storage accounts for this tradeoff.
Large recursive stack allocations caused an initial guest fault; bounded per-shell storage removed that fault.
The initrd remains below its 147456-byte slot without changing the boot layout.

Memory values are one retained-state observation per revision and boot path.
CPU time is one host process observation per revision and boot path, including boot and all benchmark commands.
Raw logs remain under `build/guided-evidence/{baseline,current}-{bios,uefi}.{txt,log}`.

### Review record

The first review found six defects in scope parsing, temporary cleanup, pager parameters, enum expressions, record validation, and empty backspace.
Each defect has a correction and regression coverage.

The second review found invalid optional-value access, scope-slot reuse, script pager admission, bare exclusive acquisition, and repeated argument expansion.
It also found an obsolete pager signature.
Each finding has a correction; subsequent reviews check these interactions.

The third review found a Boolean-name collision and incorrect stream selection for `text.search`.
Reserved binding names protect Boolean defaults; stream validation now uses the argument role.
The fourth review found that guided `source.stream` selection still inserted stdout.
Source selection now inserts stdin; destination selection inserts stdout.
The fifth independent review found no remaining defect in this correction or its argument-validation interactions.

Physical hardware requires separate qualification.
The complete ASD-STE100 Issue 9 dictionary is unavailable; prose review uses the repository rules.

### Artifact identity

```text
baseline kernel.elf aa3833481abc4120306a666a31cd7436b7b95159e3367466a4e2923bb820419d
baseline initrd.cpio 51384ec528a550cd442e953fe1252d817de2ca08c2f631ef077c11842fbc69e1
baseline os1.iso 204f125046d70ab18f3aebbed5600d8c4a309a56b7f4118f51120b8f4a4b9c6d
baseline os1.raw 5e7615b62ef65591f076fd73bce09858e3bd8740bf4a2ac8ea1aea6e3ec22a12
candidate kernel.elf 3bdaa2102c7ce8dc132029b465bc03a4c1a731aba43e78e8535f87e4e551594a
candidate initrd.cpio 8ae63d6cbafb680fa8f947cd9d08a978b5c38fa417a5b1e55bb7a92498675cf1
candidate os1.iso 27db43ebd077a6896bac419a868e86d0c521b033bf9d6f61545d850e6d86c3ff
candidate os1.raw 37051a216f0465e2d299a709c14fd0a15d7a894da0da098060728b64d5f95b61
```
