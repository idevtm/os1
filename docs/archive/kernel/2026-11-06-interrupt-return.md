# Interrupt-return investigation

Investigate the intermittent BIOS general-protection fault before merging USB storage PR 31.
The retained failure occurs at `restore_frame_ptr.resume_kernel` during `iretq`, after the first user process starts.
Controlled timing reproduces the fault; interrupt masking corrects the exposed handoff window.

```text
Kernel boot thread -> select next thread -> change stack and current thread -> restore registers -> iretq
                                               ^ interrupts must remain masked ^
```

## Plan and impact

1. Preserve the failing log and baseline artifacts from `9344844`.
2. Repeat the original BIOS storage test, then isolate the return path with controlled interrupt timing.
3. Correct only reproduced contract violations and add regression coverage.
4. Run host, BIOS, UEFI, and SMP checks; compare resources and obtain independent review.
5. Publish the correction, verify CI, merge PR 31, and remove local branches whose work is merged.

| Area | Assessment |
| --- | --- |
| Scope | GOALS kernel reliability and runnable storage; investigate before merge. |
| Boot/platform | Architecture interrupt entry/return and initial scheduler handoff affect both firmware paths. |
| Memory/isolation | Preserve the interrupted stack and frame ownership; do not reuse transient stack contents after a switch. |
| Concurrency | Mask local interrupts during scheduler selection and restoration; retain SMP lifetime rules. |
| Interfaces | TrapFrame is an internal assembly/C++ contract. User ABI and BootInfo layouts must remain unchanged. |
| Resources | Compare linked sections and repeat-test timings against the same build configuration. |
| Validation | Deterministic return-path regression, original BIOS repetitions, host suite, full integration matrix, and independent review. |

Affected requirements:

