# Graphics shell

The graphics shell is an independent user program. Init can start it from disk.
The first stage uses a driver-selected mode and draws a desktop with a clock and pointer.

```text
init -> graphical boot profile -> gsh -> native display session
                               -> sh if graphics are unavailable

gsh -> input subscription, wall clock, display
    -> terminal streams -> child sh (stage 2)
```

## First stage

- Use a versioned preference file at `/etc/gsh.conf`.
- Enable direct startup on normal boots with `/etc/gsh.boot`. Diagnostic boots start `sh`.
- Select a supported mode by width and height. Use the first driver mode if the preference is invalid.
- Start the text shell if the graphical program cannot start before it commits the initial display frame.
- After native takeover, leave the boot framebuffer untouched. Provide reboot and shutdown controls.
- Retain the display and EHCI stall checks as explicit diagnostic commands in `gsh`.
- Use 32 bits per pixel for the first backend. Qualify other pixel formats separately.

QEMU Standard VGA provides the first runtime backend.
The GTX 950 has a [firmware timing scanout](nvidia-mode-setting-plan.md#firmware-timing-scanout) path. The menu session passed its physical acceptance.
The GTX 950 has not started the desktop from `/etc/gsh.boot`. Test that start before regular use.
The laptop needs its own supported backend.

### Configuration

The presence of a regular `/etc/gsh.boot` file enables direct graphical startup.
Remove this file to restore normal text startup.
The file `/etc/gsh.conf` has this format. End each line with a newline.

```ini
version=1
width=1280
height=720
font=0 /share/fonts/spleen-6x12.font
font=1000 /share/fonts/spleen-8x16.font
```

The `width` and `height` lines are the saved mode. The `font` lines are optional [font steps](../shell/fonts.md#graphics-shell-font-steps).
A save of a mode keeps the other lines of the file.

A fresh image installs `gsh` in place of `gfxdemo`.
An update that preserves data keeps existing files outside the deployment manifest.
If the old executable is still present, remove it with `rm "/bin/gfxdemo"`.
The lower right corner shows the UTC date and time. Use R to reboot or S to shut down.
Use M to show the mode list. Use C to change between the hardware pointer and the drawn pointer.
Use T to open the shell window.
The upper right corner shows the size of the current mode.
The clock and the mode size stay under each window. `gsh` does not draw a character that a window covers.
A partly covered character shows its visible part.
The pointer body has the color `0xffcb54` while a button is down. The hardware pointer shows the same color.
`gsh` reads the font file of a [font step](../shell/fonts.md#graphics-shell-font-steps) for the height of the mode. No font has a scale.
The default fonts are 6 × 12, 8 × 16, and 12 × 24 pixels. The disk also has 5 × 8, 16 × 32, and 32 × 64 pixels.
The desktop has no top bar. The key help is text on the background, in the upper left corner.
A window can move across the full screen. The key help stays under each window, as the clock does.
If a pointer disconnects, the clock and keyboard controls remain active.
If `gsh` exits after takeover, the text shell is available over serial.

## Shell window

The T key opens one window with one child shell. The window has the keyboard until Esc closes it.
`gsh` starts `/bin/sh` one time with two [byte streams](../native-api/abi/byte-stream-abi.md).
The child reads the typed lines from the first stream. It writes its prompt and its output to the second stream.

```text
keys -> line edit in gsh -> Enter -> byte stream -> sh (standard input)
sh (both outputs) -> byte stream -> WAIT_MANY in gsh with keys and pointer -> window text
window move -> copy request -> draw the area that the window left
```

`gsh` waits for keys, pointer events, and child output in one wait request. Thus the desktop continues while a command runs.
The child stays in operation while the window is closed and across a mode change. Its directory and its state stay.
A window with a different text size after a mode change starts with empty text.
Ctrl-D closes the input stream. The child then ends, and the next key starts a new child.
A shell that init starts gets the shell init script through its constructor. The script adds the command aliases.
A desktop that init starts has no such shell before it. `gsh` then gives the child shell the same constructor.

| Limit | Cause |
| --- | --- |
| A command that does not end keeps the child busy. | The kernel has no request that stops an ordinary child process. |
| The window has no history, completion, or cursor keys. | `gsh` sends complete lines. The shell uses its line editor only on the console. |
| A full-screen program does not operate in the window. | The window shows printable characters, line feeds, and backspaces only. |
| The child gets the lines only after Enter. | `gsh` does the line edit. |
| Ctrl-D ends the shell also while a command reads its input. | The command and the shell use the same input stream. |
| `gsh` can wait in a write if the child does not read 16,384 bytes of typed lines. | A writer end has no readiness for a wait request. |

Press the left button on the window title and move the mouse to move the window.
The move uses the `copy` request of the [Display session ABI](../native-api/abi/display-abi.md#fast-drawing-requests).
A session without that request draws the window again.

## Mode list

The M key opens a window with the mode list. The boot mode is not in the list of the desktop.

```text
M -> mode list -> Up, Down, or pointer -> Enter or left button -> new session in the new mode
new mode -> question with a 10 second limit -> Enter keeps the mode -> save /etc/gsh.conf (desktop)
                                            -> Esc or no answer -> session in the mode before
```

The list marks the current mode. Enter on the current mode, Esc, or a press outside the list closes the window.
A new mode opens a new display session. The new session starts with a question and a time limit of 10 seconds.
Enter keeps the mode. The desktop then writes the mode size to `/etc/gsh.conf`. The menu session does not write the file.
Esc, or no answer in the time limit, opens a session in the mode before the change.
If a new mode does not open, `gsh` opens the mode before the change.
The shell window does not stay open across a mode change.

## Later stages

1. Give the shell window line editing, history, and completion. Add a request that stops a child process.
2. Add a window size change.
3. Measure the [GPU copy engine path](gm206-copy-engine.md) against the processor copy. The first desktop test passed.
4. Add the laptop display adapter to the display driver modules. See the [laptop display plan](laptop-display-plan.md).
5. Increase the reserved window of the BIOS kernel image when a kernel feature needs the space.
6. Qualify the Spleen fonts on the desktop in each mode size.

The [roadmap](../native-api/roadmap.md#graphics-shell-and-byte-stream-limits) records each open limit with its next step.

The [fifth desktop test](nvidia-mode-setting-plan.md#fifth-desktop-test) and the [sixth desktop test](nvidia-mode-setting-plan.md#sixth-desktop-test) and the [seventh desktop test](nvidia-mode-setting-plan.md#seventh-desktop-test) are the sources of this list.

## Impact and validation

| Area | Effect |
| --- | --- |
| Scope | The optional desktop goal gains an independent frontend. |
| Boot | Init chooses the frontend after storage mounts. BIOS and UEFI retain text startup when native graphics are unavailable. |
| Isolation | The display session owns writes. Userspace supplies bounded rectangles. |
| Concurrency | The console serializes ownership. Input uses subscriptions and the clock uses timed waits. |
| Interfaces | The display service adds native acquisition. The launch profile uses two files. |
| Resources | Measure kernel, initrd, and program sizes. Check clock updates and pointer motion in QEMU. |
| Validation | Check the direct boot and fallback paths. Qualify physical scanout separately. |

### Desktop acceptance

On the GTX 950, verify text fallback and the read-only display report.
Use a managed USB stick with **Update boot and programs (preserve data)**.
Boot `os1`. Run these commands in the text shell.

```text
os.display.gm206.diagnostics > /gm206-gsh.txt
touch "/etc/gsh.boot"
reboot
```

The next boot reports that no compatible graphics driver can start a native session.
Verify that the local `sh` prompt accepts input. Remove the marker with `rm "/etc/gsh.boot"`.
Keep `/gm206-gsh.txt` for comparison if the report changes.

The previous desktop capture confirms AUX 1, SOR 1, link 2, and head 0.
The DisplayPort link uses four HBR2 lanes.
The firmware surface records do not supply a usable native scanout allocation.
The first desktop target remains 1920 by 1080 at 60.000 Hz over the captured DisplayPort route.

### Local measurements

The comparison uses `45c130d` with the GM206 boot-state changes as the baseline.
The candidate adds the graphics shell changes to that source.
Both builds use Debug, GCC 11.2.0, and kernel memory checks on the same arm64 Mac.

| File | Baseline bytes | Candidate bytes | Difference |
| --- | ---: | ---: | ---: |
| Kernel ELF | 664,488 | 664,808 | +320 (+0.05%) |
| Initrd | 9,728 | 10,240 | +512 (+5.26%) |
| Graphics program | 12,568 | 16,776 | +4,208 (+33.48%) |
| UEFI ISO | 4,407,296 | 4,407,296 | 0 (0%) |
| BIOS image | 1,474,560 | 1,474,560 | 0 (0%) |

The focused QEMU checks use QEMU 11.1.3, q35, four vCPUs, 128 MiB RAM, and OVMF for UEFI.
One run took 4.04 s with saved preferences and 4.10 s with the default mode.
These times include guest boot, desktop checks, and teardown.
The fallback run took 2.57 s and reached the text prompt.
The baseline has no comparable direct graphical boot workload.
Physical startup and graphics performance remain unmeasured.

`./scripts/check-local.sh` passed all 918 checks on the candidate.
The host suite took 29.70 s, the normal-image suite 426.16 s, and the diagnostic-image suite 9.88 s.
See `build/gsh-full-local.log` for the full run.
Independent review found a startup lifetime defect. A separate review verified the commit-based correction.
Committed-session release and diagnostic boot with the marker present still need dedicated regression tests.
