# GM206 copy engine

This document describes the copy engine path of the GTX 950 display module.
The module has this path, and host tests check it. The [first desktop test](#first-desktop-test) on the GTX 950 passed.

```text
gsh copy request -> display service -> accelerate export of the GM206 module
accelerate -> command ring -> channel scheduler -> copy engine (class B0B5) -> video memory
completion word -> module poll -> true: the kernel updates only the shadow
no completion -> module stops the channel -> false: the drawing module copies with the processor
```

| Topic | Link |
| --- | --- |
| What the module does and does not do | [Scope](#scope) |
| Start sequence and its checks | [Start sequence](#start-sequence) |
| Video memory of the channel | [Channel memory](#channel-memory) |
| One copy request | [Copy request](#copy-request) |
| Facts that only the card can confirm | [Open hardware questions](#open-hardware-questions) |
| Procedure and log lines for the desktop | [Desktop test](#desktop-test) |
| Result on the GTX 950 | [First desktop test](#first-desktop-test) |
| Effects, measurements, and checks | [Impact and validation](#impact-and-validation) |

## Scope

The copy engine moves an area of the native surface in video memory. Window moves and scrolls use it.
The engine needs no firmware file. The 2D engine of the card needs signed firmware, and the module does not use it.
The module polls. It uses no interrupt.
All device code is in the [GM206 module](../../src/drivers/gm206/copy.cpp). The kernel has only the generic `accelerate` call.
See the [display driver interface](driver-module-abi.md#driver-table) for that call.

| Item | State |
| --- | --- |
| Copy of an area, also with an overlap | Implemented. |
| Fill of an area | Not implemented. The processor fill reads no video memory and is fast. |
| Bitmap and legacy fill requests | The processor draws them. |
| Console surface after a restore | The processor draws it. |
| An area smaller than 1,024 pixels | The processor draws it. A device request costs more than this copy. |

The references are Nouveau in Linux 6.1 and 6.6, the NVIDIA open kernel modules 550.90.07, and envytools.
The module has no code of these projects. It uses the register sequence and the class methods that they document.

## Start sequence

The module starts the engine one time, after the first successful mode change.
A failure at each step leaves the engine out of use. The display and the processor drawing continue.

| Step | Action | Stage name on failure |
| --- | --- | --- |
| 1 | Read the device table at `0x22700`. Select the last copy engine that is not on the graphics run list. | `Topology` |
| 2 | Refuse a BAR 1 with translation (`0x1704` bit 31). | `Topology` |
| 3 | Disable the card interrupts. Set the scheduler bit and the engine bit in `0x200` if they are clear. | `Scheduler` |
| 4 | Find the command reader of the run list. Refuse a channel 0 that has an owner. | `Scheduler` |
| 5 | Write the page tables, the instance block, and the run list to video memory. Read two words back. | `Memory` |
| 6 | Flush the translation cache for the new page directory. | `Translation` |
| 7 | Bind channel 0, enable it, and commit the run list. | `Channel` |
| 8 | Wait for the completion word of the first commands. | `Submit` |
| 9 | Send commands without a transfer, to find the kick method. | `Kick` |
| 10 | Copy a pattern in scratch memory and compare it. Read the error records. | `Launch` |

The module writes no value to `0x1700`, `0x1704`, `0x1714`, and `0x100c80`.
The module does not reset the scheduler. Nouveau resets it, but that reset has no test with an active display here.

### Kick methods

A kick tells the scheduler that the command ring has a new entry.
The reference drivers write the ring position to the user area of the channel through BAR 1. The scheduler monitors that write.
The firmware of this board leaves BAR 1 without translation. No reference driver uses the scheduler in that state.
Thus the start tries three methods in sequence and keeps the first that operates.

| Method | Action | Source |
| --- | --- | --- |
| 1 | Write the ring position to the user area through BAR 1. | Nouveau and NVIDIA `nvgpu`. |
| 2 | Method 1, then disable and enable the channel and commit the run list. | Inferred. |
| 3 | Method 1, then write the ring position to the command reader. | Inferred. |

### Address types

The test copy first uses channel addresses with the page tables, as Nouveau does.
The page tables map the native surface and the scratch memory.
The copy can complete with a wrong pattern or with an error record. The module then clears the error records.
It then tries video memory addresses without translation. The NVIDIA open kernel modules use this type on this card family.
A test copy without a completion ends the start. Its ring entry stays in use, thus a second attempt is not safe.
The test copy has a source pitch that is different from the target pitch, as an overlapping request has.

## Channel memory

The channel memory is 1 MiB of video memory at offset `0x06420000`. The module reaches it through BAR 1.

| Offset from `0x06420000` | Bytes | Content | Reader |
| --- | --- | --- | --- |
| `0x00000` | 4 KiB | Instance block: channel state and page directory address | Scheduler |
| `0x01000` | 4 KiB | User area: ring position at `0x8c` | Scheduler |
| `0x02000`, `0x03000` | 4 KiB each | Run list with the channel, and an empty run list | Scheduler |
| `0x04000` | 4 KiB | Command ring, 512 entries | Channel address `0x00100000` |
| `0x05000` | 4 KiB | Commands | Channel address `0x00101000` |
| `0x06000` | 4 KiB | Completion word | Channel address `0x00102000` |
| `0x20000` | 128 KiB | Page directory, one entry in use | Translation unit |
| `0x40000` | 256 KiB | Table of 4 KiB pages | Translation unit |
| `0x80000` | 512 KiB | Scratch memory | Channel address `0x00200000` |

The native surface has the channel address `0x02000000`.
Each channel address is below 64 MiB and uses a 4 KiB page.
The card has two page layouts. With this rule, the two layouts use the same directory entry and the same table index.
Thus the module does not read or change the layout selection.

## Copy request

```text
set object B0B5 (first commands only)
physical mode: video memory for source and target
source, target, pitch in, pitch out, bytes in a line, lines
launch: no pipeline, pitch layout, more than one line, address type
completion address, completion value
launch: release one word, with a cache flush
```

The module writes the commands, adds one ring entry, and does the kick. It then polls the completion word.
After the completion, the module reads the error records of the scheduler, the command reader, and the engine.
A request waits a maximum of approximately 50 ms for the completion.
With kick method 2, a request also waits for the run list commit, a maximum of 200 ms.
The start waits a maximum of 100 ms for each completion and 200 ms for each register state.
The source of the reference drivers does not give the copy order of the engine.
Thus an overlapping copy has two transfers: source to scratch memory, then scratch memory to target.
The area limit of a session request is 65,536 pixels, which is 256 KiB. The scratch memory holds one area.

If a request gets no completion or has an error record, the module records the state and disables the channel.
The `accelerate` export then returns false, and the drawing module copies the area from the shadow.
The image is correct after this, because the shadow has the pixels from before the request.
The request path runs with interrupts disabled. Thus it does not wait for the scheduler after a failure.
The request path also asks the scheduler to take the channel off the engine, without a wait.
The next mode change or console restore removes the channel from the scheduler. This step can wait 400 ms.
The module does not start the channel again before the next boot.
After a successful start, the module writes the use counts to the log at each later mode change and each console restore.
It also writes them when the copy count is a power of two, and after a failed request.
A desktop session that keeps its mode at the exit does not restore the console. Thus the exit of plain `gsh` writes no line.

## Open hardware questions

| Question | Effect if the assumption is wrong | Log evidence |
| --- | --- | --- |
| Does the device table have a copy engine with its own run list? | The engine is not in use. | `gm206 copy devices`, `gm206 copy engine` |
| Does the scheduler see a kick with BAR 1 without translation? | Methods 2 and 3 run. If none operates, the engine is not in use. | `gm206 copy state` with step `0x10` to `0x12` and `0x101`, `0x102` |
| Does the engine accept channel addresses for a copy? | The module tries addresses without translation. | `gm206 copy pattern`, `gm206 copy state` with step `0x20` |
| Does a command error stop the completion word? | The module finds the error record after the completion. | `gm206 copy state` with step `0x30` |
| Does the processor see the completion word at once? | The start fails at `Submit`. | Completion word in `gm206 copy state` |
| Does the scheduler operate without a reset after firmware? | The start fails at `Channel` or `Submit`. | `0x2100` and `0x252c` in `gm206 copy state` |
| Does scheduler record bit 29 follow only real errors of the command readers? | Each test copy fails, and the engine is not in use. | `0x2100` in `gm206 copy state` with steps `0x1f`, `0x20`, `0x21` |
| Does the display continue while the scheduler starts? | The screen shows a fault after the first mode change. | The user sees it. |

## Desktop test

Use a managed USB stick with **Update boot and programs (preserve data)**.

1. Boot `os1` to the text shell.
2. Start `gsh --menu`.
3. Select the 3840 by 2160 mode with the arrow keys.
4. Press Enter. The copy engine starts during this mode change.
5. Make sure that the desktop and the pointer show.
6. Press T to open the shell window.
7. Enter `ls` again and again until the window text moves up.
8. Move the shell window with its title across the screen for some seconds.
9. Make sure that the window content and the background are correct after each move.
10. Press Esc to close the shell window.
11. Press M.
12. Select a smaller mode with the arrow keys.
13. Press Enter.
14. Press Enter again in less than 10 seconds to keep the mode.
15. Press T.
16. Move the shell window again.
17. Press Esc to close the shell window.
18. Press Q to exit. The exit restores the console and writes a `gm206 copy use` line.
19. Export the log with `lx`.

| Log line | Values, each as a hexadecimal number |
| --- | --- |
| `gm206 copy devices` | Eight rows of the device table. The log has eight such lines. |
| `gm206 copy engine` | `0x200`, engine found, run list, engine id, reset bit, fault id, engine instance, `0x2004`, `0x2008`, `0x1704`, `0x100c80`, `0x140`, `0x2100`, `0x2254` |
| `gm206 copy scheduler` | `0x200` after the enable, reader count, reader, `0x204`, channel 0 instance and control |
| `gm206 copy state` | Step, stage, `0x2100`, `0x252c`, `0x254c`, `0x259c`, `0x25a0`, engine status, channel instance, channel control, reader status, run list status, ring get, ring put, completion word, wanted value |
| `gm206 copy reader` | Step, reader interrupt records `0x108` and `0x148`, method and data, channel, reader put and get, user area, ring address, engine records `0x908` and `0xf14`, four fault words |
| `gm206 copy pattern` | Attempt, index of the first wrong pixel, wanted value, value in video memory. Index `0x100` is a correct pattern with an error record. The two values are then those of pixel 0. |
| `gm206 copy ready` | Kick method, address type (1 is without translation), run list, reader, `0x2100`, `0x200`, sum of completion polls of the start, most polls for one step of the start |
| `gm206 copy stop` | Channel idle, run list commit complete, engine status, channel control |
| `gm206 copy use` | Stage, copies, sum of completion polls of the requests, most polls for one request, kick method, address type |

A `gm206 copy ready` line means that the engine is in use. A log without that line gives the failed step.
A failure in steps 1 to 4 of the start writes no `gm206 copy state` line. The `gm206 copy engine` and `gm206 copy scheduler` lines show the cause.
One completion poll takes 10 microseconds or more. Stage 9 in `gm206 copy use` is `Ready`, and stage 10 is `Stopped`.

| Step code in `gm206 copy state` | Meaning |
| --- | --- |
| 1 | Video memory did not hold the channel data. |
| 2, 3 | The translation flush did not start, or did not end. |
| 4, 5 | The run list commit did not end, or the scheduler has a bind error. |
| 6 | No kick method ran the first commands. |
| 7 | No kick method ran the second commands. |
| 8 | The test copy had no completion. |
| 9 | The test copy failed with the two address types. |
| `0x10` to `0x12` | State before kick methods 1 to 3 for the first commands. |
| `0x1f` | An error record from the steps before the test copy. The module clears it and continues. |
| `0x101`, `0x102` | State before kick methods 2 and 3 for the second commands. |
| `0x20`, `0x21` | A wrong pattern or an error record with channel addresses, or with video memory addresses. |
| `0x30` | A failed request after the start. |

## First desktop test

The test ran on November 14, 2026 on the GTX 950 with the uncommitted tree on commit `0557985`.
The module file had 43,504 bytes. The log is in the `log` directory of the stick copy of that day.
The repository does not hold the log. The user gave no report of a wrong image.

| Item | Value in the log | Meaning |
| --- | --- | --- |
| `gm206 copy engine` | `0x200` = `0xc0012121`, run list 5, engine 5, reset bit 7, fault id `0x16`, instance 1 | The device table has a copy engine with its own run list. The scheduler bit was set. The engine bit was clear. |
| `gm206 copy engine` | `0x1704` = 0, `0x100c80` = `0x208001`, `0x2004` = 3, `0x2008` = `0x1000` | BAR 1 has no translation. The card has three command readers. |
| `gm206 copy scheduler` | `0x200` = `0xc00121a1`, reader 1, channel 0 free | The module set only the engine bit. |
| `gm206 copy ready` | Kick method 1, address type 0, `0x2100` = 0, 3 start polls, 1 poll at most | The scheduler sees the write to the user area through BAR 1. Channel addresses operate. |
| `gm206 copy use` at the first console restore | Stage 9, `0x7707` copies, `0x4e599` polls, `0x1a` polls at most | 30,471 copies with a mean of 10.5 polls for each copy. |
| `gm206 copy use` at the last console restore | Stage 9, `0x98bb` copies, `0x5db34` polls | 39,099 copies in two sessions. No request failed. |

The log has no `gm206 copy state`, `gm206 copy pattern`, or `gm206 copy stop` line.
One poll has a delay of 10 microseconds or more. Thus one copy request waited approximately 105 microseconds or more.
The log has the use lines for 1 to 64 copies and for each console restore. It has no use line for 128 to 16,384 copies.
The cause of the absent lines is not known. The log writer can refuse a line while a different processor holds its lock.
The test gives no time comparison with the processor copy.

| Open hardware question | Answer of the test |
| --- | --- |
| Copy engine with its own run list | Yes: copy engine instance 1 on run list 5. |
| Kick with BAR 1 without translation | Method 1 operates. |
| Channel addresses for a copy | They operate. The module did not try addresses without translation. |
| Completion word visible to the processor | Yes. |
| Scheduler without a reset after firmware | It operates. |
| Display during the scheduler start | The session continued. |
| Error record after a completion | No error record occurred. This case has no hardware evidence. |

## Impact and validation

The change follows the [display driver plan](nvidia-mode-setting-plan.md) and the [graphics shell plan](gsh-plan.md#later-stages).

| Area | Effect |
| --- | --- |
| Project scope | Device code stays in the module. The kernel adds one generic optional export. A machine without the card does not load the module. |
| Boot and platform | No change of the boot paths. The module maps four more BAR 0 ranges, 8 KiB more of the first range, and approximately 1 MiB more of BAR 1. |
| Memory and isolation | The module checks each area against the surface and the limit. The page tables map only the native surface and the channel memory. |
| Concurrency | The kernel calls `accelerate` with the console output lock and with interrupts disabled. A failed request waits approximately 50 ms, one time. It then writes three log lines and disables the channel. With kick method 2, each request can also wait 200 ms for the run list commit. The start runs in the mode change. The pointer code uses a different memory window. |
| Interfaces | Display driver interface ABI 6 adds `accelerate`. The driver package dependency is `kernel.display_driver.v6`. Display v1 and DisplaySession v1 do not change. |
| Runtime resources | See the table below. Copy speed on the card is unmeasured. |
| Validation and documents | Fourteen host tests use a model of the scheduler and the engine. QEMU has no such card. |

| Item | Commit `1004725` | This tree | Difference |
| --- | --- | --- | --- |
| GM206 module | 37,472 bytes | 43,504 bytes | +6,032 bytes (+16.1%) |
| Kernel ELF | 660,768 bytes | 663,408 bytes | +2,640 bytes (+0.40%) |
| BIOS kernel ELF | 488,520 bytes | 490,824 bytes | +2,304 bytes (+0.47%) |

The kernel difference includes the byte stream of commit `0557985`. That commit has no separate size record.
The toolchain and the build settings are those of the local `build` directory on macOS.

The host tests are in `tests/host/kernel/gm206_copy_tests.cpp`. The model reads the instance block, the page tables, the ring, and the commands from memory.
The model copies forward, a byte at a time. Thus a direct overlapping copy gives a wrong image in a test.
The model checks each register access against the mapped ranges. It also checks the channel state words and the translation flush.
The tests cover the start, each kick method, the two address types, each overlap direction, the limits, and the ring wrap.
They also cover a start failure at four stages, a request without a completion, and a completion with an error record.
No host test covers the kernel call of `accelerate`, the shadow update, or a copy through the driver table of the module. The QEMU checks do not reach that call.
These focused checks passed on November 14, 2026: 58 host tests for the module, the draw code, and the stream ring.
The `os1_program_packaging_contract`, `os1_smoke_spawn`, `os1_smoke_display_session`, and `os1_smoke_no_display` checks passed on BIOS and UEFI where they have the two variants.
The full local suite did not run on this tree. Run `./scripts/check-local.sh` before the push.
A host test or a QEMU check is not proof of the behavior of the card. The [first desktop test](#first-desktop-test) gives the hardware evidence.