- [GOALS: Process and scheduling model](../../../GOALS.md#process-and-scheduling-model) requires safe thread switching.
- [GOALS: Boot strategy](../../../GOALS.md#boot-strategy) retains BIOS and UEFI paths.
- [GOALS: Testability and observability](../../../GOALS.md#testability-and-observability) requires reproducible checks.
- [Architecture: Unified TrapFrame](../../ARCHITECTURE.md#unified-trapframe) defines interrupt restoration.
- [Architecture: Scheduler Model](../../ARCHITECTURE.md#scheduler-model) defines thread selection and lifetime.

## Baseline and hypotheses

The original log remains in `build/usb-diagnostics-evidence/all-checks.log`.
It reports vector 13, error `0x6fa8`, and an instruction address resolved against that build to the final kernel `iretq`.
The register dump reports IF enabled in saved RFLAGS (`0x10292`).
The latest baseline passes 100 consecutive BIOS storage checks in 162.68 seconds.
That workload performs two independent guest boots per repetition.
Evidence is retained in `build/iretq-evidence/baseline-repeat.log`.

Hypothesis 1: direct kernel scheduler handoff permits a timer interrupt while restoration state is only partially installed.
`finish_kernel_thread` does not mask interrupts before selecting its successor; the assembly entry also has no masking instruction.
A test-only delay in the exposed interval will check this hypothesis before a production correction.

The interrupt-frame audit also found a separate representation concern.
Kernel trap entry stores the address of the hardware RSP slot and a zero SS sentinel, rather than copying both saved values.
Intel documents unconditional SS:RSP pushes and pops in 64-bit interrupt return.
This concern still requires an executable regression and must not be assumed to explain the retained fault.

Reference: [Intel system programming manual, 64-bit interrupt stack frame](https://cdrdv2-public.intel.com/835754/253668-sdm-vol-3a.pdf).

## Reproduction and correction

A temporary delay ran only when IF was already enabled at `restore_frame_ptr`.
It reproduced vector 13 with error `0x6fa8` at the kernel `iretq`, matching the retained failure signature.
A bounded QEMU interrupt trace shows timer vector `0x20` entering the restoration sequence before that fault.
The failed return uses a corrupted stack selector while the current stack is the per-CPU restore scratch area.
Logs and the instrumented source are retained under `build/iretq-evidence/`.

The root cause is interruptible direct scheduler handoff.
A timer can save a partially restored context into the destination frame while restoration still consumes it.
The correction masks interrupts before boot-thread termination selects a successor and before initial current-thread publication.
Both assembly handoff entries also execute `cli` before changing stacks or restoration state.
The destination frame still controls IF when `iretq` completes; this does not leave ordinary threads permanently masked.
The same temporary timing amplification then passes five BIOS storage repetitions in 8.06 seconds.

The same-CPL saved-frame encoding was not changed.
Its zero SS sentinel retains the hardware tail on the interrupted stack; it is a separate representation from a synthetic frame.
No additional change to that encoding was required to eliminate the reproduced fault.
This investigation does not claim to explain every historical BIOS fault with a different signature.

## Permanent regression

`os1_interrupt_handoff_contract` compiles the real assembly with test-only instrumentation into a private temporary image.
It reuses the current kernel objects and linker configuration without editing source or normal image artifacts.
When IF is enabled at the restoration boundary, the instrumentation widens the timer window and then executes `ud2` if no timer intervenes.
Thus absence of a conveniently timed interrupt cannot produce a false pass.
The positive case must reach the shell and execute a command without a fault.
A negative control explicitly reopens IF at that boundary and must report `#GP` or `#UD`.
Timeout, missing output, or another failure does not count as successful negative-control detection.

The new regression initially passes both cases in 1.53 seconds; the final matrix repeats them in 1.58 seconds.
Before the packaging correction, the corrected production image passes 100 BIOS storage repetitions in 162.50 seconds.
The 0.18-second decrease (0.11%) is workload variation, not evidence of lower interrupt latency.
All 417 host tests pass in 6.99 seconds.
All 43 integration checks pass in 300.59 seconds before the packaging rebuild.
All 43 checks pass again with final packaging in 300.17 seconds.
Fresh configuration with `BUILD_TESTING=OFF` also succeeds through the default cross-toolchain preset.

Independent read-only review found no blocking code defects.
A follow-up review accepted the packaging change after checking both loaders and test-image construction.
It covered handoff ordering, compiler barriers, frame restoration, scheduler lifetime, test construction, and retained fault evidence.
The regression checks IF at assembly restoration; it does not independently detect removal of either C++ guard.
Physical validation of this correction remains pending.
Full ASD-STE100 dictionary compliance was not verified.

Reproduce the final checks:

```sh
cmake --preset default -DOS1_KMEM_DEBUG=ON -DOS1_QEMU_DEBUGCON=ON
cmake --build build --target os1_image os1_bios_image os1_usb_image
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure --no-tests=error
ctest --test-dir build --output-on-failure --no-tests=error
ctest --test-dir build --output-on-failure -R '^os1_smoke_usb_storage_bios$' --repeat until-fail:100
ctest --test-dir build --output-on-failure -R '^os1_interrupt_handoff_contract$'
```

Final logs are `host-tests.log`, `all-checks.log`, `corrected-repeat.log`, and `handoff-contract.log` under `build/iretq-evidence/`.
Correction `030cb06` follows baseline `9344844`; subsequent CI workflow changes do not alter the kernel.
The temporary timing experiment is absent from production assembly.

## Resource comparison

Both builds use GCC 11.2.0, the existing size optimization, and the same macOS ARM64 host configuration.
CMake uses the Debug preset with `OS1_KMEM_DEBUG=ON` and `OS1_QEMU_DEBUGCON=ON`.
Kernel compile settings retain size optimization. QEMU version is 11.1.3.
QEMU uses q35, four CPUs, and 128 MiB RAM. BIOS measurements use the same PIT fallback path.
UEFI checks use Homebrew QEMU's `edk2-x86_64-code.fd` and `edk2-i386-vars.fd` firmware files.
One linked-section measurement per deterministic build compares `9344844` with the correction.

| Section | Baseline bytes | Candidate bytes | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| Text | 335872 | 335872 | 0 | 0% |
| Data | 32768 | 32768 | 0 | 0% |
| BSS | 176128 | 176128 | 0 | 0% |
| Total | 544768 | 544768 | 0 | 0% |

Existing section padding absorbs the added instructions. No new heap or DMA allocation is introduced.
Masking extends only through one-way scheduler handoff, including cleanup and selection in boot-thread completion.
Fine-grained interrupt latency and physical-machine timing remain unmeasured.
Repeated boot durations measure regression workload completion, not a controlled throughput benchmark.

## CI image-size correction

CI run 176 failed before smoke tests on baseline `9344844`.
Its compiler produced a 377344-byte BIOS kernel, exceeding the 373248-byte staging slot by 4096 bytes.
The local compiler produced 373248 bytes. This failure is separate from interrupt restoration.

The core ELF now uses 256-byte file alignment; linker-script addresses retain 4096-byte memory alignment.
The BIOS packaging step removes unused section metadata through the existing ELF compactor.
The normal kernel ELF retains its symbols for diagnosis. Neither change alters the reserved memory limits or initrd capacity.

A controlled local relink used the same object order and compared every PT_LOAD payload, address, permission, and size.
All values matched. The BIOS file decreased from 373248 to 368896 bytes: 4352 bytes, or 1.17%.
Loaded sections and allocation requirements remain unchanged. Both firmware loaders copy segments from their explicit ELF offsets.
CI run 177 passed the unchanged BIOS size guard with the compact image.

## CI fixture dependency

CI run 177 passed the image build, host suite, and interrupt-handoff regression.
Four combined USB-data checks failed because `os1-usb.img` did not exist.
The workflow built only ISO and BIOS images; local validation explicitly built the USB image too.
The workflow now builds `os1_usb_image` before CTest.
This adds the required host image-generation step without changing guest code, memory requirements, or firmware contracts.
The image-size failure in run 176 previously prevented these tests from running.
