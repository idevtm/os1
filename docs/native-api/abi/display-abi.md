# Display session ABI

The Display service reports the boot mode and any runtime modes after the trusted graphics driver binds.
A DisplaySession grants exclusive control of pixel output until its last reference closes.

```text
root grant -> Display -> open(mode_id) -> DisplaySession -> fill(rectangle, rgb)
DisplaySession -> fill_area, copy, bitmap -> framebuffer module -> shadow and video memory
diagnostic session closes -> restore boot mode -> redraw text
native session closes -> release exclusive ownership -> retain native scanout
```

| Interface | Member | Input | Output |
| --- | --- | --- | --- |
| Display v1 | `mode` | Empty | Boot `os1_display_mode` |
| Display v1 | `list_modes` | Empty | `os1_display_modes` with up to three entries |
| Display v1 | `mode_at` | Zero-based 32-bit index | One mode or empty result at end |
| Display v1 | `open` | 32-bit mode ID | One owned DisplaySession descriptor |
| Display v1 | `open_native` | 32-bit runtime mode ID | One owned DisplaySession descriptor |
| DisplaySession v1 | `fill` | `os1_display_fill` | Empty |
| DisplaySession v1 | `commit` | Empty | Empty |
| DisplaySession v1 | `fill_area` | `os1_display_fill` | Empty |
| DisplaySession v1 | `copy` | `os1_display_copy` | Empty |
| DisplaySession v1 | `bitmap` | `os1_display_bitmap` header and bitmap bytes | Empty |
| DisplaySession v1 | `cursor` | `os1_display_cursor` | Empty |

The [UAPI header](../../../src/uapi/os1/display.h) defines the copied records.
The kernel grants `OS1_RIGHT_DISPLAY` on the initial root descriptor.
A launch can attenuate that right. Binding requires the root object or a returned session object.
The session descriptor has close-on-exec set.
A bound client and its pending Operations retain the session.
A second acquire returns `OS1_ERR_WOULD_BLOCK`.
Unsupported hardware or an unavailable package gives `OS1_ERR_UNSUPPORTED`.

Mode ID 1 identifies the boot framebuffer.
The firmware backend offers only this mode.
A matching QEMU Standard VGA backend also offers 800 by 600 and 1280 by 720 RGB32 modes.
The list skips any alternate that duplicates the boot mode.
The reference GTX 950 backend offers the native mode and smaller scaled modes of its monitor.
The `mode_at` method reports each mode. The list has a capacity of 12 modes.
Mode IDs in this list are local to the boot. An acquire rejects an unknown ID.
The kernel checks a mode switch before it publishes the session.
If the check fails, it restores the boot mode and returns an I/O error.
If restoration fails, the kernel disables pixel output and further acquires. Serial output remains available.
The mode reports width, height, and bits per pixel.
The driver supports 24-bit and 32-bit firmware framebuffers with RGB or BGR byte order.
Supported graphics geometry is at most 4096 by 2160 pixels.
The kernel keeps the hardware address and pitch private.

Coordinates start at the top left. The rectangle must fit fully inside the mode.
A fill covers one to 4096 pixels and uses an RGB value in the form `0xRRGGBB`.
The kernel rejects invalid inputs before it writes pixels.
A successful call completes the writes. It does not synchronize to display refresh.

The console continues to update its text cells and serial output during graphics.
The console output lock also serializes pixel writes, keyboard echo, and terminal switches.
When an `open` session ends, the kernel redraws the active terminal and clears pixels outside its grid.
The full text redraw holds the console output lock with local interrupts disabled.

Run `inspect os.display` to read availability and capabilities.
This operation reads the published display binding. A failed restoration withdraws its mode list.
The service reports the boot geometry and the number of modes. It reports `bochs-display` when runtime switching is available.
BIOS VGA text exposes no graphics mode.

