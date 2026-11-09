# BIOS ELF size correction

This correction must fit the BIOS kernel into its existing staging slot without reducing memory or image safety checks.
CI with GCC 16.2.0 produced 434432 bytes against a 434176-byte slot, a 256-byte overflow.

```text
Initialized .data bytes -> ELF file -> loader copy
Alignment + .bss       -> ELF memory range -> loader zero-fill
```

## Plan and impact

Move final data alignment outside the file-backed linker section.
Preserve page-aligned section boundary symbols and the existing zero-fill contract.
Reproduce the overflow with a linker fixture before changing the script.
Verify both boot paths, memory permissions, image limits, and CI after the correction.

| Area | Assessment |
| --- | --- |
| Project scope | Changes kernel packaging only; preserves the shared-kernel boot model |
| Boot and platform | Changes ELF file extent; preserves segment addresses, permissions, BIOS slots, and firmware bounds |
| Memory and isolation | Requires zero-fill of alignment space and unchanged page boundaries; no allocator or user-pointer changes |
| Concurrency | No scheduling, locking, interrupt, or object-lifetime change |
| Interfaces | Preserves BootInfo, UAPI, and Native API layouts; changes only kernel ELF packaging |
| Runtime resources | Measure file size, loaded extent, and representative boot behavior against the previous revision |
| Validation and documents | Linker regression, BIOS/UEFI checks, full CI, updated architecture, and independent review |

Requirements come from GOALS `Boot strategy` and architecture `Build Outputs And Boot Artifacts` and `The Shared Kernel Contract: BootInfo`.
The [Native API index](../../native-api/README.md) remains authoritative for implemented profiles; this correction changes none of them.

## Validation

The linker fixture failed before the correction because `.data` contained 512 alignment bytes in the file.
Its original ELF size was 434432 bytes, matching the CI overflow boundary.
After correction, its size is 433920 bytes and the unchanged boot-contract check passes.
The fixture verifies initialized bytes, zero-fill extent, segment flags, virtual/physical addresses, and page-boundary symbols.

The first affected run passed thirteen checks and exposed two validation issues.
The UEFI spawn check exhausted its deadline; the linker check used a stale generated command after its new `--nm` argument was added.
Reconfiguration resolves the stale test command; the linker regression passes with its final invocation.
The independent spawn-timeout reproduction and correction are described below.
The final spawn and linker recheck passes both tests; all fifteen affected checks now have passing results.
Three additional diagnostic-image checks pass, including BIOS and UEFI capture.

Affected guest checks cover BIOS/UEFI boot, SMP observation, spawn/exec, log export, system lifecycle, menu profiles, and interrupt handoff.
No owned C/C++ source changed, so clang-format was not applicable.
The existing host suite does not link the kernel script; CI still runs it alongside the new linker regression.

Commands:

```sh
cmake --build build --target os1_image os1_bios_image os1_usb_image
ctest --test-dir build --output-on-failure --no-tests=error -R '^os1_(kernel_elf_padding_contract|smoke(_bios|_observe|_observe_bios|_spawn|_spawn_bios|_exec|_exec_bios)?|smoke_log_export_(bios|uefi)|smoke_system_lifecycle_(bios|uefi)|smoke_boot_menu_uefi|interrupt_handoff_contract)$'
ctest --test-dir build --output-on-failure --no-tests=error -R '^os1_(smoke_spawn|kernel_elf_padding_contract)$'
```

Local logs are under `build/bios-size-evidence/`.
CI provides separate GCC 16.2.0 validation of the committed revision.

## Menu timeout correction

PR 43 introduced a ten-second menu wait while retaining the old thirty-second UEFI smoke deadline.
The pre-fix image also times out in the spawn workload at thirty seconds.
The same image passes all 56 markers with a forty-second total deadline.
Thus, this timeout predates the linker correction.
The corrected UEFI deadline includes ten menu seconds plus the original thirty-second test allowance.
BIOS timing and required markers remain unchanged.

To repeat the control, obtain the `os1_smoke_spawn` command from `ctest --test-dir build --show-only=json-v1`.
Set `ISO_IMAGE` to the saved pre-fix ISO.
Use separate log files for the two runs.
Run it with `SMOKE_TIMEOUT_SECONDS=30`, then `SMOKE_TIMEOUT_SECONDS=40`.
The local transcripts are `baseline-spawn-30.txt` and `baseline-spawn-40.txt`.

## Measurements

The baseline code is revision `7fd618a`; the saved artifacts precede this correction.
That revision changed only documentation after the hardware-tested kernel build.
The candidate is the current working tree, identified by its generated source fingerprint.
The [measurement manifest](2026-11-09-bios-size-measurements.json) records artifact hashes, sizes, and loaded-segment hashes.
Each artifact was measured once; file size is deterministic for that build.

| Artifact | Before | After | Absolute change | Relative change |
| --- | ---: | ---: | ---: | ---: |
| BIOS kernel ELF | 430336 bytes | 429192 bytes | -1144 bytes | -0.266% |
| Kernel ELF with symbols | 576400 bytes | 575256 bytes | -1144 bytes | -0.198% |
| UEFI ISO | 4448256 bytes | 4446208 bytes | -2048 bytes | -0.046% |
| BIOS raw image | 1474560 bytes | 1474560 bytes | 0 bytes | 0% |
| Writable segment memory | 229376 bytes | 229376 bytes | 0 bytes | 0% |

Loaded `.text` and writable-segment hashes match before and after the correction.
The read-only segment changes only for generated build-identity strings.
All segment addresses, flags, and memory extents remain unchanged.
The BIOS loader still reads the fixed slot; its disk-read volume does not change.
Both loaders already zero the full memory extent, so the correction only removes redundant initialized padding bytes.
No runtime latency improvement is claimed or measured.
The deterministic memory footprint and file-size comparison establish the affected resource changes.

The local compiler is x86_64-elf GCC 11.2.0 on Apple M4 Max with macOS 26.6.2.
Build settings are Debug, `OS1_KMEM_DEBUG=ON`, and `OS1_QEMU_DEBUGCON=ON`.
Guest checks use QEMU 11.1.3, TCG, q35, four CPUs, and 128 MiB RAM.
UEFI uses the configured QEMU EDK2 firmware; BIOS uses the raw-image path.

## Review and limits

Independent review checked both loaders, the linker script, memory boundaries, and the regression fixture.
The correction review also checked the timeout controls and final evidence; it found no blockers.
Physical machines have not retested this packaging correction.
The preceding hardware record remains evidence for the original implementation, not this revised artifact.
The complete ASD-STE100 dictionary was unavailable; prose received active-voice, terminology, sentence-length, and link review.

