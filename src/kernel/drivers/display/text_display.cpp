// Text rendering backends for logical terminals. VGA text mode stays fixed
// while the framebuffer backend can now present a larger cell grid.
#include "drivers/display/text_display.hpp"

#include "arch/x86_64/cpu/io_port.hpp"
#include "debug/debug.hpp"
#include "font8x16.h"
#include "handoff/memory_layout.h"
#include "terminal/cells.hpp"
#include "util/memory.h"

namespace
{
// The built-in font has this cell size.
constexpr uint32_t kBuiltInCellWidth = 8;
constexpr uint32_t kBuiltInCellHeight = 16;
constexpr uint32_t kGlyphBytesLimit = OS1_CONSOLE_CELL_MAX_HEIGHT * 2;
static_assert(128 * kGlyphBytesLimit == kTextDisplayGlyphStorageBytes);
constexpr uint32_t kFramebufferBackground = 0x00000000u;
constexpr uint16_t kVgaBlankCell = 0x0720;
constexpr uint32_t kVgaPalette[16] = {
    0x00000000u,
    0x000000AAu,
    0x0000AA00u,
    0x0000AAAAu,
    0x00AA0000u,
    0x00AA00AAu,
    0x00AA5500u,
    0x00AAAAAAu,
    0x00555555u,
    0x005555FFu,
    0x0055FF55u,
    0x0055FFFFu,
    0x00FF5555u,
    0x00FF55FFu,
    0x00FFFF55u,
    0x00FFFFFFu,
};

VgaTextDisplay g_vga_text_display;
FramebufferTextDisplay g_framebuffer_text_display;
// The cell size of the font. A program can change it.
uint32_t g_cell_width = kBuiltInCellWidth;
uint32_t g_cell_height = kBuiltInCellHeight;
// Glyphs that a program loaded, in pages of the boot allocation. One glyph row has
// (g_cell_width + 7) / 8 bytes. A glyph without a loaded copy uses the built-in font if the cell
// has its size, and is empty if not.
uint8_t (*g_loaded_glyphs)[kGlyphBytesLimit] = nullptr;
bool g_glyph_loaded[128];
TextDisplayBackend g_vga_backend{TextDisplayBackendKind::VgaText, &g_vga_text_display};
TextDisplayBackend g_framebuffer_backend{TextDisplayBackendKind::FramebufferText,
                                         &g_framebuffer_text_display};

[[nodiscard]] inline uint64_t buffer_size_bytes(uint16_t columns, uint16_t rows)
{
    return (uint64_t)columns * (uint64_t)rows * sizeof(uint16_t);
}

[[nodiscard]] inline uint16_t compute_visible_columns(uint16_t columns,
                                                      const FramebufferTextDisplay& display)
{
    return (columns < display.columns) ? columns : display.columns;
}

[[nodiscard]] inline uint16_t compute_visible_rows(uint16_t rows,
                                                   const FramebufferTextDisplay& display)
{
    return (rows < display.rows) ? rows : display.rows;
}

[[nodiscard]] inline uint32_t vga_color(uint8_t index)
{
    return kVgaPalette[index & 0x0Fu];
}

[[nodiscard]] inline bool is_printable_ascii(char c)
{
    return c >= ' ';
}

[[nodiscard]] bool cells_match(const uint16_t* left, const uint16_t* right, uint16_t count)
{
    for(uint16_t index = 0; index < count; ++index)
    {
        if(left[index] != right[index])
        {
            return false;
        }
    }
    return true;
}

void present_vga_text(
    const uint16_t* buffer, uint16_t columns, uint16_t rows, uint16_t cursor_x, uint16_t cursor_y)
{
    if(nullptr == buffer)
    {
        return;
    }

    uint16_t* screen = kernel_physical_pointer<uint16_t>(0xB8000);
    memcpy(screen, buffer, buffer_size_bytes(columns, rows));

    const uint16_t position = (uint16_t)(cursor_y * columns + cursor_x);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(position & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((position >> 8) & 0xFF));
}

void detach_vga_text()
{
    uint16_t* screen = kernel_physical_pointer<uint16_t>(0xB8000);
    memsetw(screen, kVgaBlankCell, 80 * 25 * sizeof(uint16_t));
    outb(0x3D4, 0x0F);
    outb(0x3D5, 0);
    outb(0x3D4, 0x0E);
    outb(0x3D5, 0);
}

void framebuffer_clear(FramebufferTextDisplay& display, uint32_t rgb)
{
    if(!display.available)
    {
        return;
    }

    if(0 == rgb)
    {
        memset(display.framebuffer, 0, (uint64_t)display.pitch_bytes * (uint64_t)display.height);
        return;
    }

    for(uint32_t y = 0; y < display.height; ++y)
    {
        for(uint32_t x = 0; x < display.width; ++x)
        {
            uint8_t* pixel = display.framebuffer + (uint64_t)y * display.pitch_bytes +
                             (uint64_t)x * ((uint64_t)display.bits_per_pixel / 8ull);
            if(24 == display.bits_per_pixel)
            {
                pixel[0] = (uint8_t)(rgb & 0xFFu);
                pixel[1] = (uint8_t)((rgb >> 8) & 0xFFu);
                pixel[2] = (uint8_t)((rgb >> 16) & 0xFFu);
            }
            else
            {
                *((uint32_t*)pixel) = rgb;
            }
        }
    }
}

void framebuffer_set_pixel(FramebufferTextDisplay& display, uint32_t x, uint32_t y, uint32_t rgb)
{
    if(!display.available || (x >= display.width) || (y >= display.height))
    {
        return;
    }

    uint8_t* pixel = display.framebuffer + (uint64_t)y * display.pitch_bytes +
                     (uint64_t)x * ((uint64_t)display.bits_per_pixel / 8ull);
    if(24 == display.bits_per_pixel)
    {
        pixel[0] = (uint8_t)(rgb & 0xFFu);
        pixel[1] = (uint8_t)((rgb >> 8) & 0xFFu);
        pixel[2] = (uint8_t)((rgb >> 16) & 0xFFu);
        return;
    }

    *((uint32_t*)pixel) = rgb;
}

void framebuffer_draw_cell(
    FramebufferTextDisplay& display, uint32_t cell_x, uint32_t cell_y, uint16_t cell, bool inverse)
{
    char character = (char)(cell & 0x00FFu);
    if(!is_printable_ascii(character))
    {
        character = ' ';
    }

    const uint8_t glyph_index = (uint8_t)character;
    const uint8_t attribute = (uint8_t)((cell >> 8) & 0x00FFu);
    const uint32_t fg =
        inverse ? vga_color((attribute >> 4) & 0x0Fu) : vga_color(attribute & 0x0Fu);
    const uint32_t bg =
        inverse ? vga_color(attribute & 0x0Fu) : vga_color((attribute >> 4) & 0x0Fu);

    const bool built_in = g_cell_width == kBuiltInCellWidth && g_cell_height == kBuiltInCellHeight;
    const uint32_t row_bytes = (g_cell_width + 7) / 8;
    for(uint32_t glyph_row = 0; glyph_row < g_cell_height; ++glyph_row)
    {
        uint32_t bits = 0;
        if(g_glyph_loaded[glyph_index & 127])
            for(uint32_t i = 0; i < row_bytes; ++i)
                bits |= uint32_t(g_loaded_glyphs[glyph_index & 127][glyph_row * row_bytes + i])
                        << (8 * i);
        else if(built_in)
            bits = font8x16[glyph_index & 127][glyph_row];
        const uint32_t y = cell_y + glyph_row;
        for(uint32_t glyph_col = 0; glyph_col < g_cell_width; ++glyph_col)
        {
            const bool set = ((bits >> glyph_col) & 1u) != 0;
            framebuffer_set_pixel(display, cell_x + glyph_col, y, set ? fg : bg);
        }
    }
}

void present_framebuffer_text(FramebufferTextDisplay& display,
                              const uint16_t* buffer,
                              uint16_t columns,
                              uint16_t rows,
                              uint16_t cursor_x,
                              uint16_t cursor_y)
{
    if(!display.available || display.graphics_owned || (nullptr == buffer))
    {
        return;
    }

    const uint16_t visible_columns = compute_visible_columns(columns, display);
    const uint16_t visible_rows = compute_visible_rows(rows, display);
    const bool geometry_match =
        (visible_columns == display.columns) && (visible_rows == display.rows);

    for(uint32_t row = 0; row < display.rows; ++row)
    {
        const bool redraw_old_cursor_row = display.cursor_valid && (display.last_cursor_y == row);
        const bool redraw_new_cursor_row = (cursor_y == row) && (cursor_x < visible_columns);

        if(display.shadow_valid && geometry_match && !redraw_old_cursor_row &&
           !redraw_new_cursor_row && (nullptr != display.shadow_buffer) &&
           cells_match(display.shadow_buffer + row * display.columns,
                       buffer + row * columns,
                       visible_columns))
        {
            continue;
        }

        for(uint32_t col = 0; col < display.columns; ++col)
        {
            const uint16_t cell = ((row < visible_rows) && (col < visible_columns))
                                      ? buffer[row * columns + col]
                                      : kVgaBlankCell;
            const bool was_cursor = display.cursor_valid && (display.last_cursor_x == col) &&
                                    (display.last_cursor_y == row);
            const bool is_cursor = (cursor_x == col) && (cursor_y == row) &&
                                   (col < visible_columns) && (row < visible_rows);
            const uint32_t shadow_index = row * display.columns + col;
            const bool shadow_changed = !display.shadow_valid ||
                                        (nullptr == display.shadow_buffer) ||
                                        (display.shadow_buffer[shadow_index] != cell);

            if(!shadow_changed && !was_cursor && !is_cursor)
            {
                continue;
            }

            framebuffer_draw_cell(
                display, col * g_cell_width, row * g_cell_height, cell, is_cursor);
            if(nullptr != display.shadow_buffer)
            {
                display.shadow_buffer[shadow_index] = cell;
            }
        }
    }

    display.shadow_valid = display.shadow_buffer != nullptr;
    display.last_cursor_x = cursor_x;
    display.last_cursor_y = cursor_y;
    display.cursor_valid = (cursor_x < visible_columns) && (cursor_y < visible_rows);
}
}  // namespace

