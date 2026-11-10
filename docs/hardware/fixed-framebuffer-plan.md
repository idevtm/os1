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

## Pointer stalls and runtime modes

The user reports smooth pointer movement on the desktop.
The laptop pointer paused every few seconds during continuous movement.
The cause remains unknown.

```text
pointer events -> wait-many readiness -> bounded event batch -> cursor redraw
display mode list -> exclusive acquire -> QEMU mode switch -> restore on close
```

Make native input subscriptions waitable before changing the display backend.
Measure input publication gaps, event age at consumption, and draw time separately.
Print the summary after the display session closes.
Test keyboard exit with an idle pointer, actual pointer movement, and ring-loss readiness.

Match QEMU Standard VGA by PCI identity and use the Bochs display registers.
Publish only modes the backend can set. Restore the boot mode before the console redraw.
Keep the firmware framebuffer fallback for physical machines.
Defer NVIDIA mode setting to a separate hardware milestone.


### Impact and checks

| Area | Impact |
| --- | --- |
| Scope | The display and input milestones gain runtime mode selection and a test for the reported pauses. |
| Boot and platform | Keep the existing BootInfo layout. Enable mode changes only on matching QEMU Standard VGA. |
| Memory and isolation | Claim and bound the additional mappings. Retain the target geometry in each session. |
| Concurrency | Signal input waiters after controller dispatch. Serialize graphics and console ownership through session lifetime. |
| Interfaces | Add Display `list_modes` and native input readiness. Use internal graphics module ABI 2. |
| Resources | Measure image size, event age, draw duration, and restoration. Keep the diagnostics out of the polling and IRQ paths. |
| Validation | Cover BIOS, UEFI, two output resolutions, fallback, corrupt packages, and native input readiness. |

Run `gfxdemo` on the same laptop with the same mouse to investigate the reported pauses.
Move continuously for at least 20 seconds. Press `q` to stop.
Record the pointer diagnostics and compare the input gaps with the event age and draw duration.
Physical testing of the new event path remains pending.

For QEMU with an existing data disk, run `./os1.sh storage_deploy` before `./os1.sh run`.
Inspect `context os.display` to confirm the backend.
Select an alternate resolution from the `gfxdemo` menu.
The Standard VGA test verifies the new screenshot dimensions and the return to the boot dimensions.


### Runtime display validation

The final `./scripts/check-local.sh` run passed on `16c6086` plus the runtime display changes.
The normal build identity is `aa512090513633ed3c1f24c69885eadfd452ccccf9423bacb453aa456ff92bc9`.
The executable and test diff SHA-256 is `e2b53207c7919273b3cde360635ce4d7b4a29a93d44d756781c4c962e532df1a`.

| Profile | Passed | Duration |
| --- | ---: | ---: |
| Host | 682 / 682 | 22.25 s |
| Normal image | 94 / 94 | 388.70 s |
| Diagnostic image | 3 / 3 | 9.82 s |

The command log is `build/display/runtime-full-qualified.log`. Profile logs are in `build/local-validation/`.
The UEFI display test verified 800 by 600 and 1280 by 720 screenshots and restored text at the boot resolution.
It also verified the fixed framebuffer with VGA MMIO disabled and rejection of a corrupt driver.
The host test covers readiness for matching input, overflow, and removal.
The QEMU test covers pointer motion and idle keyboard exit after an event.
Injected restoration failure and demo resynchronization after overflow remain untested.

The first event-only display checks passed on both boot paths.
The initial combined build lacked two declarations. The build logs remain in `build/display/modes-build*.log`.
Independent review identified producer notification, cursor edge order, restoration failure, and timing sample issues.
The changes address those findings. The final readiness check avoids an atomic exchange on idle IRQs.
The full suite ran again after that correction.

### Runtime display measurements

The isolated comparison uses the same host and QEMU settings as the previous section.
The baseline is a clean build of `16c6086`. The candidate adds the recorded source and test diff.
Each timing row shows the median of three samples per tree.
See `build/display/runtime-boot-comparison.json` for the samples and artifact hashes.

| Workload | Baseline | Candidate | Difference |
| --- | ---: | ---: | ---: |
| UEFI startup | 2.413 s | 2.420 s | +0.008 s, +0.32% |
| UEFI echo | 5.730 ms | 4.971 ms | -0.759 ms, -13.25% |
| BIOS startup | 0.473 s | 0.472 s | -0.001 s, -0.17% |
| BIOS echo | 11.957 ms | 11.830 ms | -0.127 ms, -1.06% |

| Artifact | Baseline bytes | Candidate bytes | Difference |
| --- | ---: | ---: | ---: |
| Kernel ELF | 625,184 | 626,856 | +1,672 bytes, +0.27% |
| UEFI ISO | 4,362,240 | 4,364,288 | +2,048 bytes, +0.05% |
| Display module | 8,952 | 9,112 | +160 bytes, +1.79% |
| Demo ELF | 8,024 | 10,608 | +2,584 bytes, +32.20% |
| BIOS image | 1,474,560 | 1,474,560 | 0 bytes, 0% |
| Initrd | 10,240 | 10,240 | 0 bytes, 0% |

Four boot-mode closes per tree had median elapsed times of 15.036 and 15.279 ms.
The difference is +0.242 ms, or +1.61%.
The eight alternate-mode closes took 15.459 to 15.840 ms.
This includes register restoration and text redraw. There is no alternate-mode baseline.

The final QEMU test measured two pointer events, one at each alternate resolution.
The event ages were 2.461 and 3.775 ms. Cursor draw durations were 0.327 and 0.304 ms.
There is no equivalent event measurement from the polling demo.
The two samples cannot establish a latency trend or reproduce continuous motion.
See `build/artifacts/test-runs/os1_smoke_display_session_uefi-v92tnd9f/guest.display.log`.
The laptop pause, maximum geometry restoration cost, and IRQ CPU cost remain unmeasured.
