# Display session ABI

The Display service reports the boot mode and any runtime modes after the trusted graphics driver binds.
A DisplaySession grants exclusive control of pixel output until its last reference closes.

```text
root grant -> Display -> open(mode_id) -> DisplaySession -> fill(rectangle, rgb)
last session reference closes -> restore boot mode -> redraw active text terminal
```

| Interface | Member | Input | Output |
| --- | --- | --- | --- |
| Display v1 | `mode` | Empty | Boot `os1_display_mode` |
| Display v1 | `list_modes` | Empty | `os1_display_modes` with up to three entries |
| Display v1 | `mode_at` | Zero-based 32-bit index | One mode or empty result at end |
| Display v1 | `open` | 32-bit mode ID | One owned DisplaySession descriptor |
| DisplaySession v1 | `fill` | `os1_display_fill` | Empty |

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
When the session ends, the kernel redraws the active terminal and clears pixels outside its grid.
The full text redraw holds the console output lock with local interrupts disabled.

Run `inspect os.display` to read availability and capabilities.
This operation reads the published display binding. A failed restoration withdraws its mode list.
The service reports the boot geometry and the number of modes. It reports `bochs-display` when runtime switching is available.
BIOS VGA text exposes no graphics mode.

Run `gfxdemo` from the disk shell.
Use Up and Down to highlight a mode. Press Enter to apply the highlighted mode.
The boot mode is highlighted first.
Move the mouse to move the pointer.
Press Escape or `q` to restore the console.
The demo selects the first USB pointer or falls back to the first non-USB pointer.
A detached pointer ends the demo and releases both sessions.


The demo waits on the selected pointer subscription and its console input session.
It applies motion in event order and redraws once per batch of up to 32 events.
After event loss, it reads the current state and ignores events covered by that snapshot.
On exit, it reports event publication gaps, event age, and cursor draw duration.
The report counts samples at or above 50 ms and includes maximum values and sample counts.
A zero sample count means no timing result for that measure.
Publication gaps can include time when the pointer was stationary.

### Indexed mode enumeration

Display v1 method `mode_at` (member 4) accepts a copied 32-bit zero-based index.
It returns one `os1_display_mode` record. The index equal to the mode count returns an empty result.
A larger index returns an invalid-argument error. Use the returned ID to open a session.
The older `list_modes` reply retains its fixed layout.

The `bits_per_pixel` field describes framebuffer storage.
The firmware renderer supports packed 24-bit and 32-bit RGB or BGR storage.
A 32-bit RGB pixel uses 24 color bits. The remaining bits do not increase link color depth.
