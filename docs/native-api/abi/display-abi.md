# Fixed display ABI

The Display service reports one firmware mode after the trusted graphics driver binds.
A DisplaySession grants exclusive control of pixel output until its last reference closes.

```text
root grant -> Display -> open(mode_id) -> DisplaySession -> fill(rectangle, rgb)
last session reference closes -> redraw active text terminal
```

| Interface | Member | Input | Output |
| --- | --- | --- | --- |
| Display v1 | `mode` | Empty | `os1_display_mode` |
| Display v1 | `open` | 32-bit mode ID | One owned DisplaySession descriptor |
| DisplaySession v1 | `fill` | `os1_display_fill` | Empty |

The [UAPI header](../../../src/uapi/os1/display.h) defines the copied records.
The kernel grants `OS1_RIGHT_DISPLAY` on the initial root descriptor.
A launch can attenuate that right. Binding requires the root object or a returned session object.
The session descriptor has close-on-exec set.
A bound client and its pending Operations retain the session.
A second acquire returns `OS1_ERR_WOULD_BLOCK`.
Unsupported hardware or an unavailable package gives `OS1_ERR_UNSUPPORTED`.

Mode ID 1 identifies the fixed framebuffer for this boot.
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

Inspect `context os.display` to read availability and capabilities.
This operation reads the published boot binding.
The firmware driver reports its capabilities separately from the PCI inventory.
BIOS VGA text exposes no graphics mode.

Run `gfxdemo` from the disk shell.
Select the listed mode with `1` or Enter.
Move the mouse to move the pointer.
Press Escape or `q` to restore the console.
The demo selects the first USB pointer or falls back to the first non-USB pointer.
A detached pointer ends the demo and releases both sessions.
