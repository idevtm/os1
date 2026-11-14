# Laptop display plan

This plan adds the display adapter of the Asus G74SX laptop to the existing GM206 display module.
The result is one module file that drives the desktop GTX 950 and the laptop GF116M.
This document is a plan. The tree contains no code for it, and the laptop has run no test.

```text
one package, two PCI identities -> one module file
module probe -> BOOT0 chip number -> 0x126: Maxwell profile (implemented)
                                  -> 0x0cf: Fermi profile (this plan)
shared code:  EDID, video memory layout, aperture check, page tables, driver table
profile code: boot state, mode list, memory size, display engine registers, core channel methods,
              supervisor, pointer, copy path
```

| Topic | Link |
| --- | --- |
| The two adapters | [Adapters](#adapters) |
| Decisions that the user must make | [Decisions](#decisions) |
| What stays and what changes in the module | [Module structure](#module-structure) |
| Register differences | [Display engine differences](#display-engine-differences) |
| Order of work and laptop tests | [Stages](#stages) |
| Facts that no source confirms | [Open questions](#open-questions) |
| Risk to the laptop panel | [Failure behavior](#failure-behavior) |
| Assessment and checks | [Impact assessment](#impact-assessment), [Validation](#validation) |

## Adapters

The Linux captures of November 13, 2026 give the laptop values.
See the [physical NVIDIA mode-setting plan](nvidia-mode-setting-plan.md#reference-machines) for those captures.

| Property | Desktop (implemented) | Laptop (this plan) |
| --- | --- | --- |
| Adapter | GM206, GTX 950, `10de:1402` | GF116M, `10de:1251`, revision `a1` |
| Subsystem | `1458:36c5` | `1043:204a` |
| Nouveau chip number | `0x126` | `0x0cf` (from the Nouveau device table; the laptop must confirm it) |
| Output in use | DisplayPort, 3840 × 2160 | LVDS panel, 1920 × 1080 at 60.02 Hz |
| Other connectors | HDMI, DVI | HDMI and VGA, both disconnected in the capture |
| BAR 0, BAR 1, BAR 3 | 16 MiB registers, 256 MiB aperture | 32 MiB, 128 MiB, 64 MiB |
| Display engine | Nouveau `gm200_disp`, core class `0x957d` | Nouveau `gt215_disp`, core class `0x857d` |
| Channel scheduler | Nouveau `gm200_fifo`, channel class `0xb06f` | Nouveau `gf100_fifo`, channel class `0x906f` |
| Copy engine | Class `0xb0b5`, no microcode | Class `0x90b5`, a processor that needs microcode |

The laptop capture has no Nouveau kernel log lines. Thus the chip number and the video memory size have no laptop evidence.
The preferred EDID mode of the panel has a pixel clock of 138.70 MHz.

## Decisions

The plan needs these decisions before the related stage starts.

| Decision | Recommendation | Stage |
| --- | --- | --- |
| Module name. The module, its package, and the `os.display.gm206` tree have the desktop chip name. | Keep the names in stages 1 to 4. Rename them in one separate change after the laptop works. | After 4 |
| Microcode for the laptop copy engine. Nouveau has open microcode with an MIT license, 2088 bytes. | Add it to `third_party/` and build it into the module only if stage 6 starts. | 6 |
| Order of the copy path. The laptop surface has one quarter of the desktop pixels. | Measure the processor copy on the laptop first. Start stage 6 only if the measurement shows a need. | 6 |
| Own timings, HDMI, and VGA. They need the VBIOS scripts and the clock registers. | Keep them out of this plan. | None |

## Module structure

The module selects a profile from bits 28:20 of register `0x000000`, as it does now for `0x126`.
A profile is a table of constants and functions in the module. The kernel does not see it.

| Part | State | Change |
| --- | --- | --- |
| [Package table](driver-module-abi.md#display-package-table) (`driver_package.py`, `display_module_table.py`, kernel loader) | One PCI device for each package | Accept a list of PCI devices for one package. This is generic kernel code. The packaging test changes with it. |
| PCI match and subsystem check (`display.cpp`) | `10de:1402`, subsystem `1458:36c5` | A table of supported boards. Each entry names its profile. |
| EDID parser and head timing (`driver.cpp`) | Shared | No change. The laptop does not read EDID in stages 1 to 4. |
| Mode list (`offer_modes`) | It needs the DisplayPort route of the desktop board, the Maxwell viewport value, and EDID modes | The profile gives the native size and the scaled sizes. See [mode list conditions](#mode-list-conditions). |
| Video memory size (`memory_bytes`) | Maxwell registers | Fermi registers `0x022438`, `0x022554`, and `0x11020c`. |
| Video memory layout and aperture check | Fixed offsets below `0x06520000` | No change. BAR 1 of the laptop has 128 MiB. |
| Boot state capture (`probe`) | Maxwell registers | A Fermi register list. See [stage 1](#stage-1-read-only-capture). |
| Scanout (`take_display`, `service`, `run_commands`, `confirm`) | Maxwell registers and methods | Move register addresses and method numbers to the profile. Add the Fermi sequence. |
| Hardware pointer (`cursor`) | Maxwell position channel | A Fermi position channel. |
| Copy path (`copy.cpp`) | Maxwell only | The Fermi profile reports no copy path until stage 6. The `accelerate` export returns false. |
| Reflection (`reflection.cpp`) | Maxwell snapshot fields | Fermi fields in the same subtree. |
| Display driver interface, ABI 6 | Implemented | No change. |

The kernel gets no device code. The only kernel change is the PCI device list of a package.
A machine with no supported adapter loads no module, as now.

## Display engine differences

Linux v6.6 Nouveau is the reference. The table names the source file for each row.
The paths are relative to `drivers/gpu/drm/nouveau/`.

| Item | Maxwell profile | Fermi profile | Nouveau source |
| --- | --- | --- | --- |
| Firmware ownership | `0x6100ac` bit 8 | `0x610024` bit 8; write `0x100`, then clear bit 0 of `0x6194e8` and wait for bit 1 | `nvkm/engine/disp/nv50.c` |
| Capability copy | `0x6101b4`, `0x6101c0`, `0x6301c4` | `0x614004` to `0x610184`; head, DAC, SOR, and PIOR values to `0x610190` and up | `nv50.c` `nv50_disp_init` |
| Instance block | `0x610010` | `0x610010`, same value format | `nv50.c`, `gf119.c` |
| Request enable | `0x000140 = 0`, `0x6100b0 = 0x307` | `0x000140 = 0`, `0x61002c = 0x370`, `0x610028 = 0` | `nv50.c` `nv50_disp_init` |
| Core channel control | `0x610490` to `0x61049c` | `0x610200` to `0x61020c`; two stuck states need a release first | `nv50.c` `nv50_disp_core_init` |
| Command pointers | `0x640000`, `0x640004` | Same | `cl507c.h` |
| Hash table context | `(offset << 9) \| (channel << 27) \| 1` | `(offset << 10) \| (channel << 28) \| channel` | `nv50_disp_dmac_bind`, `gf119_disp_dmac_bind` |
| Memory object | GF119 format | GF100 format, 24 bytes | `nvkm/engine/dma/usergf100.c` |
| Head methods | `0x0400` plus `0x300` for each head | `0x0800` plus `0x400` for each head | `cl907d.h`, `cl507d.h`, `cl827d.h` |
| SOR control method | `0x0200` plus `0x20` for each SOR | `0x0600` plus `0x40` for each SOR; bits 19:16 hold a pixel depth | Same files |
| Surface methods | `0x0460` to `0x0474` | `0x0860`, `0x0868`, `0x086c`, `0x0870`, `0x0874` | `head827d.c` |
| Viewport methods | `0x04b0` to `0x04c8` | `0x08c0`, `0x08c8`, `0x08d8`, `0x08dc` | `head507d.c` |
| Active state readback | `0x640000` plus the method | Head table `0x610a00` to `0x610b50` and `0x610c50` to `0x610c78`, plus `0x540` for each head. SOR control at `0x610794` plus 8 for each SOR. The active value is 4 bytes after the pending value. | `g84.c`, `g94.c` |
| Supervisor request | `0x6100ac`, `0x6101d4`, `0x6101d0` | `0x610024` bits 4 to 6, `0x610030`; write `0x80000000` to complete a stage | `nv50.c` `nv50_disp_super` |
| Error words | `0x61009c` bit 0, three words at `0x6101f0` | `0x610020` bits 16 to 20, `0x610080` and `0x610084` plus 8 for each channel | `nv50_disp_intr_error` |
| Heads | 4 | 2 | `nv50_head_cnt` |
| Backlight | None | `0x61c080` and `0x61c084` plus `0x800` for each SOR | `nouveau_backlight.c` |

The address translation tables, the flush registers `0x100cb8` and `0x100cbc`, and the registers `0x1700`, `0x1704`, and `0x1714` have the same format on both chips.
Nouveau sets the LVDS link count and the panel pixel depth from driver state in supervisor stage 2, not from a core method.
The plan keeps the firmware timing, as the desktop does. Thus the module runs no VBIOS script and writes no clock register.
The plan assumes that the firmware values for the link count and the depth then stay active. Stage 3 tests this assumption.

## Stages

Each stage ends with one laptop run. The user copies the stick with `./os1.sh usb_capture` after each run.

```text
1 match and capture -> 2 host model -> 3 surface and console -> 4 sizes and pointer -> 5 backlight report
                                                                                 -> 6 copy path (optional)
```

### Stage 1: read-only capture

The purpose is to confirm the chip and the firmware state before any register write.

1. Add `10de:1251` to the package and the board table.
2. Add the Fermi profile with a probe that reads registers only. The profile offers no mode in this stage.
3. Show the values in the reflection subtree and in the `run-tests` capture.

The probe reads these values:

| Group | Registers |
| --- | --- |
| Chip | `0x000000`, `0x022500` (engine straps), `0x02240c` (firmware initialization) |
| Ownership | `0x610024`, `0x610030`, `0x61002c`, `0x610010`, the core channel registers `0x610200` to `0x61020c`, and the base channel control `0x610210` |
| Outputs | `0x610184`, `0x610794` to `0x6107b0` (SOR control, pending and active) |
| Head 0 and head 1 | `0x610ad0` to `0x610b00` (clock and raster), and the viewport and surface entries of the same table, pending and active |
| Boot framebuffer | Address, size, pitch, and pixel layout from the boot information |
| Memory | `0x001700`, `0x001704`, `0x001714`, `0x619f04`, `0x022438`, `0x022554`, `0x11020c` for each partition |
| Backlight | `0x61c080` and `0x61c084` for each SOR |
| Copy engine | `0x10412c` (processor version), `0x104108` (code and data limits) |

Acceptance: the capture shows chip `0x0cf`, firmware ownership, one active SOR with the LVDS protocol, and a 1920 × 1080 raster.
Bit 31 of `0x001704` must be clear. The fixed video memory layout needs BAR 1 without address translation.
The capture must also show that the boot framebuffer is in a BAR of the card.
Make one capture with BIOS start and one with UEFI start, if the laptop has both.
If one value is different, correct the plan before stage 3.

#### Mode list conditions

The module offers a native mode only if the conditions of `offer_modes` are true. The stage 1 capture must show each one.

- The boot framebuffer has 32-bit pixels with red at bit 16, and a pitch that is a multiple of 256 bytes.
- The native width and height are larger than the boot framebuffer. The boot configuration requests 1024 × 768.
- The native width is a multiple of 64.
- The boot framebuffer is not larger than the 4 MiB console surface range of the video memory layout.

The desktop takes the native size from the output viewport of the head.
On the laptop, firmware can show the 4:3 boot framebuffer in a 1440 × 1080 viewport. The width 1440 is not a multiple of 64.
Firmware can also give a 1920 × 1080 boot framebuffer, which is not smaller than the native size.
Such a framebuffer needs 7.9 MiB and does not go into the console surface range.
If the capture shows one of these cases, change the plan before stage 3.
For the first case, the candidate rule takes the native size from the active raster and sends a full viewport.
The second case needs a different video memory layout or a smaller console surface. The user must make that decision.

### Stage 2: host model

Add a host test model of the Fermi display engine. The `Card` model of `gm206_copy_tests.cpp` is the example.
The model checks each register access against the mapped ranges and refuses a method outside the tables above.
Move the Maxwell register addresses to its profile in the same change. The desktop register sequence must not change.
Acceptance: the existing GM206 host tests pass with no change to their expected register order.

### Stage 3: surface and console

The module shows its surface with the firmware timing, and then the console surface. This is the [desktop takeover sequence](nvidia-mode-setting-plan.md#takeover-sequence) with Fermi registers.

1. Read the active head and SOR state before the first register write.
2. Check BAR 1 through PRAMIN, as the desktop does.
3. Write the hash table and the GF100 memory object for all video memory.
4. Disable the interrupt line.
5. Copy the capabilities.
6. Take the display from firmware.
7. Write the instance block address to `0x610010`, then the request enable values.
8. Start the core channel with the command page in video memory.
9. Clear bit 25 (`NOT_DRIVER`) in the firmware value of the pixel clock method.
10. Send the firmware state methods that differ from the pending state.
11. Send the surface, viewport, and UPDATE methods.
12. Acknowledge each supervisor stage. Do not change a clock, a link, or a panel power register.
13. Wait for the active surface offset in the readback table.

The stage also adds the console restore with the console surface and the head scaler. The `restore` export is mandatory.
The desktop engine refused bit 24 (`NOT_DRIVER`) of its clock configuration method `0x0454`. Step 9 assumes the same behavior for bit 25 on this chip.
The mode list has only the native size in this stage. See the [mode list conditions](#mode-list-conditions).
`gsh "--check"` is the first laptop test. It shows the mode surface and then the console surface, four times.
Acceptance: the panel shows the test surface at 1920 × 1080 and then the console. Each `scanout` log line has stage 8.

### Stage 4: sizes and pointer

1. Offer scaled sizes from a table in the profile: 1600 × 900, 1280 × 720, and 1024 × 576.
2. Add the hardware pointer through the position channel of the head.
3. Run `gsh` with the USB mouse and the touchpad.

Read the Nouveau pointer files (`curs507a.c`, `head827d.c`) before step 2. The research for this plan did not cover the position channel registers.
Acceptance: the [drawing acceptance procedure](nvidia-mode-setting-plan.md#drawing-acceptance-procedure) passes on the laptop with its mode list.

### Stage 5: backlight report

Report the two backlight registers in the reflection subtree. Do not write them.
A later plan can add a brightness request if the registers show a nonzero divider.

### Stage 6: copy path (optional)

This stage needs the decisions about microcode and order. It is larger than the display stages.

| Part | Difference from the desktop copy path |
| --- | --- |
| Channel scheduler | One run list at `0x002270` and `0x002274`. The channel table is at `0x003000`. The user area is in the BAR 1 address space (`0x002254`). |
| Channel block | Different RAMFC words. The copy engine context pointer is at offset `0x230`. |
| Copy engine start | Load 384 code words and 138 data words, then start the processor at `0x104000`. |
| Context | The channel needs a 256-byte context object for the copy engine. |
| Methods | Addresses `0x030c` to `0x0318`, pitch `0x031c` and `0x0320`, counts `0x0324` and `0x0328`, start `0x0300 = 0x110`. One start copies a maximum of 8191 lines. |

The user area needs an address space for BAR 1, and thus a BAR 1 instance in `0x001704`.
The display stages need BAR 1 without address translation. This is a conflict.
Stage 6 must map all reserved video memory at the same addresses in that address space before it sets `0x001704`.
The mapping must also include the firmware framebuffer while the console uses it.
The page table format and the completion check through a semaphore word can stay shared if the laptop accepts them.

## Open questions

No source that the research read confirms these items. Stage 1 or stage 3 must answer each one.

| Question | Answered by |
| --- | --- |
| Is the chip number `0x0cf`, and is the display engine present (`0x022500` bit 0 clear)? | Stage 1 |
| Does firmware show the panel through an NVIDIA SOR? The [Linux capture](reference-captures/laptop/linux/drm-info.txt) shows the LVDS connector on `10de:1251`. | Stage 1 (active SOR) |
| Is BAR 1 without address translation after firmware start (`0x001704` bit 31 clear)? | Stage 1 |
| Which boot framebuffer size and which output viewport does firmware give? | Stage 1 |
| Does the boot framebuffer of BIOS and of UEFI start lie in BAR 1? | Stage 1 |
| Can the core channel alone show a surface? Linux v6.6 shows the image through the base channel `0x857c`. | Stage 3 |
| Does the engine accept an UPDATE with firmware timing values and keep the LVDS link? The desktop showed this for DisplayPort only. | Stage 3 |
| Do the supervisor bits of `0x610024` show a request while the module polls, with which value of `0x61002c`? | Stage 3 |
| Does a surface change with the same timing make a supervisor request? | Stage 3 |
| Does firmware use the reserved video memory range below `0x06520000`? | Stage 3 |
| Does the backlight use the SOR registers or the embedded controller? | Stage 5 |
| Which rule connects a subchannel to the copy engine on channel class `0x906f`? | Stage 6 |

If the core channel does not show a surface, start the base channel `0x857c` for the image. Nouveau gives its sequence.
That method adds a second channel with control `0x610210`, a user area at `0x641000`, and a notifier object. It is a stage of its own.
Do not continue to stage 4 until one of the two methods passes.

## Failure behavior

The panel is the only display of the laptop, and the laptop has no serial port.
A failure after the ownership change can leave a dark panel.

- Each stage writes its log lines to the kernel log on the USB stick before and after the display change.
- A failure before the ownership change keeps the firmware console, as on the desktop.
- After a later failure, the display service refuses each new session until the next boot.
- The test procedure of stage 3 uses `gsh "--check"`, which ends by itself. The user then stops the laptop with the power key.
- The module does not write the SOR power, panel sequence, clock, or backlight registers in stages 1 to 5.

The laptop firmware initializes the adapter again after a power cycle. This is the recovery from a dark panel.
The plan expects no permanent effect from these stages, but no laptop test confirms this.

## Impact assessment

Affected requirements: [GOALS](../../GOALS.md) "Driver model", the [driver architecture](driver-architecture.md#hardware-priorities) priority 3, the [display driver interface](driver-module-abi.md#display-driver-interface), and the [graphics shell plan](gsh-plan.md#later-stages).

| Area | Assessment |
| --- | --- |
| Project scope | One module with two profiles shows the differences between two chip families. The kernel gets no device code. |
| Boot and platform | The loader loads the module for a second PCI device. BIOS and UEFI boot of the laptop need a stage 1 capture each. Headless operation does not change. |
| Memory and isolation | The module maps the same BAR 0 ranges plus `0x614000` for the capability source in stages 1 to 3. The position channel of stage 4 needs `0x647000` and `0x648000`. It uses the same fixed video memory offsets. The module stays trusted. |
| Concurrency | No change. The session that owns the display makes each call, and the module polls with the interrupt line off. |
| Interfaces | Display driver ABI 6 and DisplaySession do not change. The package table format gets a device list. The reflection subtree gets Fermi fields. |
| Runtime resources | The module file grows. The kernel grows by the device list code only. Measure both sizes and the BIOS kernel limit at each stage. |
| Validation and documents | Host tests use a register model. QEMU has no such adapter. Only the laptop can qualify each stage. |

This change is a document. It has no runtime measurement.

## Validation

For each code stage: run the affected host tests, then `./scripts/check-local.sh` on the final tree, then one laptop run.
Run the desktop [drawing acceptance procedure](nvidia-mode-setting-plan.md#drawing-acceptance-procedure) one time after stage 2, because that stage moves desktop code.
Record each laptop run in this document with the capture path, the log lines, and the result.

Limits of this plan:

- The register facts come from one Nouveau revision, Linux v6.6. The `envytools` register database was not read.
- One research pass collected the facts. No second source checked each register value.
- The plan has no laptop evidence other than the Linux captures of November 13, 2026.
