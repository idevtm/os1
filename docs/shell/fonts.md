# Fonts

The text console and the graphics shell use Spleen bitmap fonts from font files on the disk.
The disk has six sizes. A configuration file selects the font of each program.
By default, `sh` uses the 6 × 12 font on the console, and `gsh` uses one step smaller than before.

```text
third_party/spleen/*.bdf -> make_font.py (build) -> /share/fonts/spleen-<size>.font on the disk
sh start on the console -> /etc/sh.conf -> font file -> console set_glyphs -> new cell grid
gsh mode change -> font step of /etc/gsh.conf for the mode height -> font file -> bitmap requests
file absent or not valid -> built-in font of the kernel (8 x 16) or of gsh (8 x 8)
```

| Topic | Link |
| --- | --- |
| Fonts that the project examined | [Font options](#font-options) |
| Font sizes on the disk | [Font files on the disk](#font-files-on-the-disk) |
| Font of `sh` and its configuration file | [Shell configuration](#shell-configuration) |
| Font steps of `gsh` | [Graphics shell font steps](#graphics-shell-font-steps) |
| Layout of a font file | [Font file format](#font-file-format) |
| Console request for glyphs and cell size | [Text console](#text-console) |
| Effects and checks | [Impact and validation](#impact-and-validation) |

## Font options

The project examined these bitmap fonts with a free license on November 14, 2026.
The license column gives the result of a web search. Only the Spleen license file is in the repository.

| Font | License | Sizes of interest | Result |
| --- | --- | --- | --- |
| [Spleen](https://github.com/fcambus/spleen) | BSD 2-Clause | 5 × 8 to 32 × 64 | Selected. It has thin strokes and six sizes. |
| [Terminus](https://terminus-font.sourceforge.net/) | SIL OFL 1.1 | 8 × 16, 10 × 20, 12 × 24, 16 × 32, and more | Not selected. Its README rates the normal 16 × 32 style lower than its other sizes. |
| [Cozette](https://github.com/slavfox/Cozette) | MIT | One small size | Not selected. It is too small for a 3840 × 2160 mode without a scale. |
| UNSCII | Public domain | 8 × 8, 8 × 16 | Not selected. Its strokes are not thinner than those of the built-in font. |
| Oldschool PC fonts | CC BY-SA 4.0 | 8 × 16 and more | Not selected. The license has a share-alike condition. |

The Spleen files are in `third_party/spleen`, with the license file and without changes. See [ORIGIN.md](../../third_party/spleen/ORIGIN.md) for the upstream commit.

## Font files on the disk

| Font file | Cell | File bytes | `sh` on the console | `gsh` |
| --- | --- | --- | --- | --- |
| `/share/fonts/spleen-5x8.font` | 5 × 8 | 1,040 | Yes | Yes |
| `/share/fonts/spleen-6x12.font` | 6 × 12 | 1,552 | Yes, default | Yes, default under 1000 |
| `/share/fonts/spleen-8x16.font` | 8 × 16 | 2,064 | Yes | Yes, default from 1000 |
| `/share/fonts/spleen-12x24.font` | 12 × 24 | 6,160 | Yes | Yes, default from 1800 |
| `/share/fonts/spleen-16x32.font` | 16 × 32 | 8,208 | Yes | Yes |
| `/share/fonts/spleen-32x64.font` | 32 × 64 | 32,784 | No. The console cell has a limit of 16 × 32. | Yes |

Each character has the pixel size of its font cell. No program changes the scale of a font.
The disk deployment writes a font file or a configuration file only if the disk does not have it. Thus a file that the user replaced stays.
An existing disk gets each file that it does not have, for example a new font file or `/etc/sh.conf`. A changed default in the build does not replace an existing file.
The **Update boot files** action of the USB tool does not write font files. The programs then use the built-in fonts.
The VGA text console of a BIOS boot uses the font of the display adapter.

## Shell configuration

`sh` reads `/etc/sh.conf` when it starts on the text console. The file has this format. End each line with a newline.

```ini
version=1
# A comment line starts with #. An empty line is also a comment.
font=/share/fonts/spleen-6x12.font
```

| Key | Value | Effect |
| --- | --- | --- |
| `version` | `1` | Necessary. The first setting line. |
| `font` | Absolute path of a font file, without spaces | `sh` gives this font to the text console. |
| `font` | `none` | The console keeps the font that it has. |

The deployment writes the file above as the default. It also lists the font files in comments.
Without the file, `sh` uses `/share/fonts/spleen-6x12.font`.
If the file is not valid, `sh` writes `shell: /etc/sh.conf is not valid; the default font is in use` and uses the 6 × 12 font.
A file is not valid if it has an unknown key, a second `font` line, a line without a newline, or more than 1,024 bytes.
The console accepts only a cell of 5 to 16 pixels wide and 8 to 32 pixels high.
If the font file is absent or not valid, `sh` writes `shell: the console font file is absent or not valid` and the console keeps its font.
A shell that has a byte stream as its input does not read the file. The shell window of `gsh` starts such a shell.
The parser is in [settings.hpp](../../src/common/text/settings.hpp).

## Graphics shell font steps

`gsh` selects the font of a font step for the height of the mode. It reads the font file again at each mode change.

| Mode height | Default font | Font before this change |
| --- | --- | --- |
| Less than 1000 | `/share/fonts/spleen-6x12.font` | `/share/fonts/spleen-8x16.font` |
| 1000 to 1799 | `/share/fonts/spleen-8x16.font` | `/share/fonts/spleen-12x24.font` |
| 1800 or more | `/share/fonts/spleen-12x24.font` | `/share/fonts/spleen-16x32.font` |

`/etc/gsh.conf` can replace the default steps. One `font` line is one step: the lowest mode height, a space, and the font path.

```ini
version=1
width=1920
height=1080
font=0 /share/fonts/spleen-6x12.font
font=1000 /share/fonts/spleen-8x16.font
font=1800 /share/fonts/spleen-16x32.font
```

- The heights of the steps must rise from line to line. The file can have 1 to 8 steps.
- `gsh` uses the last step with a height that is not more than the mode height.
- If the file has steps but no step for the mode height, `gsh` uses its built-in 8 × 8 font.
- If the file has no `font` line, `gsh` uses the default steps.
- `width` and `height` are the saved mode. The file must have both or neither.

When the desktop saves a mode, `gsh` writes the version line, each other line of the old file, and the new mode lines. Thus the font steps and the comments stay.
If the file is not valid, `gsh` writes `gsh: /etc/gsh.conf is not valid; the default settings are in use`. It then uses the default steps and the first mode.
A later save replaces a file that is not valid with only the version line and the mode lines. A save also drops the other lines if the new file would be larger than 1,024 bytes. `gsh` then writes `gsh: /etc/gsh.conf is too large; the save keeps only the mode`.
A file that is larger than 1,024 bytes or that cannot be read is also not valid. `gsh` then writes `gsh: /etc/gsh.conf is too large or cannot be read; the default settings are in use`.
If the font file of a step is absent or not valid, `gsh` uses its built-in 8 × 8 font.
The spaces between the desktop elements use a unit of 4 pixels with a font of 32 pixels wide, 2 pixels with a font of 16 to 31 pixels wide, and 1 pixel with the other fonts.
The shell window has 80 columns and 24 rows if the mode has the space.
A glyph that does not go into one bitmap request of 232 bytes goes into bands of rows. The 32 × 64 font uses two bands.

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
The build tool is [make_font.py](../../cmake/scripts/make_font.py). It refuses a BDF font with a glyph that does not fill the font cell.

## Text console

The Console member `set_glyphs` sets the cell size and the glyphs of the framebuffer text console.
See the [Console ABI](../native-api/abi/kernel-console-abi.md#console-font).

```text
first request: first=0, new cell size -> loaded glyphs removed -> new grid = screen / cell
-> each terminal keeps its rows and the cursor row -> later requests add glyphs -> new presentation
```

A new cell size changes the number of columns and rows of each terminal. Each row keeps its left cells, and the cursor row stays on the screen.
The terminal buffers have space for the grid of a 5 × 8 cell. The kernel allocates them at boot for a framebuffer console.
A glyph without a loaded copy is empty, except with the 8 × 16 cell. That cell uses the built-in font for such a glyph.
`sh` sends the 128 glyphs in requests of up to 252 glyph bytes. The 6 × 12 font needs 7 requests, and the 16 × 32 font needs 43.
The console draws after the request with character 127. An earlier request only marks the cells, and the next console output draws them.
A request that changes no glyph and no cell size causes no new drawing of cells.

## Impact and validation

| Area | Effect |
| --- | --- |
| Project scope | Two small text files configure the fonts. The parser is one header without file access. The console request stays generic. |
| Boot and platform | No change of the boot paths. The framebuffer console allocates more pages at boot. The BIOS kernel has no new data in its image. |
| Memory and isolation | The kernel checks the cell, the range, and the payload size before a change. The glyph storage is 8,192 bytes in two boot pages. See the memory table. |
| Concurrency | The cell change and the terminal changes use the console output lock. A display owner keeps its pixels. The console draws again at release. |
| Interfaces | Console v1 member 9 has a new header: `first`, `count`, `width`, `height`. The earlier layout of member 9 was not published. The disk manifest adds three font files and `/etc/sh.conf`. `/etc/gsh.conf` accepts `font` lines, and an old file stays valid. |
| Runtime resources | See the tables below. The request time on a machine is unmeasured. |
| Validation and documents | Host tests check the parser, the payload check, and the grid change. The QEMU display check uses the new window size and the 6 × 12 prompt. |

These sizes are from the Debug build with GCC 11.2.0. The values before are from the font change in commit `6bdcffa`, as its document records them.

| Item | Before | This tree | Difference |
| --- | --- | --- | --- |
| Kernel ELF | 664,144 bytes | 666,296 bytes | +2,152 bytes (+0.32%) |
| BIOS kernel ELF | 491,336 bytes | 493,000 bytes | +1,664 bytes (+0.34%) |
| `gsh` program | 53,456 bytes | 57,544 bytes | +4,088 bytes (+7.6%) |
| `sh` program | 242,336 bytes | 243,776 bytes | +1,440 bytes (+0.59%) |
| Font files on the disk | 16,432 bytes | 51,808 bytes | +35,376 bytes (+215%) |

The BIOS kernel image and its data end at physical address 0x1BB000. With the reserve of 0x4000 bytes, it uses 0xBF000 of 0xC0000 bytes of its window.
The first form of this change put the glyph storage in the kernel data. That form went past the BIOS window by 0x1000 bytes, and the build check refused it.

The terminal and shadow buffers are calculated values for 12 terminals and one shadow buffer, in pages of 4,096 bytes. No machine measured them.

| Boot framebuffer | Before (8 × 16 grid) | This tree (5 × 8 capacity) | Difference |
| --- | --- | --- | --- |
| 1280 × 800 | 52 pages | 169 pages, plus 2 glyph pages | +119 pages (+229%) |
| 1920 × 1080 | 104 pages | 338 pages, plus 2 glyph pages | +236 pages (+227%) |
| 3840 × 2160 | 416 pages | 1,326 pages, plus 2 glyph pages | +912 pages (+219%) |

One bitmap request holds 232 bytes. The table gives the requests of `gsh` for a row of 80 characters.

| Font | Characters in one request | Requests for 80 characters |
| --- | --- | --- |
| Spleen 5 × 8 | 46 | 2 |
| Spleen 6 × 12 | 25 | 4 |
| Spleen 8 × 16 | 14 | 6 |
| Spleen 12 × 24 | 6 | 14 |
| Spleen 16 × 32 | 3 | 27 |
| Spleen 32 × 64 | 1 in the band of 58 rows, 9 in the band of 6 rows | 89 |

The `os1_font_contract` check converts each of the six Spleen files and examines the header, the size, and the bit sequence of one glyph.
The `os1_smoke_display_session_uefi` check passes only if `gsh` loaded the 6 × 12 font, because the shell window then has 308 rows of pixels.
The same check finds the prompt `os1>` on the text console in the 6 × 12 glyphs.
The host tests in [console_font_tests.cpp](../../tests/host/common/console_font_tests.cpp) check the settings parser, the glyph payload check, and the grid change.
No check sends a `set_glyphs` request to the kernel that the kernel must refuse.
No check covers the fonts other than 6 × 12 in a session. No QEMU check uses a font step from `/etc/gsh.conf`.
No check covers the 32 × 64 bands of `gsh` on a display.
The desktop has not run these fonts.
