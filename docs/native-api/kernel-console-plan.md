# Kernel console provider plan — 2026-11-05

Implement the first kernel-hosted provider through the existing native IPC transport.
The initial protocol provides copied `write`, `try_read`, and information-property requests.
The plan was recorded before implementation; the validation record below tracks completion.

```text
Granted console descriptor -> bind kernel endpoint -> submit copied request
  -> retained Operation -> kernel worker -> console callbacks -> retained result
```

## Scope and contract

Public methods, properties, and events use `snake_case`; types and interfaces may use `UpperCamelCase`.
The console members are `write`, `try_read`, and the `info` property.

- Add IPC operation 28 to bind a known kernel interface from an existing resource descriptor.
- Select Console v1 from a fixed internal provider table; reject unknown interfaces and unsupported resource kinds.
- Capture a retained open instance and an explicit subset of its READ, WRITE, and INSPECT authority.
- Return one ready CLOEXEC client endpoint; expose no provider descriptor or ambient console lookup.
- Preserve DELEGATE only when the source grant permits delegation. Endpoint WRITE authorizes submission; captured service rights authorize methods.
- Bound kernel bindings globally and retain their charges through Operations and aliases.
- Bound accepted Operations per binding, including retained results; serialize dispatch through one kernel worker with a FIFO queue.
- Copy requests before publication. Invoke provider code outside IPC, descriptor, and scheduler locks and outside IRQ context.
- Use existing SUBMIT, RESULT, WAIT, WAIT_MANY, and synchronous wrappers; completion can race the caller's first wait.
- Keep accepted work alive after endpoint/Operation close. Closing or timing out does not cancel a request.
- Support `write` up to 256 bytes, `try_read` with an explicit bounded capacity, and a read-only information property.
- `try_read` returns WOULD_BLOCK when no complete line is available. It consumes input into the retained result exactly once.
- Result-copy failure retains the result for retry; dropping that result does not put input back into the console queue.
- Reject capability/Buffer envelopes and subscriptions on this provider before acquiring attachments or changing Buffer ownership.
- Preserve existing descriptor console I/O and shared input ordering. No mutable filesystem, service directory, blocking input, input events, or kernel Buffer borrower is added.

The existing console descriptor is the bootstrap grant for this first slice.
General service-directory discovery remains the next integration decision, not a prerequisite for this adapter.
Kernel provider identity is independent of a user process; no user provider process is impersonated.

## Impact assessment

| Area | Requirements and effects |
| --- | --- |
| Scope | GOALS Native system interface, Simplicity first, and Runnable vertical slices; implement the roadmap's first console adapter. |
| Boot/platform | Initialize one kernel worker before user publication; fail startup explicitly if allocation fails. Validate UEFI and BIOS. |
| Memory/isolation | Retain source authority and accepted Operations; enforce binding/operation bounds and transactional creation; reject attachment modes. |
| Concurrency | FIFO worker dispatch, SMP completion, close races, lost-wakeup prevention, no handlers under global locks or IRQ context. |
| Interfaces | Add a versioned binding record and Console v1 protocol; preserve existing IPC records, syscalls, and descriptor compatibility. |
| Resources | Measure kernel/initrd growth and comparable repeated UEFI/BIOS workloads; record worker and binding memory costs. |
| Validation | Host syscall/contract tests, user sync/async two-process example, UEFI/BIOS/SMP smokes, and independent adversarial review. |

## Implementation sequence

1. Capture baseline artifacts and repeat the existing workload on both boot paths.
2. Implement the bounded provider binding, worker queue, and console protocol.
3. Add host tests for rights, malformed records, limits, retained completion, input delivery, allocation rollback, and cleanup.
4. Extend an existing initrd regression program to exercise two console clients without duplicating a program image.
5. Update the ABI, architecture, roadmap, and indexes with actual behavior and deferred work.
6. Run host and QEMU checks; compare resource measurements and obtain independent review.

## Validation record

Baseline revision: `ecd337cb4affd3a0ef4152e6081f46bc01f699ab`.
Logs and machine/toolchain details are collected under `build/kernel-console-evidence/`.
Physical hardware is not available to the agent; emulator results do not establish physical qualification.
Full ASD-STE100 dictionary compliance remains unverified because the complete standard and dictionary are unavailable.

### Completed implementation