TextDisplayBackend* select_text_display(const BootInfo& boot_info)
{
    if(BootSource::BiosLegacy == boot_info.source)
    {
        debug("console backend: vga")();
        return &g_vga_backend;
    }

    if(initialize_framebuffer_text_display(g_framebuffer_text_display, boot_info.framebuffer))
    {
        debug("framebuffer console active")();
        debug("framebuffer ")(boot_info.framebuffer.width)("x")(boot_info.framebuffer.height)(
            " pitch ")(boot_info.framebuffer.pitch_bytes)(" bpp ")(
            boot_info.framebuffer.bits_per_pixel)(" format ")(
            boot_framebuffer_pixel_format_name(boot_info.framebuffer.pixel_format))();
        return &g_framebuffer_backend;
    }

    if(0 != boot_info.framebuffer.physical_address)
    {
        debug("framebuffer console unavailable")();
    }
    else
    {
        debug("console backend: serial-only")();
    }

    return nullptr;
}

bool initialize_framebuffer_text_display(FramebufferTextDisplay& display,
                                         const BootFramebufferInfo& framebuffer)
{
    display.available = false;
    display.framebuffer = nullptr;
    display.width = framebuffer.width;
    display.height = framebuffer.height;
    display.pitch_bytes = framebuffer.pitch_bytes;
    display.bits_per_pixel = framebuffer.bits_per_pixel;
    display.columns = 0;
    display.rows = 0;
    display.shadow_buffer = nullptr;
    display.shadow_cell_count = 0;
    display.shadow_valid = false;
    display.cursor_valid = false;
    display.pixel_format = framebuffer.pixel_format;

    if((0 == framebuffer.physical_address) || (0 == display.width) || (0 == display.height) ||
       (0 == display.pitch_bytes))
    {
        return false;
    }

    if((BootFramebufferPixelFormat::Rgb != display.pixel_format) &&
       (BootFramebufferPixelFormat::Bgr != display.pixel_format))
    {
        return false;
    }

    if((24 != display.bits_per_pixel) && (32 != display.bits_per_pixel))
    {
        return false;
    }

    display.columns = (uint16_t)(display.width / g_cell_width);
    display.rows = (uint16_t)(display.height / g_cell_height);
    if((0 == display.columns) || (0 == display.rows))
    {
        return false;
    }

    display.framebuffer = kernel_physical_pointer<uint8_t>(framebuffer.physical_address);
    display.available = true;
    framebuffer_clear(display, kFramebufferBackground);

    return true;
}

