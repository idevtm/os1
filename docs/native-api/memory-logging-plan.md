# Memory logging and command correlation

This plan implements the first diagnostic slice with bounded memory and explicit correlation.
The baseline is `146c0c84498ceba6159e945451dfbd533f98357f`; the branch is `codex/memory-logging`.

```text
shell command -> kernel-assigned command identity -> child process / retained IPC Operation
      |                         |                                  |
 shell history            kernel event ring                 completion record
      |                         |
 own-process queries       authorized Native Log queries
```

## Requirements and scope

Implement the [accepted budgets](context-diagnostics.md#accepted-defaults--2026-11-08) for memory capture.
Keep persistence, export staging, completion UI, and general discovery for later slices.
Use kernel-authenticated process identity; user data must not forge kernel events or another command identity.
Retain IPC correlation through asynchronous completion and failure.
Expose bounded filtered queries and safe text presentation through shell bindings and aliases.
History suppression must happen before capture. Log readers must escape terminal control bytes.

## Impact assessment

| Area | Requirements and effects |
| --- | --- |
| Scope | GOALS: Native system interface; Testability and observability. Keep capture, queries, and shell presentation separate. |
| Boot/platform | ARCHITECTURE: shared kernel initialization and BIOS image bounds. Initialize bounded storage after allocator startup; check both boot paths. |
| Memory/isolation | Enforce normal/diagnostic budgets, shell aggregate accounting, cleanup, pointer validation, and separate read authority. |
| Concurrency | Capture must not wait for disk or allocate; avoid lock recursion. IPC completion retains immutable origin data. |
| Interfaces | Add bounded log/context records and a Native Log provider. Preserve existing observation records and startup ABI. |
| Resources | Measure kernel image size, physical memory, command workload latency, and capture overhead against the baseline. |
| Validation/docs | Host ring, filter, quota, authority, and correlation tests; UEFI/BIOS and SMP smoke coverage; independent adversarial review. |

## Implementation sequence

1. Define records, bounded ring algorithms, and loss accounting.
2. Add kernel capture, authorized queries, shell-context accounting, and lifecycle cleanup.
3. Propagate command identity through launches and retained IPC Operations.
4. Add shell history, log aliases, filters, and safe rendering.
5. Add host and guest regression coverage; compare baseline resources and performance.
6. Review the complete diff independently; fix confirmed defects and repeat affected validation.

## Evidence

Baseline build: `cmake --build build -j 4` passed before source changes.
The memory log ABI defines the implemented subset and its limits.
Capture uses scheduling time with explicit coarse resolution; it does not claim high-resolution timestamps.
Paired export clock anchors and persistent boot identity remain requirements for the export slice.

### Build and regression checks

- `cmake --build build -j 4`: passed with GCC 11.2.0, Debug, `-Os`, LTO, and kmem checks.
- `cmake --build build-host-tests -j 4`: passed with the host AppleClang toolchain.
- `ctest --test-dir build-host-tests --output-on-failure`: 522 tests passed.
- The focused UEFI/BIOS run passed 12 checks for logging, lifecycle, observation, spawn, exec, and SMP balance.
- `ctest --test-dir build --output-on-failure`: all 62 checks passed in 382.29 seconds; see `build/logging-evidence/final-smoke.txt`.
- Both boot paths passed log/history queries without a data disk.
- A UEFI image with `os1.log=diagnostic` passed the 1 MiB capture-profile regression.

The initial broad run exposed a filesystem-only rights mask that accidentally included the new LOG_READ authority.
The fixture now excludes that authority and also verifies that namespace lookup cannot recover it.
Some local monitor tests required permission to create Unix sockets.
A concurrent fixture rebuild also interrupted two image-construction checks; final reruns use stable artifacts.
These validation-environment failures are separate from the corrected rights-mask regression.

The final matrix supersedes the earlier focused checks.
These commands repeat the complete matrix and the separate diagnostic-profile regression:

```sh
ctest --test-dir build --output-on-failure
python3 tests/scripts/test_memory_logging.py --diagnostic \
  --qemu /opt/homebrew/bin/qemu-system-x86_64 \
  --image build/logging-evidence/diagnostic-final.iso \
  --program-disk build/artifacts/program-test-disk.raw \
  --code /opt/homebrew/share/qemu/edk2-x86_64-code.fd \
  --vars /opt/homebrew/share/qemu/edk2-i386-vars.fd \
  --log build/logging-evidence/diagnostic-uefi.log
```

The diagnostic image contains the same kernel and initrd, with `os1.log=diagnostic` in both Limine configuration copies.
The no-disk commands are recorded in `build/logging-evidence/no-disk-{bios,uefi}-command.txt`.
Those runs remove the data device, query both log capacities, and reject panic or general-protection faults.

### Independent review

The independent reviewer inspected the full source diff and the accepted requirements without editing files.

| Finding | Correction and regression |
| --- | --- |
| P1: console echo entered kernel text capture | Echo bypasses capture; guest tests inspect kernel records for suppressed commands and input bodies |
| P2: exec retained an active command scope | Successful exec marks REPLACED and releases scope ownership; host and guest tests cover subsequent commands |
| P2: explicit zero cursor selected the recent window | Track explicit cursor presence; guest tests inspect earlier retained records |
| P2: failed write/edit reported success | Helpers return status; guest history tests check usage failures |

The second review accepted these corrections.
Diagnostic-profile testing then exposed excessive renderer tracing and filtered-query selection problems.
Text output now uses a 4 KiB batch buffer; filtered queries start at retained history unless a cursor specifies otherwise.
The third source review accepted this change and the complete integration.
A subsequent narrow review accepted the rights-mask fixture correction without weakening timestamp authority checks.
Another review accepted the guest-clock synchronization correction in the timestamp fixture.
That fixture now observes a separate RAM file before it checks one target timestamp update.
Both firmware paths passed the corrected check; target advancement and persistence assertions remain unchanged.

### Comparable resource measurements

The baseline revision is `146c0c84498ceba6159e945451dfbd533f98357f`.
The candidate uses this branch's memory-log implementation; the kernel hash below identifies the measured binary.
Both builds use GCC 11.2.0, Debug configuration, `-Os`, LTO, and enabled kmem checks.
The host runs macOS 26.6.2 on ARM64; QEMU 11.1.3 uses TCG, q35, four CPUs, and 128 MiB RAM.
BIOS uses the project loader; UEFI uses Limine 11.4.0 and the Homebrew OVMF firmware.
Each profile runs three trials of 20 sequential `echo` commands, followed by `sys` and `kmem`.
Elapsed times use the host monotonic clock; physical memory use comes from the guest page allocator.

| Measurement | Baseline | Candidate | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| BIOS, median 20-command time | 155.461 ms | 147.974 ms | -7.487 ms | -4.82% |
| UEFI, median 20-command time | 114.999 ms | 111.451 ms | -3.548 ms | -3.09% |
| BIOS, allocated physical memory | 3352 KiB | 3708 KiB | +356 KiB | +10.62% |
| UEFI, allocated physical memory | 20772 KiB | 21140 KiB | +368 KiB | +1.77% |
| Kernel ELF size | 543960 bytes | 555032 bytes | +11072 bytes | +2.04% |
| UEFI image size | 4362240 bytes | 4378624 bytes | +16384 bytes | +0.38% |
| BIOS image size | 1474560 bytes | 1474560 bytes | 0 bytes | 0% |

BIOS baseline trials were 165.641, 155.461, and 153.647 ms; candidate trials were 153.936, 147.974, and 147.216 ms.
UEFI baseline trials were 68.139, 116.278, and 114.999 ms; candidate trials were 70.093, 118.011, and 111.451 ms.
This small sample does not establish a speed improvement or isolate capture cost from emulator variability.
Slab-page use remained 168 pages in both profiles.
Memory use increased as expected for the ring, shell history, and correlation bookkeeping.
The measured profile satisfies the payload budgets; complete capture under contention is not guaranteed.
Diagnostic-mode timing, worst-case capture latency, and physical-device overhead remain unmeasured.
The diagnostic regression verifies behavior but does not use a controlled timing comparison.
Repeat the same baseline workload with diagnostic capture before selecting that profile for continuous use.
No isolated capture-latency harness or physical test session was available for this task.
Add a bounded capture benchmark, then measure interrupt latency and contention on both physical test systems.

The BIOS kernel is 413952 bytes within its 425984-byte staging slot.
The initrd is 96256 bytes within its 147456-byte staging slot.
Raw transcripts and configuration are in the local `build/logging-evidence/` directory.
Files `baseline-{bios,uefi}.txt` and `candidate-{bios,uefi}.txt` contain timing and allocator results.
The `.logging.log` companions contain serial transcripts; `measurements.json` contains calculated differences.

Use this command to repeat the BIOS workload on the current build:

```sh
python3 tests/scripts/test_memory_logging.py --baseline \
  --qemu /opt/homebrew/bin/qemu-system-x86_64 \
  --image build/artifacts/os1.raw \
  --program-disk build/artifacts/program-test-disk.raw \
  --log build/logging-evidence/repeat-bios.log
```

For UEFI, select `build/artifacts/os1.iso` and a separate output log.
Add `--code /opt/homebrew/share/qemu/edk2-x86_64-code.fd` and `--vars /opt/homebrew/share/qemu/edk2-i386-vars.fd`.
The `--baseline` flag selects only the common workload; it does not select a source revision.

| Artifact | SHA-256 |
| --- | --- |
| Candidate kernel ELF | `8a8130938099aa136b43d3eb4b046d31c684914881f1828f1c99ef5bba6440ea` |
| Candidate BIOS kernel | `1708c6a77d2f3a8de87b36a662a3efdc7a0d32b1e53bb308e1979207d3c9ea26` |
| OVMF code | `33090cc07675baa5190d9f1e84bf5176b33bcbfa9bacac522961150cdb6dbb2a` |
| OVMF variables template | `5d2ac383371b408398accee7ec27c8c09ea5b74a0de0ceea6513388b15be5d1e` |

### Resource changes and limits

The shared reserved kernel window ends at `0x198000`, an increase of 16 KiB from `0x194000`.
This preserves the required 16 KiB post-image reserve; the BIOS staging slots remain 416 KiB and 144 KiB.
The stripped kernel remains within the existing disk and staging bounds.
The normal ring has 256 KiB of payload; each allocated history adds 64 KiB plus bookkeeping.
The maximum history payload remains 256 KiB system-wide.
The export staging buffer is not allocated in this memory-only slice.

Worst-case physical interrupt latency and physical-device capture contention are unmeasured.
QEMU workload timings do not establish those properties; hardware qualification remains necessary.
No physical-hardware result is claimed for this change.
The complete ASD-STE100 Issue 9 standard and dictionary are unavailable; language review follows repository rules.