[Console v1](abi/kernel-console-abi.md) now uses IPC operation 28 and the existing retained Operation transport.
The implementation includes the worker, binding limits, method authority, C++ helpers, and a two-process `ipccheck` example.
Metadata validation enforces `snake_case` member names while preserving type and interface naming flexibility.
The architecture, ABI index, copied-metadata profile, and roadmap describe the implemented boundary.

### Checks and review

| Check | Result |
| --- | --- |
| `cmake --build build` | Passed; UEFI, BIOS, and initrd artifacts fit their current layouts. |
| `cmake --build build-host-tests` and `ctest --test-dir build-host-tests --output-on-failure` | 333 tests passed; baseline had 317. |
| Console host coverage | 15 tests cover binding, authority, copy faults, retained input/results, FIFO order, limits, aliases, allocation failures, and wait lifecycle. |
| Metadata coverage | One additional test checks methods, both property operations, and events against the naming rule. |
| UEFI/BIOS spawn | Both passed with the new two-process console marker required. |
| Complete integration matrix | All 16 tests passed after the register correction: UEFI, BIOS, SMP, xHCI, no-UART, and script contracts. |
| Source formatting | Explicit owned C++ paths formatted; `kernel_main.cpp` used changed ranges because existing formatting differs. |
| Document links and whitespace | Local targets checked; `git diff --check` passed. |

The first complete matrix encountered two sandbox restrictions on local Unix sockets.
The monitor contract and no-UART smoke both passed with local socket access.
These failures were environment restrictions, not guest failures.
The final matrix uses that same local socket access.
No USB device was written.

An independent reviewer inspected the complete diff, new files, contracts, and validation evidence without editing or executing code.
The reviewer identified two defects:

- P1: IPC wakeup replaced a kernel worker's saved `RAX` with a syscall result.
- P2: The ABI incorrectly described input and output callback capacities as belonging to one console resource.

A regression test reproduced the register overwrite before correction.
Wakeup now writes the return register only for user threads.
The kernel-worker test verifies preservation of an arbitrary saved register value.
The ABI now distinguishes input and output resources; tests check both information records.
The second independent review passed.
It verified both corrections, their interactions, the naming rule, and the completed validation evidence.
No substantive findings remain.

### Comparable runtime measurements

The baseline is `ecd337cb4affd3a0ef4152e6081f46bc01f699ab`; the comparison uses this working-tree implementation.
Both used macOS 26.6.2 on arm64, GCC 11.2.0, QEMU 11.1.3, and the existing Debug build.
Kernel C++ compilation uses `-Os`; ACPICA C compilation uses `-O1`.
Kmem debugging and QEMU debug-console output were enabled.
The emulator used q35, four CPUs, default software emulation, and 128 MiB RAM.
A stopped QEMU instance confirmed the default RAM size; no memory override was supplied.

UEFI used `/opt/homebrew/share/qemu/edk2-x86_64-code.fd` and the existing i386-named variables template.
The code firmware SHA-256 was `33090cc07675baa5190d9f1e84bf5176b33bcbfa9bacac522961150cdb6dbb2a`.
The variables-template SHA-256 was `5d2ac383371b408398accee7ec27c8c09ea5b74a0de0ceea6513388b15be5d1e`.
BIOS used the same QEMU installation and its default BIOS.

Each comparison booted the existing spawn workload three times per boot path.
Both revisions used the frozen baseline initrd, including identical user programs and shell input.
Its SHA-256 was `961e945a5ddf7542ad007feb8c37ec9163e003afae4813bf54dcac37e1aafacf`.
Only these frozen-workload runs omitted the newly introduced console marker, which the baseline program cannot emit.
The current initrd and complete marker set were restored before final integration tests.

| Workload | Baseline median | Current median | Difference | Relative difference |
| --- | --- | --- | --- | --- |
| UEFI spawn | 9.914972 s | 9.942460 s | +0.027489 s | +0.277% |
| BIOS spawn | 8.119501 s | 8.102499 s | −0.017002 s | −0.209% |

Current UEFI repeats were 9.892891, 9.954677, and 9.942460 seconds.
Current BIOS repeats were 8.173724, 8.068203, and 8.102499 seconds.
All six comparison runs passed.
These are host elapsed times for complete smoke workloads, including startup and runner delays.
They do not measure individual Console call latency or establish a statistically significant performance change.
The requirement is successful bounded operation; no latency target was assigned for this slice.
CPU utilization and physical-hardware latency remain unmeasured; a dedicated service benchmark is the next performance validation step.

### Image and memory costs