void present_text_display(const TextDisplayBackend* backend,
                          const uint16_t* buffer,
                          uint16_t columns,
                          uint16_t rows,
                          uint16_t cursor_x,
                          uint16_t cursor_y)
{
    if(nullptr == backend)
    {
        return;
    }

    switch(backend->kind)
    {
        case TextDisplayBackendKind::VgaText:
            present_vga_text(buffer, columns, rows, cursor_x, cursor_y);
            break;
        case TextDisplayBackendKind::FramebufferText:
            present_framebuffer_text(*(FramebufferTextDisplay*)backend->instance,
                                     buffer,
                                     columns,
                                     rows,
                                     cursor_x,
                                     cursor_y);
            break;
        default:
            break;
    }
}

void detach_text_display(const TextDisplayBackend* backend)
{
    if(nullptr == backend)
    {
        return;
    }

    switch(backend->kind)
    {
        case TextDisplayBackendKind::VgaText:
            detach_vga_text();
            break;
        case TextDisplayBackendKind::FramebufferText:
            break;
        default:
            break;
    }
}

void reset_framebuffer_text_display(FramebufferTextDisplay& display)
{
    display.shadow_valid = false;
    display.cursor_valid = false;
    // Text presentation redraws every cell. Clear only pixels outside the grid.
    const uint32_t right = display.columns * g_cell_width;
    const uint32_t bottom = display.rows * g_cell_height;
    for(uint32_t y = 0; y < display.height; ++y)
        for(uint32_t x = y < bottom ? right : 0; x < display.width; ++x)
            framebuffer_set_pixel(display, x, y, kFramebufferBackground);
}