Run `gsh "--menu"` from the disk shell for the display diagnostic.
Use Up and Down to highlight a mode. Press Enter to apply the highlighted mode.
The boot mode is highlighted first. The menu draws the highlighted mode on one console row.
A serial console receives one line for each change.
Move the mouse to move the pointer.
Press M to show the mode list. Select a mode with Up and Down or with the pointer. Press Enter or the left button to apply it.
A new mode asks for a confirmation. Press Enter in 10 seconds to keep it. If not, the mode before it returns.
Press C to change between the hardware pointer and the drawn pointer.
Press T to open the shell window. Press Escape to close the window.
With no open window, press Escape or `q` to restore the console.
The demo selects the first USB pointer or falls back to the first non-USB pointer.
A detached pointer ends the demo and releases both sessions.


The demo waits on the selected pointer subscription and its console input session.
It applies motion in event order and redraws once per batch of up to 32 events.
After event loss, it reads the current state and ignores events covered by that snapshot.
On exit, it reports event publication gaps, event age, and cursor draw duration.
The report counts samples at or above 50 ms and includes maximum values and sample counts.
A zero sample count means no timing result for that measure.
Publication gaps can include time when the pointer was stationary.

### Fast drawing requests

DisplaySession members 3 to 6 are `fill_area`, `copy`, `bitmap`, and `cursor`.
Members 3 to 5 need a surface with 32-bit pixels. A different surface returns an invalid-argument error.
A caller sends a `fill_area` request of one pixel to find if the session has these members.

| Member | Rule |
| --- | --- |
| `fill_area` | The rectangle has a maximum of 65,536 pixels. The color uses the form `0xRRGGBB`. |
| `copy` | The source and the target have the same size and a maximum of 65,536 pixels. They can overlap. |
| `bitmap` | Each row starts at a new byte. Bit 0 of a byte is the left pixel. The bitmap has a maximum of 232 bytes. |
| `cursor` | The position is the pointer tip in surface pixels. A zero `visible` value hides the pointer. The `rgb` value is the body color in the form `0xRRGGBB`. A larger value returns an invalid-argument error. |

A `bitmap` request contains the 24-byte header and only the necessary bitmap bytes.
A set bit draws the foreground color. A clear bit draws the background color.
The background value `0x01000000` keeps the pixel of a clear bit.
Each rectangle must fit fully inside the mode. The framebuffer module refuses a request before it writes pixels.
A caller divides a larger area into requests. For an overlapping move down, send the lowest band first.
The kernel keeps a shadow of the surface in normal memory for these members.
A `copy` request reads the shadow. If the kernel has no memory for the shadow, the request reads video memory.
The shadow starts with zero pixels. Draw an area before you copy it.
The `fill` member does not update the shadow. Do not use `fill` and `copy` for the same area.
The `cursor` record has 16 bytes. The kernel refuses the earlier record of 12 bytes.
Only the GTX 950 backend has a hardware pointer. A different session returns `OS1_ERR_UNSUPPORTED`.
A failed pointer request returns an I/O error. The caller then draws the pointer.
See the [drawing work](../../hardware/nvidia-mode-setting-plan.md#drawing-speed-scaled-modes-and-hardware-pointer) for the design.

### Indexed mode enumeration

Display v1 method `mode_at` (member 4) accepts a copied 32-bit zero-based index.
It returns one `os1_display_mode` record. The index equal to the mode count returns an empty result.
A larger index returns an invalid-argument error. Use the returned ID to open a session.
The older `list_modes` reply retains its fixed layout.

The `bits_per_pixel` field describes framebuffer storage.
The firmware renderer supports packed 24-bit and 32-bit RGB or BGR storage.
A 32-bit RGB pixel uses 24 color bits. The remaining bits do not increase link color depth.


### Native acquisition

Display v1 method `open_native` (member 5) rejects the boot framebuffer mode.
The caller uses `commit` (session member 2) after it draws its initial frame.
Before the first commit in a boot, closing the last reference restores the boot mode.
After commit, closing the last reference releases ownership and preserves the native scanout.
The framebuffer text console stays suspended. Serial output remains available.
Only another native acquisition can claim the display until reboot.
A failed initial takeover uses the existing mode-switch recovery path.
