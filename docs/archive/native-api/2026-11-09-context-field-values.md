# Passive hardware field display

The context inspector shows cached hardware field values and identifies empty collections.
It preserves passive inspection and does not invoke methods or property getters.

```text
boot observation -> copied hardware field -> context description and value
empty collection -> context heading -> (no children)
```

## Requirements and impact

| Area | Assessment |
| --- | --- |
| Project scope | GOALS: Platform discovery and Native system interface. Improve inspection of existing records. |
| Boot and platform | No loader, firmware, driver, or BootInfo change. Check image size because the initrd slot is full. |
| Memory and isolation | Read copied observation fields. Keep bounded stack storage and existing read-only observation authority. |
| Concurrency | No lifetime, locking, or scheduling change. Output retains the existing stream synchronization rules. |
| Interfaces | Preserve discovery and observation ABI layouts. Add values to textual hardware descriptions and identify empty collections. |
| Runtime resources | Measure shell and initrd sizes and matched inspection workloads. Reuse numeric output instead of adding another formatter. |
| Validation and documents | Reproduce the old output. Test field values, empty USB collections, passive method inspection, and BIOS/UEFI navigation. |

Architecture sections: Passive hardware reflection and Live context exploration.
The Native API index identifies the runtime discovery and hardware observation contracts as implemented profiles.
Functional device interfaces remain accepted future work.

## Plan

1. Preserve member kinds and signatures.
2. Append values only for external hardware fields from cached observation descriptions.
3. Print an explicit message when enumeration returns no children.
4. Verify that inspecting a shutdown method does not execute it.
5. Check both firmware paths, resource costs, documentation, and independent review.

## Validation

The baseline image fails the new PCI field-value assertion, as expected.
The USB heading was the existing empty-collection result; default QEMU does not attach USB hardware.
The numeric output helper now emits one complete number and returns write failure to its caller.
Previously, it wrote one digit per call and discarded write errors.
The change reuses this helper to avoid a separate hardware-value formatter.

The unchanged image-layout gates pass.
All 590 host tests pass. Eight affected BIOS/UEFI tests also pass with four guest CPUs.
These tests cover context output, keyboard navigation, typed shell commands, and filesystem metadata tools.
The independent code review found no blockers.

Commands:

```sh
cmake --build build -j 6
cmake --build build-host-tests -j 6
ctest --test-dir build-host-tests --output-on-failure
ctest --test-dir build --output-on-failure -R '^os1_smoke_(context(_keyboard)?|typed_shell|tree_tools)_(bios|uefi)$'
```

## Comparative measurements

Baseline revision: `5e3a07e5592b1b414fd5eb37c890f25162af7b5d`.
Candidate: the source change recorded here, applied to that revision.
The source diff SHA-256 is `fc2942d47e82fb7417e57b43fa8a15a3672ca8dc1bb27424b00808421c458118`.
Artifact hashes and sizes are in `build/context-values-evidence/artifacts.json`.

Both images use GCC 11.2.0, GNU ld 2.37, Debug, KMEM_DEBUG, and QEMU_DEBUGCON.
The host is an Apple M4 Max with sixteen physical cores and 64 GiB RAM, running macOS 26.6.2.
QEMU 11.1.3 uses q35, TCG, four CPUs, and 128 MiB RAM.
UEFI uses installed `edk2-x86_64-code.fd` and fresh `edk2-i386-vars.fd` copies.
BIOS uses the default QEMU firmware.
Each run receives a disposable program-disk copy.

The workload runs twenty `context os.devices.inventory.pci.pci_0` commands.
Allocator observations run before and after those commands.
Three alternating runs per revision and firmware produce the medians below.
QEMU CPU time covers each complete process run, including boot.
Host background applications were not controlled.

| Property | Baseline | Candidate | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| BIOS inspection workload, seconds | 5.444419 | 5.974346 | +0.529927 | +9.73% |
| BIOS boot, seconds | 0.477119 | 0.474277 | -0.002842 | -0.60% |
| BIOS QEMU CPU, seconds | 6.399350 | 7.022583 | +0.623233 | +9.74% |
| UEFI inspection workload, seconds | 2.548239 | 2.747154 | +0.198915 | +7.81% |
| UEFI boot, seconds | 12.161502 | 12.158390 | -0.003112 | -0.03% |
| UEFI QEMU CPU, seconds | 5.317541 | 5.619285 | +0.301744 | +5.67% |
| Shell ELF, bytes | 165536 | 165640 | +104 | +0.06% |
| Initrd, bytes | 147456 | 147456 | 0 | 0.00% |

The additional field values increase output and elapsed inspection time.
No run reports an allocator failure. This workload does not establish physical-device latency or a leak bound.
The initrd remains at its existing limit; moving the full shell to ext2 is separate work.
The measurement script and raw results remain under `build/context-values-evidence/`.

Evidence is stored under `build/context-values-evidence/`.
Physical hardware behavior remains unmeasured.
The complete ASD-STE100 Issue 9 dictionary is unavailable; technical prose receives a manual review.
