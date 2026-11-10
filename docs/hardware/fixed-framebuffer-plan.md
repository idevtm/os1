# Fixed framebuffer display sessions

This plan adds an optional graphics driver and an interactive pointer demo.
The driver matches the firmware framebuffer and loads from the system ext2 volume.

```text
BootInfo -> validated framebuffer -> trusted disk module -> display session
Console cells -> text renderer -> framebuffer when the display has no session
Pointer events -> demo -> bounded pixel updates
```

| Area | Impact |
| --- | --- |
| Scope | GOALS “Driver model” and “Optional desktop variant” gain a runnable graphics example. |
| Boot and platform | Use the existing firmware handoff. VGA text boots keep the console without a graphics session. |
| Memory and isolation | Keep framebuffer addresses in the kernel. Validate rectangle bounds and limit each update. |
| Concurrency | Serialize framebuffer writes with console presentation. Restore text when the final session reference closes. |
| Interfaces | Add versioned display discovery and session interfaces. Keep existing console and input contracts. |
| Resources | Measure module, kernel, demo, and image sizes. Measure acquisition, drawing, and restoration in QEMU. |
| Validation | Test supported and unsupported modes, exclusive acquisition, bounds, and release. Run the full local suite. |

The graphics driver uses the firmware mode as its hardware match.
The display reports one mode when the package loads and its probe succeeds.
The demo asks the user to select that mode before it acquires the display.
The demo draws a software pointer with the Native API input service.

## Implementation and acceptance

The [Display ABI](../native-api/abi/display-abi.md) defines the session and the demo procedure.
The [module profile](driver-module-abi.md#firmware-framebuffer-module) defines the package and its trust check.
Use the USB menu action **Update boot and programs** for a managed stick.
Boot through UEFI to test graphics. Run `context os.display`, then run `gfxdemo`.

| Check | Evidence |
| --- | --- |
| UEFI graphics | The guest reports a 1024 by 768 RGB32 mode and a loaded ext2 module. |
| Session lifetime | The check rejects an invalid mode, a competing acquire, and an out-of-range fill. |
| Retained client | A bound client keeps the session after the raw descriptor closes. |
| Interactive demo | QMP selects mode 1, moves the USB pointer by 19 and 13 pixels, and quits. |
| Console restoration | The QEMU screenshot contains text pixels after the graphics background disappears. |
| Package trust | A corrupt display ELF fails digest verification. The console remains usable. |
| BIOS | VGA text reports no compatible graphics mode. The shell continues. |

Early focused runs exposed two issues.
The demo initially selected the PS/2 pointer ahead of the USB mouse.
The first restoration assertion expected white glyphs. The text console uses gray glyphs.
The final focused test checks the actual pointer position and gray text pixels.
The original transcripts remain under `build/artifacts/test-runs/`.
See `build/display/focused-first.log` through `build/display/focused-final.log`.

Independent review identified a race between console echo and graphics ownership.
Echo, backspace, terminal switching, and display operations now use the output lock.
The input path takes the input lock before the output lock.
The review found no reverse lock order on the examined paths.
The final package script preserves Python 3.9 compatibility.

The first complete run passed 681 host tests, 93 normal-image checks, and three diagnostic checks.
The packaging check still expected one module. The revised check requires both complete packages and validates their digests.
The focused packaging check passed. The first full log and profile logs remain in `build/display/`.
The independent reviewer found no blockers in the revised contract.

## Resource limits

The comparison uses a clean build of `f1dbfb5` and that revision plus this feature.
Both builds use GCC 11.2.0, Debug configuration, and kernel `-Os`.
Both enable kernel memory checks and QEMU debug output.

| Artifact | Baseline bytes | Candidate bytes | Difference |
| --- | ---: | ---: | --- |
| Kernel ELF | 620,384 | 624,776 | +4,392 bytes, +0.71% |
| UEFI ISO | 4,356,096 | 4,362,240 | +6,144 bytes, +0.14% |
| BIOS image | 1,474,560 | 1,474,560 | 0 bytes, 0% |
| Initrd | 10,240 | 10,240 | 0 bytes, 0% |
| Program disk image | 34,603,008 | 34,603,008 | 0 bytes, 0% |
| Display ELF | 0 | 8,952 | +8,952 bytes |
| Demo ELF | 0 | 8,024 | +8,024 bytes |

The relative difference is undefined for new files.
The fixed disk image size includes free space.
The ext2 fixture increases from 158 to 165 used 4 KiB blocks.
This uses 28,672 additional bytes, an increase of 4.43%.
Used inodes increase from 54 to 58, an increase of 7.41%.
The graphics module uses two mapped pages for code and read-only data in this build.
Its image snapshot uses temporary kernel memory. The module mappings remain until reboot.
A session uses a NativeObject and a descriptor instance. It does not allocate a second framebuffer.
A fill covers at most 4096 pixels. Restoration redraws the full active text grid once.
The console output lock holds local interrupts disabled during the redraw.
The restoration cost at the maximum geometry and on physical hardware remains unmeasured.

The final UEFI run measured 4 one-pixel fills and their surrounding calls.
Guest elapsed time spans were 0.100 to 0.261 ms for acquisition and 0.088 to 0.223 ms for fill.
Final close plus redraw took 14.845 to 15.247 ms.
These samples include syscall or IPC costs. They do not measure the exact interrupt-disabled interval.
The baseline has no display session, so the relative differences for these operations are undefined.
See `build/artifacts/test-runs/os1_smoke_display_session_uefi-fdes0z67/guest.display.log`.

The isolated comparison used QEMU 11.1.3 TCG on arm64 macOS 26.6.2.
Each q35 guest used four CPUs and 128 MiB. Only one guest ran at a time.
The UEFI guest used the Homebrew OVMF firmware and a USB data disk.
The BIOS guest used the bundled BIOS and a virtio data disk.
Each row uses three samples per tree and reports the median.
Raw samples and artifact hashes are in `build/display/boot-comparison.json`.

| Workload | Baseline | Candidate | Difference |
| --- | ---: | ---: | ---: |
| UEFI startup | 2.425 s | 2.456 s | +0.031 s, +1.27% |
| UEFI echo | 4.894 ms | 5.596 ms | +0.702 ms, +14.34% |
| BIOS startup | 0.492 s | 0.488 s | -0.004 s, -0.87% |
| BIOS echo | 12.205 ms | 12.651 ms | +0.447 ms, +3.66% |

The startup sample includes firmware and shell startup. The echo sample includes shell input, execution, and output.
These short runs do not establish a performance trend.

## Final validation

The full local command ran on `f1dbfb5` plus the final source and test diff.
The normal build identity is `44755c0ca83dc63cb91e65f7d4b19ca312a5f9e809a9df0293154fa38c9c5174`.
The executable and test diff SHA-256 is `776e5c4aeb9cde5f781ea43fb42280acee1f4a33faf5e0a14d098ec8b918375f`.

| Profile | Passed | Duration |
| --- | ---: | ---: |
| Host | 681 / 681 | 21.65 s |
| Normal image | 94 / 94 | 387.82 s |
| Diagnostic image | 3 / 3 | 9.74 s |

The command log is `build/display/full-local-qualified.log`.
The profile logs remain in `build/local-validation/`.
The independent review covered the loader, capability lifetime, locking, renderer, demo, and deployment.
The reviewer verified the corrections and found no merge blockers.

Physical graphics acceptance remains pending.
The complete ASD-STE100 dictionary was unavailable. The prose received a manual check against the project rules.
