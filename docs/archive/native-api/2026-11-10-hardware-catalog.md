# Hardware catalog implementation evidence — 2026-11-10

The catalog adds bounded boot records and disk-based inspection programs.
This record separates automated results from physical-hardware acceptance.

```text
baseline 98f2b3a -> candidate codex/hardware-catalog
three alternating repeats per firmware -> median time and memory comparison
```

## Measurement configuration

The baseline is `98f2b3a7b3e510af619314a48baaceba6875399a`.
The candidate uses the final implementation on `codex/hardware-catalog`.
[Raw measurements](2026-11-10-hardware-catalog-measurements.json) identify artifacts through SHA-256 hashes.
The host uses an Apple M4 Max, 16 CPU cores, and 64 GiB RAM.
The operating system is macOS 26.6.2, build 25G83.

The cross compiler is GCC 11.2.0. QEMU is version 11.1.3 with TCG acceleration.
Guests use `q35`, four CPUs, and 128 MiB RAM.
The default Debug preset uses `-Os` for kernel and user programs, with kernel memory diagnostics enabled.
The raw report records OVMF image paths and hashes.
BIOS runs use the firmware bundled with this QEMU installation.

Each revision uses its own boot and program-disk images.
Each firmware has three alternating baseline and candidate runs.
The workload reads `os.devices.inventory.pci.pci_0.vendor_id` through `context` 20 times.
`sys` and `kmem` records bracket the workload.
Times include the existing QEMU harness and console processing; they are not isolated syscall measurements.

## Measured time

All values are medians in seconds. Positive differences indicate additional time.

| Workload | Baseline | Candidate | Difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| BIOS Boot | 0.430928 | 0.454979 | +0.024051 | +5.58% |
| BIOS 20 inspections | 3.645777 | 3.711345 | +0.065568 | +1.80% |
| BIOS QEMU CPU | 4.454655 | 4.633293 | +0.178638 | +4.01% |
| UEFI Boot | 1.989437 | 1.983026 | -0.006411 | -0.32% |
| UEFI 20 inspections | 1.833731 | 1.869278 | +0.035547 | +1.94% |
| UEFI QEMU CPU | 4.116349 | 4.164334 | +0.047985 | +1.17% |

These small samples do not establish a performance guarantee.
BIOS boot increased by approximately 24 ms; UEFI boot decreased by approximately 6 ms.
The 20-inspection workload changed by +1.8% for BIOS and +1.9% for UEFI.
The implementation adds catalog capture and larger discovery descriptions.

## Memory and image costs

| Resource | Baseline | Candidate | Difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| Kernel ELF bytes | 578072 | 586440 | +8368 | +1.45% |
| Rescue initrd bytes | 7680 | 7680 | 0 | 0% |
| BIOS image bytes | 1474560 | 1474560 | 0 | 0% |
| UEFI ISO bytes | 4313088 | 4321280 | +8192 | +0.19% |
| Program-disk image bytes | 34603008 | 34603008 | 0 | 0% |
| New utility executable bytes | 0 | 17024 | +17024 | Undefined |
| Catalog allocation bytes | 0 | 151616 | +151616 | Undefined |
| BIOS used physical pages before workload | 1066 | 1105 | +39 | +3.66% |
| UEFI used physical pages before workload | 5433 | 5474 | +41 | +0.75% |

Physical pages contain 4096 bytes. The observed increase is 156 KiB for BIOS and 164 KiB for UEFI.
`Catalog` contains 256 entries; each record contains 584 bytes.
The allocation includes entry IDs, counts, alignment, and publication state.
Memory allocation failures remained zero during every measured workload.
Both revisions used one additional physical page across the workload.

The four new executables occupy 17024 bytes before filesystem allocation overhead.
`lshw` uses 4280 bytes; each other utility uses 4248 bytes.
The fixed-size program-disk image does not show internal filesystem space consumption.
The BIOS staging area now reserves 504 KiB for the kernel and 64 KiB for the rescue initrd.
The physical kernel reservation still ends at `0x1A0000`, before the page-frame bitmap.
This exchanges unused initrd staging capacity for kernel growth without enlarging the disk image.

## Commands and local evidence

```sh
python3 tests/scripts/measure_hardware_catalog.py \
  --baseline build/hardware-catalog/baseline \
  --candidate build/artifacts \
  --output build/hardware-catalog/measurements \
  --qemu /opt/homebrew/bin/qemu-system-x86_64 \
  --code /opt/homebrew/share/qemu/edk2-x86_64-code.fd \
  --vars /opt/homebrew/share/qemu/edk2-i386-vars.fd
./scripts/check-local.sh
```

Measurement console logs remain under `build/hardware-catalog/measurements/`.
`full-local-2.log` records the final local command after the host include correction.
The rebuilt host suite passed 606 tests in 20.46 seconds.
The normal-image suite passed 86 checks in 371.02 seconds.
The diagnostic suite passed all three checks in 5.82 seconds.
The complete local command returned status 0.
The normal suite includes six new catalog smokes and both firmware paths.
No checks were disabled or skipped.
Local document targets, Python syntax, JSON syntax, whitespace checks, and secret review passed.

## Failure evidence and corrections

| Evidence under `build/hardware-catalog/` | Finding and correction |
| --- | --- |
| `build.log` | An unintended xHCI signature edit failed compilation. The signature was corrected. |
| `build-2.log` through `build-5.log` | Catalog initialization and kernel growth exceeded the old BIOS slot. Zero initialization and the staging split corrected these failures. |
| `qemu-targeted-1.log` | An enlarged kernel reservation overlapped the frame bitmap. The catalog now uses boot-time heap allocation. |
| `qemu-targeted-2.log` | A smoke callback used the wrong signature. The callback now accepts the harness arguments. |
| `qemu-targeted-3.log` through `qemu-targeted-6.log` | Exact names with longer prefix matches kept completion open. Exact matches now close automatic menus. |
| `qemu-targeted-7.log` | Six catalog tests and both serial context tests passed. Two framebuffer tests could not create sandboxed monitor sockets. |
| `full-local-1.log` | The host tests lacked the memory-layout header. The explicit include corrects compilation. |

Earlier targeted commands ran an old host-test binary after compilation failed.
Those runs do not validate the final test source.
The complete local command rebuilds before running tests and preserves each failure log.
The final QEMU suite requires permission to create local monitor sockets.

## Independent review and limitations

Three independent source-review cycles examined the complete task diff and correction interactions.
Confirmed findings covered failed configuration visibility, controller retirement, and malformed descriptor reporting.
Corrections preserve failed device records and propagate unavailable state before catalog publication.
Host regressions cover failure reporting, bounds, freeze rules, and exact-name completion.
The third review found no remaining source defects, subject to final validation.
A separate follow-up reviewed the missing-header correction, formatting cleanup, and measurement evidence.
It found no remaining defects. The full command confirms that the final host tests rebuilt successfully.

Physical-hardware behavior remains unmeasured for this change.
Retest `lshw`, `lspci`, `lsusb`, `lsblk`, context values, and Tab navigation on both physical devices.
Runtime hotplug remains planned in the [catalog plan](../../native-api/hardware-catalog-plan.md#follow-up-hotplug-state-and-reflection-updates).
The complete ASD-STE100 standard and dictionary were unavailable; full language compliance remains unverified.
