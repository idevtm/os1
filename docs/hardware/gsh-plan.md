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
```

A fresh image installs `gsh` in place of `gfxdemo`.
An update that preserves data keeps existing files outside the deployment manifest.
If the old executable is still present, remove it with `rm "/bin/gfxdemo"`.
The lower right corner shows the UTC date and time. Use R to reboot or S to shut down.
Use M to open the next mode. Use C to change between the hardware pointer and the drawn pointer.
Use T to open the shell window.
If a pointer disconnects, the clock and keyboard controls remain active.
If `gsh` exits after takeover, the text shell is available over serial.

## Shell window

The T key opens one window with a command line. The window has the keyboard until Esc closes it.
Enter runs the typed line. `gsh` writes the line to a temporary file.
It starts `/bin/sh` with that file as standard input and a second temporary file as both outputs.
It waits for the end of the process, then shows the output file in the window.

```text
typed line -> temporary file -> sh (standard input) -> temporary file -> window text
window move -> copy request -> draw the area that the window left
```

| Limit | Cause |
| --- | --- |
| The desktop does not update while a command runs. | `waitpid` has no time limit, and a user program has one thread. |
| A command that does not end stops the desktop until a reset. | The kernel has no request that stops an ordinary child process. |
| A command cannot read from the keyboard. | The kernel has no pipe or terminal object for a second input stream. |
| Each command starts in a new shell. A `cd` command has no later effect. | The window starts one `sh` process for each line. |
| One command shows a maximum of 65,536 output bytes. | This is the size limit of a temporary file. |

Press the left button on the window title and move the mouse to move the window.
The move uses the `copy` request of the [Display session ABI](../native-api/abi/display-abi.md#fast-drawing-requests).
A session without that request draws the window again.

## Later stages

1. Add a kernel byte stream object and a wait with a time limit. Then connect a child `sh` that stays open.
   Give it full line editing, history, and completion in its first interactive release.
2. Add a mode chooser, timed confirmation, and rollback to the preceding native mode. Save only a confirmed mode.
3. Increase the reserved window of the BIOS kernel image before more kernel features.
4. Show the current mode in the upper right corner.
5. Find why the window move does not operate on the GTX 950 desktop. Then add a window size change.
6. Use a larger font for large modes.

The [fifth desktop test](nvidia-mode-setting-plan.md#fifth-desktop-test) is the source of items 4 to 6.

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
