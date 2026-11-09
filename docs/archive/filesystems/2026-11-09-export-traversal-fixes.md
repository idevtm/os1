# Log export and traversal failure correction

This record preserves two PR 45 failures and the correction plan.
Validation results follow the implementation sections.

```text
unrelated log directory mutation -> scoped tree checks -> stable file command
active foreground syscall -> retained export request -> admission -> one execution
```

## Original evidence

| Run and job | Revision | Failure |
| --- | --- | --- |
| [42668031097](https://github.com/idevtm/os1/actions/runs/42668031097), job 129336817908 | `919dc0d` | Recursive copy stopped with `OS1_ERR_CHANGED` after partial progress. |
| [42675847104](https://github.com/idevtm/os1/actions/runs/42675847104), job 129356862612 | `c8dc8dc` | Rescue export did not produce an accessible `/tmp/log`. |

The first failure reported this output:

```text
cp -r "/tmp/src" "/disk/tree"
tree: stopped; visited=3 completed_actions=4 bytes=8; earlier effects remain
shell: cp failed status=-24
```

The second failure reported this output after `log_export`:

```text
ls /tmp/log
ls: cannot open directory
rescue: command failed
rescue>
```

The second CI log does not contain the export's numeric status.
The original logs remain under `build/pr45-failures/original-ci-{0,1}.log`.
Later passing runs do not replace these failures.

| Local log | Bytes | SHA-256 |
| --- | --- | --- |
| `original-ci-0.log` | 366927 | `64694dca7c0ab18a8542087afeac2685d0ae995330e149fdada3864452e0da11` |
| `original-ci-1.log` | 367292 | `01c3e4b31b18390aaae8939d50c678d301dc2f0b0709d31e115e0539d6993e62` |

The full logs remain local because they contain unrelated build output.
The linked CI runs, exact failure text, and hashes preserve the evidence identity.

## Requirements and impact

The following current requirements govern this correction:

- `GOALS.md`: **Simplicity first**, **Storage and filesystems**, and **Testability and observability**.
- `docs/ARCHITECTURE.md`: **Filesystem metadata and user traversal** and the log export architecture.
- `docs/native-api/abi/filesystem-metadata-abi.md`: **Guarded tree steps** and **Concurrency and failure**.
- `docs/native-api/abi/log-export-abi.md`: **Calls and authority** and **Storage and scheduling**.
- `AGENTS.md`: **Mandatory local test policy** and **Mandatory independent review**.

These ABI profiles describe implemented behavior. They are not future object-model proposals.

| Area | Assessment |
| --- | --- |
| Scope | Correct two timing-dependent failures without automatic replay of partially completed file mutations. |
| Boot and platform | Cover UEFI and BIOS. BootInfo, firmware layouts, and hardware drivers do not change. |
| Memory and isolation | Preserve descriptor authority, bounded queues, and cleanup. Measure added node and Operation state. |
| Concurrency | Refine namespace conflict scope. Retain export requests during admission conflicts without blocking other providers. |
| Interfaces | Keep legacy global tree checks. Add a versioned directory-scoped tree profile without changing record size. |
| Resources | Measure affected smoke durations and artifacts against `62a6952`. Check for retry loops and unbounded allocations. |
| Validation and documents | Preserve CI output, force both interleavings, run full local validation, and obtain independent review. |

## Plan

1. Preserve the original logs and reproduce both mechanisms with deterministic host tests.
2. Add directory-scoped tree checks while retaining version 1 behavior.
3. Track source and destination generations separately in each walker frame.
4. Keep stale-name and object-identity checks for mutations inside the traversed directories.
5. Keep explicit Log export Operations pending until admission succeeds.
6. Distinguish admission deferral from storage errors after export starts.
7. Add bounded dispatch backoff and permit other queued requests to proceed.
8. Report numeric rescue command errors.
9. Run targeted regressions and the complete local validation command.
10. Obtain independent review before commit and push.

No test may hide a failure through a blind retry.
Physical hardware validation remains separate from host and QEMU evidence.

## Baseline

The source baseline is `62a69523c923aa0e4e429950a059773563b866e5`.
Five unchanged QEMU runs passed for each affected UEFI check.
Log-export durations were 18.99, 18.97, 18.97, 18.96, and 18.90 seconds.
Tree-tool durations were 5.78, 5.76, 5.75, 5.76, and 5.73 seconds.
The combined command took 123.59 seconds.
The transcript is `build/pr45-failures/baseline-qemu.log`.
These passes did not reproduce or resolve the original failures.

## Deterministic reproduction

`build/pr45-failures/reproduction-2.log` records both failures on the unchanged implementation.
The first regression forced export between two tree steps and received `-24`.
The second held foreground admission while dispatching a retained export request.
That request completed with `WOULD_BLOCK` (`-9`), and `tmp_bytes` remained zero.
The initial fixture attempt failed before either interleaving; `reproduction.log` preserves that setup error.

The host reproduction establishes both mechanisms. The old rescue CI log does not identify its exact admission contender.

## Implemented correction

Tree-step version 2 checks each input directory's token.
Version 1 retains the global contract and unchanged 384-byte layout.
The walker keeps separate source and destination tokens in each of its sixteen frames.
It does not retry `OS1_ERR_CHANGED` or replay mutations.

The Log provider distinguishes admission deferral from an admitted result.
A deferred Operation retains its existing reference and returns to the worker queue.
Its next eligible dispatch time is one millisecond later.
The worker skips deferred requests and sleeps when no request is eligible.
After admission, export executes once, including when storage returns `WOULD_BLOCK`.
Rescue prints numeric command status to make future failures observable.

## Validation and measurements

The host is a Mac M4 Max with 16 physical cores and 64 GiB RAM.
The host runs macOS 26.6.2, build `25G83`.
QEMU is version 11.1.3. Guest tests use TCG, q35, four CPUs, and 128 MiB RAM.
UEFI uses Homebrew `edk2-x86_64-code.fd` and a private copy of `edk2-i386-vars.fd`.
BIOS checks use the normal raw image and QEMU firmware.
The guest compiler is x86_64-elf GCC 11.2.0 with the Debug preset and `-Os` kernel options.
Host tests use Apple Clang 21.0.0.
Normal images enable allocator checks and debugcon; boot and USB keyboard diagnostics remain disabled.
The full validation command builds the separate USB diagnostic profile afterward.

### Structure sizes

Compiler-emitted constants measure the x86-64 layouts before and after the correction.
These measurements compare baseline headers and source from `62a6952` with the working tree.

| Type | Baseline bytes | Corrected bytes | Difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| VFS node | 416 | 424 | +8 | +1.92% |
| Retained Operation | 1168 | 1176 | +8 | +0.68% |
| Walker frame | 288 | 304 | +16 | +5.56% |
| Complete walker | 8784 | 9032 | +248 | +2.82% |

The walker retains its sixteen-frame limit and uses no new dynamic allocation.
A deferred Operation retains existing ownership and adds no queue allocation.
Assembly evidence is under `build/pr45-failures/*structure-sizes.s` and `*operation-size.s`.

### Checks

The validation commands are:

```sh
build-host-tests/os1_host_tests --gtest_filter='LogExport.*:TmpfsContract.*:KernelConsole.*'
ctest --test-dir build -R '^os1_smoke_(tree_tools|log_export)_(uefi|bios)$' --parallel 2 --output-on-failure
./scripts/check-local.sh
ctest --test-dir build -R '^os1_smoke_(tree_tools|log_export)_uefi$' --repeat until-fail:5 --output-on-failure
```

The repeated runs measure stability and timing after the deterministic corrections.
They do not replace the recorded failing runs.

The two deterministic regression failures pass after the correction.
The expanded host filter passes all 87 selected cases.
The affected UEFI and BIOS tree/export checks pass, four checks total.

The first full run passed all 598 host tests.
It exposed expected-error assertions in both bootstrap checks.
Those tests deliberately read missing files; the new numeric rescue error triggered the generic failure guard.
The complete first-run output remains in `build/pr45-failures/full-local-1.log`.
The correction accepts those two deliberate failures and asserts their exact numeric status.
All other command checks remain active.
The first full run finished with 78 of 80 normal checks passing and all three diagnostic checks passing.
Its phase durations were 20.16, 300.85, and 5.75 seconds.
The first-run phase logs also remain under `build/pr45-failures/full-1/`.

Two independent review passes found no concrete defects.
The second pass reviewed the expected-error assertion correction and its interaction with generic error detection.
The reviewer did not edit files or run tests.
The complete corrected local suite passed with exit status zero:

| Profile | Result | Duration |
| --- | --- | ---: |
| Host unit tests | 598/598 | 19.86 seconds |
| Normal images and contracts | 80/80 | 313.33 seconds |
| USB diagnostic images | 3/3 | 5.74 seconds |

The full command transcript is `build/pr45-failures/full-local-2.log`.
Phase logs are `build/local-validation/{host,full,diagnostics}.log`.
The final source hashes are recorded in `build/pr45-failures/candidate-artifacts.json`.
The complete normal suite includes both firmware paths, SMP checks, storage transports, lifecycle, and preserving deployment.
Formatting, whitespace, and changed-document local-link checks pass.
Secret review found no credentials; token references describe namespace generations.
Comparative timing results follow below.
Physical-device behavior and complete ASD-STE100 dictionary compliance remain unverified.

### Artifact sizes

| Artifact | Baseline bytes | Corrected bytes | Difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| `kernel.elf` | 576904 | 578072 | +1168 | +0.20% |
| `initrd.cpio` | 7680 | 7680 | 0 | 0% |
| `os1.iso` | 4311040 | 4313088 | +2048 | +0.05% |
| `os1.raw` | 1474560 | 1474560 | 0 | 0% |

The baseline artifacts use the same source tree as `62a6952`, before these corrections.
Their embedded source revision can precede the squash merge.
The baseline and candidate artifact records include SHA-256 hashes under `build/pr45-failures/`.
Both firmware images remain within the existing size limits.

### Repeated timing and allocation comparison

Five isolated runs per check passed before and after correction.
The [measurement data](2026-11-09-export-traversal-measurements.json) contains every sample and its evidence directory.
These are sequential host batches, not alternating runs. No performance improvement is claimed.

| Measurement | Baseline median | Corrected median | Difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| Tree check execution | 5.727 s | 5.714 s | -0.012 s | -0.22% |
| Tree check child CPU | 6.116 s | 6.123 s | +0.007 s | +0.11% |
| Export check execution | 18.927 s | 20.099 s | +1.172 s | +6.19% |
| Export check child CPU | 10.674 s | 10.682 s | +0.008 s | +0.07% |
| Rescue export command | 7.316 ms | 7.273 ms | -0.043 ms | -0.59% |

Execution times exclude the isolation wrapper's input-copy preparation.
Child CPU includes QEMU and the host test helpers.
The missing-disk phase used five status queries instead of four in the corrected median run.
That phase increased from 10.480 to 11.550 seconds (+1.071 seconds, +10.22%).
The extra one-second polling interval accounts for most of the whole-check increase.
The failure-suspension threshold and polling assertions remain unchanged.

The tree workload ended with 180 slab pages and 2366 live objects in all ten samples.
Both values have zero absolute and relative difference.
Allocation failures remained zero; their relative difference is undefined because the baseline is zero.
The twenty repeated checks passed. Their transcripts are `baseline-qemu.log` and `candidate-qemu.log` under `build/pr45-failures/`.
