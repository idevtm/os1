# Fonts

The text console and the graphics shell use the Spleen bitmap font from font files on the disk.
The kernel and `gsh` each keep one built-in font as a fallback. No program changes the scale of a font.

```text
third_party/spleen/*.bdf -> make_font.py (build) -> /share/fonts/spleen-<size>.font on the disk
sh start on the console -> read spleen-8x16.font -> console set_glyphs -> text console
gsh mode change -> read the font file for the mode height -> bitmap requests of the session
file absent or not valid -> built-in font of the kernel (8 x 16) or of gsh (8 x 8)
```

| Topic | Link |
| --- | --- |
| Fonts that the project examined | [Font options](#font-options) |
| Font for each program and mode | [Font selection](#font-selection) |
| Layout of a font file | [Font file format](#font-file-format) |
| Console request for glyphs | [Text console](#text-console) |
| Effects and checks | [Impact and validation](#impact-and-validation) |

## Font options

The project examined these bitmap fonts with a free license on November 14, 2026.
The license column gives the result of a web search. Only the Spleen license file is in the repository.

| Font | License | Sizes of interest | Result |
| --- | --- | --- | --- |
| [Spleen](https://github.com/fcambus/spleen) | BSD 2-Clause | 8 × 16, 12 × 24, 16 × 32 | Selected. It has thin strokes and three sizes for the supported modes. |
| [Terminus](https://terminus-font.sourceforge.net/) | SIL OFL 1.1 | 8 × 16, 10 × 20, 12 × 24, 16 × 32, and more | Not selected. Its README rates the normal 16 × 32 style lower than its other sizes. |
| [Cozette](https://github.com/slavfox/Cozette) | MIT | One small size | Not selected. It is too small for a 3840 × 2160 mode without a scale. |
| UNSCII | Public domain | 8 × 8, 8 × 16 | Not selected. Its strokes are not thinner than those of the built-in font. |
| Oldschool PC fonts | CC BY-SA 4.0 | 8 × 16 and more | Not selected. The license has a share-alike condition. |

The Spleen files are in `third_party/spleen`, version 2.2.0, with the license file and without changes.

## Font selection

| Program | Condition | Font file | Fallback without the file |
| --- | --- | --- | --- |
| `sh` on the text console | Framebuffer console | `/share/fonts/spleen-8x16.font` | Font in the kernel, 8 × 16 |
| `gsh` | Mode height 1800 or more | `/share/fonts/spleen-16x32.font` | Font in `gsh`, 8 × 8 |
| `gsh` | Mode height 1000 to 1799 | `/share/fonts/spleen-12x24.font` | Font in `gsh`, 8 × 8 |
| `gsh` | Mode height less than 1000 | `/share/fonts/spleen-8x16.font` | Font in `gsh`, 8 × 8 |

`gsh` reads the font file again at each mode change. Each character has the pixel size of its font cell.
The spaces between the desktop elements use a unit of 2 pixels with the 16 × 32 font and 1 pixel with the other fonts.
The shell window has 80 columns and 24 rows if the mode has the space.
A shell that has a byte stream as its input does not load a font. The shell window of `gsh` starts such a shell.
The VGA text console of a BIOS boot uses the font of the display adapter.
The disk deployment writes a font file only if the disk does not have it. Thus a font file that the user replaced stays.
For the same cause, a later version of a font file in the build does not replace the file on an existing disk.
The **Update boot files** action of the USB tool does not write font files. The programs then use the built-in fonts.
`gsh` uses a font file only if one character goes into one bitmap request of 232 bytes.

## Font file format

All values are bytes. The file has no padding after the glyphs.

| Offset | Bytes | Content |
| --- | --- | --- |
| 0 | 8 | `OS1FONT1` |
| 8 | 1 | Cell width in pixels, 1 to 32 |
| 9 | 1 | Cell height in pixels, 1 to 64 |
| 10 | 1 | First character code. The value must be 0. |
| 11 | 1 | Character count. The value must be 128. |
| 12 | 4 | Zero |
| 16 | 128 × height × row bytes | Glyphs in character code sequence |

One glyph has `height` rows. One row has (width + 7) / 8 bytes, without the remainder.
Bit 0 of the first byte of a row is the left pixel.
A character without a glyph in the source font has an empty glyph.
The loader is in [font.hpp](../../src/user/include/os1/font.hpp). It refuses a file with a different size or header.
`sh` uses only a file with a cell of 8 × 16 pixels.
The build tool is [make_font.py](../../cmake/scripts/make_font.py). It refuses a BDF font with a glyph that does not fill the font cell.

## Text console

The Console member `set_glyphs` replaces glyphs of the 8 × 16 font of the framebuffer text console.
See the [Console ABI](../native-api/abi/kernel-console-abi.md#console-font).
The kernel keeps its built-in font for each glyph without a replacement.
`sh` sends the 128 glyphs of the font file in 10 requests when it starts on the console.
A request that changes no glyph causes no new presentation of the console.

## Impact and validation

| Area | Effect |
| --- | --- |
| Project scope | The font data moves from the programs to files. The console request is generic and has no device code. |
| Boot and platform | No change of the boot paths. A machine without the disk uses the built-in fonts. |
| Memory and isolation | The kernel holds 2,176 bytes for loaded glyphs. It checks the glyph range and the payload size. `gsh` holds 32,784 bytes for one font file, and `sh` holds 2,064 bytes. |
| Concurrency | The glyph change uses the console output lock. |
| Interfaces | Console v1 adds member 9. The disk manifest adds three resource files. DisplaySession v1 does not change. |
| Runtime resources | See the table below. Text needs more bitmap requests than before. The request time on a machine is unmeasured. |
| Validation and documents | A build check converts the fonts. The QEMU display check examines the window size and the console prompt. |

| Item | Before | This tree | Difference |
| --- | --- | --- | --- |
| Kernel ELF | 663,408 bytes | 664,144 bytes | +736 bytes (+0.11%) |
| BIOS kernel ELF | 490,824 bytes | 491,336 bytes | +512 bytes (+0.10%) |
| `gsh` program | Unmeasured | 53,456 bytes | Undefined. The build before this change has no size record. |
| `sh` program | Unmeasured | 242,336 bytes | Undefined, for the same cause. |
| Font files on the disk | 0 bytes | 16,432 bytes | Three new files |

The BIOS kernel uses 491,336 of 516,096 bytes of its reserved window.

One bitmap request holds 232 bytes. The table gives the characters in one request and the requests for a row of 80 characters.

| Font | Characters in one request | Requests for 80 characters |
| --- | --- | --- |
| Earlier 8 × 8 font, scale 1 | 29 | 3 |
| Earlier 8 × 8 font, scale 2 | 7 | 12 |
| Spleen 8 × 16 | 14 | 6 |
| Spleen 12 × 24 | 6 | 14 |
| Spleen 16 × 32 | 3 | 27 |

The `os1_font_contract` check converts each Spleen file and examines the header, the size, and the bit sequence of one glyph.
The `os1_smoke_display_session_uefi` check passes only if `gsh` loaded the 8 × 16 font, because the shell window then has 408 rows of pixels.
The same check finds the prompt `os1>` on the text console with the glyphs of the Spleen file.
No check sends a `set_glyphs` request that the kernel must refuse.
No check covers the 12 × 24 font or the 16 × 32 font in a session. QEMU Standard VGA has no mode of that height in the list of the check.
The desktop has not run these fonts.
