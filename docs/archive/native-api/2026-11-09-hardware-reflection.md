# Hardware reflection implementation record

This task exposes cached hardware records through live context discovery and Tab navigation.
The accepted [hardware architecture](../../native-api/hardware-objects.md) separates descriptive records from functional device objects.

```text
boot discovery -> cached records -> bounded observation queries
observation queries -> runtime discovery -> context, Tab, and typed fields
```

## Requirements and impact

| Area | Assessment |
| --- | --- |
| Project scope | Follow GOALS: Platform discovery, Native system interface, Driver model, and Runnable vertical slices. Keep reflection passive. |
| Boot and platform | Cover UEFI and BIOS. Preserve BootInfo and mappings. Cache PCI bridge facts during existing enumeration. Retain USB outcomes. |
| Memory and isolation | Use bounded records and pages. Validate query versions, identifiers, cursors, lengths, and user buffers. Expose no kernel pointers. |
| Concurrency | Read boot-published topology. Do not acquire device-control authority or execute hardware transactions. Define cached-state limits. |
| Interfaces | Extend observation without changing existing record layouts. Extend runtime discovery with external bounded descriptions. |
| Runtime resources | Measure image sizes, discovery memory, and comparable shell workload timing. Avoid copying the complete tree into the fixed View. |
| Validation and documents | Add host boundary tests and guest discovery/navigation checks. Run UEFI and BIOS checks with SMP. Obtain independent review. |

Relevant contracts are Architecture: Native objects and OS services, Modern Platform Support, and Live context exploration.
The Native API index distinguishes implemented profiles from accepted designs.
The current runtime discovery profile defines generations, bounded pages, aliases, and passive enumeration.
Hardware observation retains the existing global observation disclosure policy; scoped inspection grants remain future work.

## Plan

1. Define a versioned, bounded hardware observation query and typed descriptions.
2. Describe existing cached records and physical relationships without hardware access.
3. Add external discovery nodes without enlarging the fixed 256-node registry.
4. Connect field values, aliases, context inspection, and Tab navigation.
5. Add boundary tests, firmware smokes, and comparative measurements.
6. Review the complete diff independently before publication.

## Validation evidence

The normal and diagnostic builds pass their image-layout checks.
The corrected host suite passes all 590 tests.
BIOS and UEFI context tests pass, including typed hardware fields, bindings, and Tab selection.
The complete 77-test guest matrix passes on the final implementation.
All three post-correction diagnostic keyboard checks pass.
The matched baseline comparison also completes without allocator failures.

Commands and logs:

```sh
cmake --build build -j 6
cmake --build build-host-tests -j 6
ctest --test-dir build-host-tests --output-on-failure
ctest --test-dir build --output-on-failure -j 2
cmake --build build/hardware-reflection-diagnostic --target os1_usb_image -j 6
ctest --test-dir build/hardware-reflection-diagnostic --output-on-failure --no-tests=error -R '^os1_smoke_usb_keyboard_'
```

Local logs are under `build/hardware-reflection-evidence/`.
The first sandboxed matrix could not bind QEMU monitor sockets; it was stopped and restarted with local socket access.
An initial test incorrectly passed an Integer to `echo`, whose accepted parameter is String.
The corrected test checks direct Integer evaluation, bindings, and String printing without changing the language contract.

## Comparative measurements

Baseline revision: `453aa978a1fa570cc391b31c4e9554b62ab75c74`.
A separate detached checkout and build produced the baseline artifacts.
Candidate source: `44f2c0688eb08a3e2cc66c23ad3bff3d28edce23`.
The tested artifacts were built before that commit; their embedded revision is `f34648e` with the compact snapshot correction.
Artifact hashes are recorded in `build/hardware-reflection-evidence/artifacts-final.json`.
Both builds use GCC 11.2.0, Debug, `OS1_KMEM_DEBUG=ON`, and `OS1_QEMU_DEBUGCON=ON`.
The host runs macOS 26.6.2 on an Apple M4 Max with sixteen physical cores and 64 GiB RAM.
GNU ld 2.37 links both builds.
QEMU 11.1.3 uses q35, TCG, four CPUs, and 128 MiB RAM.
UEFI uses the installed `edk2-x86_64-code.fd` and a fresh copy of `edk2-i386-vars.fd`.
BIOS uses the QEMU default firmware. Each run receives a fresh program-test disk copy.

The workload executes twenty identical `context process.environment.path` commands, with allocator observations before and after.
Each firmware and revision has three runs. Values below are medians.

