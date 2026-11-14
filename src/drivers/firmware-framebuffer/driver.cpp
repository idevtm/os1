#include "driver/display.h"
#include "driver/draw.hpp"

namespace
{
bool usable(const os1_framebuffer_surface* s)
{
    return s->pixels && s->width && s->height && (s->pixel_bytes == 3 || s->pixel_bytes == 4) &&
           s->width <= s->pitch / s->pixel_bytes &&
           ((s->red_shift == 16 && s->blue_shift == 0) ||
            (s->red_shift == 0 && s->blue_shift == 16)) &&
           s->green_shift == 8;
}

void fill(const os1_framebuffer_surface* s,
          uint32_t x,
          uint32_t y,
          uint32_t width,
          uint32_t height,
          uint32_t rgb)
{
    const uint32_t color = (((rgb >> 16) & 255) << s->red_shift) |
                           (((rgb >> 8) & 255) << s->green_shift) | ((rgb & 255) << s->blue_shift);
    for(uint32_t row = 0; row < height; ++row)
    {
        auto* pixel = s->pixels + uint64_t(y + row) * s->pitch + uint64_t(x) * s->pixel_bytes;
        for(uint32_t col = 0; col < width; ++col, pixel += s->pixel_bytes)
            for(uint32_t byte = 0; byte < s->pixel_bytes; ++byte)
                pixel[byte] = uint8_t(color >> (8 * byte));
    }
}
// Standard VGA BAR 2 exposes the Bochs DISPI register file.
enum Register
{
    Id,
    Xres,
    Yres,
    Bpp,
    Enable,
    Bank,
    VirtualWidth,
    VirtualHeight,
    Xoff,
    Yoff
};
constexpr uint16_t enabled_lfb = 0x41;
struct BochsMode
{
    uint16_t width, height, bpp, enable, virtual_width, x, y;
};
bool read_mode(volatile uint16_t* regs, BochsMode* mode)
{
    const auto id = regs[Id];
    if(id < 0xb0c0 || id > 0xb0c5 || (regs[Enable] & enabled_lfb) != enabled_lfb)
        return false;
    *mode = {regs[Xres],
             regs[Yres],
             regs[Bpp],
             regs[Enable],
             regs[VirtualWidth],
             regs[Xoff],
             regs[Yoff]};
    return true;
}
bool write_mode(volatile uint16_t* regs, const BochsMode* mode)
{
    regs[Enable] = 0;
    regs[Xres] = mode->width;
    regs[Yres] = mode->height;
    regs[Bpp] = mode->bpp;
    regs[Enable] = mode->enable | 0x80;  // Keep the framebuffer contents during the switch.
    regs[VirtualWidth] = mode->virtual_width;
    regs[Xoff] = mode->x;
    regs[Yoff] = mode->y;
    BochsMode active{};
    return read_mode(regs, &active) && active.width == mode->width &&
           active.height == mode->height && active.bpp == mode->bpp &&
           active.virtual_width == mode->virtual_width && active.x == mode->x &&
           active.y == mode->y;
}

// Driver state. The boot surface is the firmware framebuffer.
os1_framebuffer_surface boot;
volatile uint16_t* bochs;
// The mode of the firmware, and the mode that the last successful request set.
BochsMode boot_mode, active_mode;
os1_display_mode alternatives[2];
unsigned alternative_count;

// Find the QEMU Standard VGA device that owns the boot framebuffer. It can change its mode.
void find_bochs(const os1_display_host* host)
{
    constexpr uint64_t largest = 1280 * 720 * 4;
    for(uint32_t index = 0; index < host->pci_count; ++index)
    {
        os1_display_pci pci{};
        if(!host->pci(index, &pci) || pci.vendor != 0x1234 || pci.device != 0x1111 ||
           pci.class_code != 3 || pci.bars[0].base != host->boot_physical || !pci.bars[0].memory ||
           !pci.bars[2].memory || pci.bars[2].bytes < 0x516 || pci.bars[0].bytes < largest ||
           boot.pixel_bytes != 4 || boot.red_shift != 16)
            continue;
        const uint64_t span = uint64_t(boot.pitch) * boot.height;
        const uint64_t length = span > largest ? span : largest;
        if(length > pci.bars[0].bytes || !host->claim(index, 0))
            return;
        if(host->claim(index, 2))
        {
            volatile uint8_t* registers = nullptr;
            if(host->map(index, 0, 0, length, 0) &&
               (registers = host->map(index, 2, 0, 0x516, 0)) != nullptr)
            {
                auto* candidate = reinterpret_cast<volatile uint16_t*>(registers + 0x500);
                if(read_mode(candidate, &boot_mode) && boot_mode.width == boot.width &&
                   boot_mode.height == boot.height && boot_mode.bpp == 32 &&
                   uint32_t(boot_mode.virtual_width) * 4 == boot.pitch && !boot_mode.x &&
                   !boot_mode.y)
                {
                    bochs = candidate;
                    active_mode = boot_mode;
                    constexpr os1_display_mode sizes[] = {{0, 800, 600, 32}, {0, 1280, 720, 32}};
                    for(const auto& size : sizes)
                        if(size.width != boot.width || size.height != boot.height)
                            alternatives[alternative_count++] = size;
                    return;
                }
            }
            host->release(index, 2);
        }
        host->release(index, 0);
        return;
    }
}

bool probe(const os1_display_host* host)
{
    boot = host->boot;
    if(!usable(&boot))
        return false;
    find_bochs(host);
    return true;
}

const char* text(uint32_t which)
{
    if(which)
        return bochs ? "QEMU runtime display" : "Fixed firmware display";
    return bochs ? "bochs-display" : "firmware-framebuffer";
}

bool mode(uint32_t index, os1_display_mode* out)
{
    if(index >= alternative_count)
        return false;
    *out = alternatives[index];
    return true;
}

os1_display_result set_mode(uint32_t index, os1_framebuffer_surface* surface)
{
    if(index >= alternative_count)
        return OS1_DISPLAY_REFUSED;
    const auto& wanted = alternatives[index];
    const BochsMode desired{uint16_t(wanted.width),
                            uint16_t(wanted.height),
                            32,
                            enabled_lfb,
                            uint16_t(wanted.width),
                            0,
                            0};
    if(!write_mode(bochs, &desired))
        // Return to the mode before the request. If this also fails, the display state is not
        // known.
        return write_mode(bochs, &active_mode) ? OS1_DISPLAY_REFUSED : OS1_DISPLAY_LOST;
    active_mode = desired;
    *surface = boot;
    surface->width = wanted.width;
    surface->height = wanted.height;
    surface->pitch = wanted.width * 4;
    return OS1_DISPLAY_DONE;
}

os1_display_result restore(os1_framebuffer_surface* console)
{
    *console = boot;
    if(!bochs)
        return OS1_DISPLAY_DONE;
    if(!write_mode(bochs, &boot_mode))
        return OS1_DISPLAY_LOST;
    active_mode = boot_mode;
    return OS1_DISPLAY_DONE;
}

const os1_display_driver driver{OS1_DISPLAY_MODULE_ABI,
                                sizeof(driver),
                                probe,
                                text,
                                mode,
                                set_mode,
                                restore,
                                fill,
                                display_draw::draw,
                                nullptr,
                                nullptr,
                                nullptr};
}  // namespace

extern "C" const os1_display_driver* os1_driver_init()
{
    return &driver;
}