| Artifact | Baseline bytes | Current bytes | Difference | Relative difference |
| --- | --- | --- | --- | --- |
| UEFI kernel | 455,928 | 460,832 | +4,904 | +1.076% |
| BIOS kernel | 340,480 | 344,576 | +4,096 | +1.203% |
| Initrd | 197,120 | 199,168 | +2,048 | +1.039% |
| ISO | 4,380,672 | 4,388,864 | +8,192 | +0.187% |
| BIOS raw image | 1,474,560 | 1,474,560 | 0 | 0% |

Boot frontend, preloader, and kernel16 artifact sizes did not change.
The 196 KiB BIOS initrd slot has 1,536 bytes remaining; additional programs will require renewed layout checks.

Cross-compiled debug type records measure the shared structure changes:

| Structure | Baseline bytes | Current bytes | Difference | Relative difference |
| --- | --- | --- | --- | --- |
| Connection | 208 | 224 | +16 | +7.692% |
| Operation | 888 | 896 | +8 | +0.901% |
| Endpoint | 64 | 64 | 0 | 0% |
| OpenInstance | 32 | 32 | 0 | 0% |
| Thread | 400 | 400 | 0 | 0% |

Connection and Operation still use their existing 256-byte and 1,024-byte allocation size classes.
These structure additions also affect user-provider connections; their allocation size classes do not grow.
Each new binding allocates one Connection, one Endpoint, and one OpenInstance.
Each accepted request allocates one Operation and one OpenInstance.
The queue links are intrusive and require no separate queue allocation.

The worker adds four 4-KiB stack pages and one 400-byte Thread record.
The raw additional storage is 16,784 bytes before allocator overhead; the relative difference from no worker is undefined.
At maximum admission, binding and request payload size classes total 73,216 bytes before debug redzones and slab overhead.
This is a calculated bound, not a measured resident-memory high-water mark.
Existing source instances are retained rather than copied.
The costs buy bounded asynchronous dispatch and resource lifetime independent of caller descriptors.

### Evidence files

Local logs remain under `build/kernel-console-evidence/`:

- `baseline.json`, `baseline-runs.json`, and `final-artifacts.json` record artifact sizes and comparison inputs.
- `final-frozen-runs.json` and `final-frozen-*.log` record repeated frozen-workload timings.
- `final-host-tests.log` and `final-full-smokes.log` record the final checks.
- `register-regression-before.log` records the reproduced register-preservation failure.
- `monitor-contract.log` and `no-uart.log` record the socket-enabled reruns.
- `size-*.log` and `baseline-size-*.log` record cross-compiled structure sizes.

Build outputs and local logs are not committed documentation artifacts.
The numerical tables above preserve the result without requiring those local files.

## Documentation alignment and publication — 2026-11-05

Record the approved handle model before publishing this slice to PR 24.
This follow-up applies GOALS sections “Native system interface”, “Simplicity first”, and “Runnable vertical slices”.
It aligns GOALS, README, architecture, glossary, the Native API contract, the C++ proposal, and implemented ABI status links.
The shared table, integer ABI, object ownership, and service policies remain unchanged.
Boot, memory, concurrency, and runtime costs therefore retain the implementation evidence above.
No new benchmark is required for this documentation alignment.

Use “object handle” for the native concept and retain `fd` in existing signatures and compatibility descriptions.
Distinguish transport rights from captured service authority and object identity from process-local handle values.
Typed owning and borrowed C++ references remain proposed SDK work.
Correct obsolete status statements without rewriting historical measurements or archived plans.
Validate local links, source claims, executable examples, and the final PR description.
Run final host and integration checks for the code publication, then obtain independent review before commit.
Push only the reviewed commit; verify its CI and leave the PR unmerged.

Final publication checks passed: 333 host tests and 16 integration checks.
Logs are `publication-host-tests.log`, `publication-smokes.log`, and `publication-doc-links.json` in the evidence directory.
The publication review found stale GOALS statements that still classified Console dispatch as unimplemented.
Those statements now mark the Console slice complete and distinguish additional service contracts.
The console-descriptor profile also distinguishes Buffer unmap from unsupported general address-space unmap.
The correction review passed with no remaining substantive findings.
It covered the final task changes and their integration with existing IPC and resource contracts, not every historical PR change.
All 41 new local links and anchors resolve; whitespace checks pass.
The secret scan found only recorded commit and artifact hashes; no credentials were found.