| Property | Baseline | Candidate | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| BIOS workload, seconds | 2.036182 | 2.067531 | +0.031349 | +1.54% |
| UEFI workload, seconds | 0.931720 | 0.941853 | +0.010133 | +1.09% |
| BIOS boot, seconds | 0.472506 | 0.481552 | +0.009046 | +1.91% |
| UEFI boot, seconds | 12.169731 | 12.150793 | -0.018938 | -0.16% |
| BIOS QEMU CPU, seconds | 2.722673 | 2.804800 | +0.082127 | +3.02% |
| UEFI QEMU CPU, seconds | 3.647250 | 3.675442 | +0.028192 | +0.77% |
| Kernel `.bss`, bytes | 196608 | 196608 | 0 | 0.00% |
| Kernel load memory, bytes | 626688 | 630784 | +4096 | +0.65% |
| BIOS kernel file, bytes | 429192 | 428936 | -256 | -0.06% |
| UEFI ISO, bytes | 4446208 | 4450304 | +4096 | +0.09% |
| BIOS raw image, bytes | 1474560 | 1474560 | 0 | 0.00% |
| Shell ELF, bytes | 162344 | 165536 | +3192 | +1.97% |
| Initrd, bytes | 144896 | 147456 | +2560 | +1.77% |
| Runtime View, bytes | 52392 | 54456 | +2064 | +3.94% |
| Completion state, bytes | 3640 | 10808 | +7168 | +196.92% |
| Hardware page cache, bytes | 0 | 1512 | +1512 | Undefined |

The existing command workload shows small timing variation; these runs do not establish a speed improvement.
No run reports an allocator failure.
All runs report identical allocator observations: 168 to 169 slab pages and 2335 to 2337 live objects.
Asynchronous cleanup and log export can affect these observations; this workload does not establish a leak bound.
Hardware metadata queries allocate no kernel memory and issue no storage I/O.
Large physical inventories and hardware-query latency remain unmeasured on physical devices.
The corrected local BIOS kernel has 5240 bytes of file-slot headroom. The initrd has no remaining file-slot headroom.
Both remain within unchanged layout gates. Future shell growth needs an explicit image-layout or size-reduction decision.

Reproduce timing measurements with `tests/scripts/measure_hardware_reflection.py`.
Use `--baseline build/hardware-reflection-baseline/artifacts`, `--candidate build/artifacts`, and an isolated `--output` directory.
Supply the installed QEMU, OVMF code, and OVMF variables paths through `--qemu`, `--code`, and `--vars`.
The script preserves per-run logs and `measurements.json`.
Final measurements are under `build/hardware-reflection-evidence/measure-final/`.
The full guest log is `final-matrix-compact.log`; diagnostic results are in `diagnostic-compact-tests.log`.
Host results are in `compact-bindings-host-tests.log`.
All three logs are under `build/hardware-reflection-evidence/`.

## Independent review

The first read-only review found two P2 defects.
Failed PCI probes lacked retained state when the driver never published a binding.
The correction records failed attempts independently and adds a regression to the existing failed-probe host test.
The USB heading also differed from the shared persistence test expectation.
The test now expects the controller-independent heading.

Additional host assertions cover USB root/hub parents, filtered controller observations, and capture overflow.
The correction review found no remaining blockers.
A combined host-test run then identified shared diagnostic-array state between two tests.
A test-only reset and RAII cleanup corrected that isolation defect; the same sequence now passes.
A third independent review approved the correction. The host suite passed 589 tests at that stage.
All reviews were read-only and covered the complete implementation plus correction interactions.
Final validation identified two additional blockers after the third review.
CI produced a 437640-byte BIOS kernel, which exceeded the 434176-byte slot by 3464 bytes.
The framebuffer test also assumed two `os` children, although hardware reflection exposes four.

The linker now places `.text` and `.rodata` alignment tails in zero-filled memory.
It preserves initialized bytes, segment addresses, permissions, memory extents, and page-aligned boundary symbols.
Unused text padding changes from alignment instructions to zero bytes.
The local BIOS kernel decreases from 433288 to 428680 bytes; the staging limits remain unchanged.
An identical-object comparison verifies the loaded content before each alignment tail.
Two linker fixture checks cover aligned sections, short sections, zero-filled tails, and the existing layout gate.
The framebuffer test derives its cleared-row expectation from live `os` children.
Both BIOS and UEFI framebuffer checks pass after this correction.
The fourth read-only review found no blocking defects in these corrections or their interactions with the complete slice.
It independently passed both linker fixtures and checked both loader implementations.
The next CI run passed file-size validation but exceeded the reserved kernel memory window by 4096 bytes.
Repeated driver-name storage accounted for 9216 bytes of static snapshot data.
The corrected snapshot uses 1024 bytes for names and references, plus small counters.
It retains sixteen distinct copied names, matching the existing PCI driver registry capacity.
Capacity overflow returns `LIMIT`; it cannot silently report incomplete binding names.
A regression covers all PCI slots, caller-string mutation, shared names, overflow, and reset.
The fifth read-only review found no blocking defects in copied-name ownership, indexing, capacity handling, or reset.
The reserved memory window and post-image reserve remain unchanged.
CI now passes both unchanged boot limits.
The final local host suite passes 590 tests.
The final guest matrix passes all 77 checks; the diagnostic image passes three additional checks.
Five independent code review cycles found no remaining blockers after verified corrections.
A final independent documentation review verified measurements, artifact sizes, test totals, and archive links.
CI run 42644741594 passes host tests and both boot layout gates; its guest checks remain in progress.
Physical hardware behavior remains unmeasured until the user tests the new image.
The complete ASD-STE100 Issue 9 dictionary is unavailable locally; prose receives a manual clarity and sentence-length review.
