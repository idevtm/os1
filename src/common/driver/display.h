#pragma once
#include <os1/display.h>
#include <os1/hardware.h>
#include <stdint.h>

// Interface between the kernel display service and a trusted display driver module.
// The kernel knows this interface only. It has no code for one display device.
#define OS1_DISPLAY_MODULE_ABI 4u

struct os1_framebuffer_surface
{
    volatile uint8_t* pixels;
    uint32_t width, height, pitch, pixel_bytes;
    uint32_t red_shift, green_shift, blue_shift;
};

// One PCI function, as the kernel inventory records it.
struct os1_display_pci
{
    uint16_t vendor, device, segment, command;
    uint8_t class_code, subclass, revision, bus, slot, function;
    uint32_t subsystem;  // Subsystem vendor in the low word, subsystem device in the high word.
    struct
    {
        uint64_t base, bytes;
        bool memory;
    } bars[6];
};

#define OS1_DISPLAY_MAP_WRITE_COMBINE 1u

// Kernel services for a display driver. The functions are valid for the life of the system.
struct os1_display_host
{
    uint32_t version, bytes;
    // Boot framebuffer of the firmware. `pixels` is null on a machine without one.
    os1_framebuffer_surface boot;
    uint64_t boot_physical;
    uint32_t boot_format;
    // A driver that draws is in use. Without it, the kernel offers no mode of a later driver.
    bool drawing;
    uint32_t pci_count;
    bool (*pci)(uint32_t index, os1_display_pci*);
    // Take or return exclusive use of one memory BAR.
    bool (*claim)(uint32_t index, uint32_t bar);
    void (*release)(uint32_t index, uint32_t bar);
    // Map a range of a claimed BAR. Return the address of BAR byte 0, or null. All ranges of one
    // BAR use one linear address space, so the driver adds a BAR offset to the returned address.
    volatile uint8_t* (*map)(
        uint32_t index, uint32_t bar, uint64_t offset, uint64_t bytes, uint32_t flags);
    // Zeroed memory that the driver keeps.
    void* (*allocate)(uint64_t bytes);
    void (*delay_us)(uint64_t);
    // Write one kernel log line: "display: ", the label, then each value as a hexadecimal number.
    void (*log)(const char* label, const uint32_t* values, uint32_t count);
};

// Generic display state that the kernel passes to a driver reflection function.
struct os1_display_summary
{
    bool available;
    const char *driver, *description;
    uint32_t width, height, bits_per_pixel, mode_count;
    uint64_t generation;
    uint64_t framebuffer_address;
    uint32_t framebuffer_pitch, framebuffer_format;
};

enum os1_display_result : uint32_t
{
    OS1_DISPLAY_DONE,
    // The request failed and the display shows what it showed before.
    OS1_DISPLAY_REFUSED,
    // The request failed and the display state is not known. The kernel stops the display service.
    OS1_DISPLAY_LOST,
};

struct os1_display_driver
{
    uint32_t version, bytes;
    // Find the device and prepare the driver. A false result means that the driver has no device.
    bool (*probe)(const os1_display_host*);
    // 0 gives the driver name and 1 gives a description of the display.
    const char* (*text)(uint32_t which);
    // Report one mode of the driver. The boot framebuffer mode is not in this list.
    bool (*mode)(uint32_t index, os1_display_mode*);
    // Show one mode and report its surface.
    os1_display_result (*set_mode)(uint32_t index, os1_framebuffer_surface*);
    // Show the text console again and report its surface. The surface can be a new one.
    os1_display_result (*restore)(os1_framebuffer_surface* console);
    // The next four exports can be null.
    void (*fill)(const os1_framebuffer_surface*,
                 uint32_t x,
                 uint32_t y,
                 uint32_t width,
                 uint32_t height,
                 uint32_t rgb);
    // Run one FILL_AREA, COPY, or BITMAP session request on a surface with 32-bit pixels.
    // The shadow is a copy of the surface in normal memory, or null.
    bool (*draw)(const os1_framebuffer_surface*,
                 uint32_t* shadow,
                 uint32_t member,
                 const uint8_t* payload,
                 uint32_t bytes);
    // Show the hardware pointer with its tip at a surface position, or hide it.
    bool (*cursor)(uint32_t x, uint32_t y, bool visible);
    // Answer a hardware reflection query for the display subtree.
    long (*reflect)(Os1HardwareQuery*, const os1_display_summary*);
};
using os1_display_entry = const os1_display_driver* (*)();