namespace
{
bool same_bytes(const uint8_t* left, const uint8_t* right, unsigned bytes)
{
    for(unsigned i = 0; i < bytes; ++i)
        if(left[i] != right[i])
            return false;
    return true;
}
}  // namespace

void set_text_display_glyph_storage(uint8_t* storage)
{
    g_loaded_glyphs = reinterpret_cast<uint8_t (*)[kGlyphBytesLimit]>(storage);
}

bool text_display_glyph_storage()
{
    return g_loaded_glyphs != nullptr;
}

void framebuffer_text_capacity(const FramebufferTextDisplay& display,
                               uint16_t& columns,
                               uint16_t& rows)
{
    columns = (uint16_t)(display.width / OS1_CONSOLE_CELL_MIN_WIDTH);
    rows = (uint16_t)(display.height / OS1_CONSOLE_CELL_MIN_HEIGHT);
}

void text_display_cell(uint32_t& width, uint32_t& height)
{
    width = g_cell_width;
    height = g_cell_height;
}

bool set_text_display_glyphs(
    uint8_t first, uint8_t count, uint8_t width, uint8_t height, const uint8_t* rows)
{
    auto& display = g_framebuffer_text_display;
    bool grid_changed = false, cell_changed = false;
    if(width != g_cell_width || height != g_cell_height)
    {
        // A new cell size removes each loaded glyph. The caller sends the glyphs from 0.
        g_cell_width = width;
        g_cell_height = height;
        cell_changed = true;
        for(bool& loaded : g_glyph_loaded)
            loaded = false;
        display.shadow_valid = false;
        if(display.width && display.height)
        {
            const uint16_t columns = (uint16_t)(display.width / width);
            const uint16_t rows_count = (uint16_t)(display.height / height);
            grid_changed = columns != display.columns || rows_count != display.rows;
            display.columns = columns;
            display.rows = rows_count;
            display.cursor_valid = false;
        }
    }
    const unsigned bytes = os1::terminal::glyph_bytes(width, height);
    const bool built_in = width == kBuiltInCellWidth && height == kBuiltInCellHeight;
    for(unsigned i = 0; i < count && first + i < 128; ++i)
    {
        const uint8_t* source = rows + i * bytes;
        // A glyph that does not change causes no new presentation.
        bool same = true;
        if(g_glyph_loaded[first + i])
            same = same_bytes(g_loaded_glyphs[first + i], source, bytes);
        else if(built_in)
            same = same_bytes(font8x16[first + i], source, bytes);
        else
            for(unsigned b = 0; b < bytes; ++b)
                same = same && !source[b];
        if(same && (g_glyph_loaded[first + i] || built_in))
            continue;
        memcpy(g_loaded_glyphs[first + i], source, bytes);
        g_glyph_loaded[first + i] = true;
        if(!same)
            display.shadow_valid = false;
    }
    // Clear the pixels outside a smaller grid. The display owner keeps its pixels.
    if(cell_changed && display.available && !display.graphics_owned)
        reset_framebuffer_text_display(display);
    return grid_changed;
}
