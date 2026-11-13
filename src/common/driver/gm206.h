#pragma once
#include <stdint.h>

#include "driver/display.h"

// Internal interface of the GM206 display module. The kernel does not include this file.
// The module exports the generic display driver table of `driver/display.h`.

namespace gm206
{
constexpr unsigned channels = 8, heads = 4, mode_limit = 16;
constexpr unsigned register_bytes = 0x800000;
// BAR 0 ranges that the kernel maps for the module. Each register access must stay inside them.
struct RegisterRange
{
    unsigned offset, bytes;
};
inline constexpr RegisterRange register_ranges[] = {
    {0x000000, 0x2000},
    {0x00d000, 0x1000},
    {0x021000, 0x2000},
    {0x070000, 0x1000},
    {0x110000, 0x4000},
    {0x610000, 0x3000},
    {0x616000, 0x2000},
    {0x619000, 0x1000},
    {0x61a000, 0x2000},
    {0x61c000, 0x4000},
    {0x630000, 0x4000},
    {0x640000, 0x1000 + heads * 0x1000},
    {0x64d000, heads * 0x1000},
    {0x660000, 0x1000 + heads * 0x1000},
    {0x700000, 0x100000},
};
struct Mode
{
    uint32_t width, height, millihz, pixel_hz;
    uint32_t horizontal_total, vertical_total;
    uint32_t horizontal_sync_start, horizontal_sync_end, vertical_sync_start, vertical_sync_end;
    bool preferred, positive_hsync, positive_vsync;
};
struct HeadTiming
{
    uint32_t raster, sync_end, blank_end, blank_start, pixel_hz;
};
bool prepare_head_timing(const Mode&, HeadTiming&);

struct Output
{
    uint8_t channel, dpcd_revision, max_link_rate, max_lanes;
    uint8_t edid_blocks, mode_count;
    uint8_t edid_read_blocks;
    char manufacturer[4], name[14], serial_text[14];
    uint8_t edid[256];
    Mode modes[mode_limit];
};
struct Head
{
    uint32_t horizontal_total, vertical_total, pixel_hz;
    uint8_t index, sor, link;
};
// Reference Gigabyte board, DCB output 2 and CCB AUX 1.
constexpr unsigned board_aux = 1, board_route = 0x612488;
struct BootRegister
{
    const char* name;
    unsigned offset;
};
inline constexpr BootRegister boot_registers[] = {
    {"display_instance", 0x610010},
    {"supervisor_status", 0x6100ac},
    {"core_channel_control", 0x610490},
    {"core_channel_push", 0x610494},
    {"core_channel_config", 0x610498},
    {"core_channel_target", 0x61049c},
    {"core_channel_put", 0x640000},
    // Ownership and memory layout inputs. The probe only reads them.
    {"vbios_display_owner", 0x6194e8},
    {"pramin_window", 0x001700},
    {"bar1_instance", 0x001704},
    {"bar2_instance", 0x001714},
    {"vram_fbpa_disable", 0x021c14},
    {"vram_fbp_disable", 0x021d38},
    {"vram_fbp_count", 0x022438},
    {"vram_fbpa_per_fbp", 0x022458},
    {"vram_fbpa0_mib", 0x11020c},
    {"vram_fbpa1_mib", 0x11120c},
    {"vram_fbpa2_mib", 0x11220c},
    {"vram_fbpa3_mib", 0x11320c},
};
inline constexpr BootRegister head_registers[] = {
    {"head_output_control", 0x640404},
    {"head_control", 0x640408},
    {"head_raster_total", 0x640414},
    {"head_sync_end", 0x640418},
    {"head_blank_end", 0x64041c},
    {"head_blank_start", 0x640420},
    {"head_pixel_hz", 0x640450},
    {"head_surface_offset_raw", 0x640460},
    {"head_surface_size_raw", 0x640468},
    {"head_surface_storage_raw", 0x64046c},
    {"head_surface_params_raw", 0x640470},
    {"head_surface_context_raw", 0x640474},
    {"head_viewport_point_in_raw", 0x6404b0},
    {"head_viewport_in_raw", 0x6404b8},
    {"head_viewport_point_out_adjust_raw", 0x6404bc},
    {"head_viewport_out_raw", 0x6404c0},
    {"head_viewport_out_min_raw", 0x6404c4},
    {"head_viewport_out_max_raw", 0x6404c8},
};
// Base channel of one head. Control registers and user areas use different strides.
struct ChannelRegister
{
    const char* name;
    unsigned offset, stride;
};
inline constexpr ChannelRegister base_registers[] = {
    {"base_channel_control", 0x6104a0, 0x10},
    {"base_channel_push", 0x6104a4, 0x10},
    {"base_channel_config", 0x6104a8, 0x10},
    {"base_channel_target", 0x6104ac, 0x10},
    {"base_channel_put", 0x641000, 0x1000},
    // Nouveau reports the 0x64xxxx copy as the previous state and 0x66xxxx as the next state.
    {"base_context_raw", 0x6410c0, 0x1000},
    {"base_surface_offset_raw", 0x641400, 0x1000},
    {"base_surface_size_raw", 0x641408, 0x1000},
    {"base_surface_storage_raw", 0x64140c, 0x1000},
    {"base_surface_params_raw", 0x641410, 0x1000},
    {"base_context_next_raw", 0x6610c0, 0x1000},
    {"base_surface_offset_next_raw", 0x661400, 0x1000},
    {"base_surface_size_next_raw", 0x661408, 0x1000},
    {"base_surface_storage_next_raw", 0x66140c, 0x1000},
    {"base_surface_params_next_raw", 0x661410, 0x1000},
};
// Index of `head_viewport_out_raw` in `head_registers`.
inline constexpr unsigned head_viewport_out = 15;
struct BootState
{
    uint32_t route, sor_control;
    uint32_t core[sizeof(boot_registers) / sizeof(*boot_registers)];
    uint32_t head[sizeof(head_registers) / sizeof(*head_registers)];
    uint32_t base[sizeof(base_registers) / sizeof(*base_registers)];
    uint8_t sor, link, head_index;
    bool route_matched, link_config_read, link_status_read;
    uint8_t link_config[2], link_status[6];
};
struct Snapshot
{
    unsigned output_count, head_count;
    uint32_t boot0, resources;
    uint8_t aux_present_mask, aux_enabled_mask, dpcd_failed_mask;
    Output outputs[channels];
    Head scanouts[heads];
    BootState boot;
};
// One linear 32-bit surface in video memory. The offset counts bytes from the start of video
// memory.
struct Surface
{
    uint32_t offset, width, height, pitch;
};
// Video memory that the kernel reserves for the scanout path. BAR 1 must expose it without
// translation.
struct ScanoutLayout
{
    uint32_t instance, push;        // 64 KiB display instance block and 4 KiB command page.
    uint32_t check;                 // Offset of one word that the module uses to check BAR 1.
    volatile uint32_t* check_word;  // The same word through the BAR 1 mapping.
    uint32_t cursor;                // 16 KiB for the pointer image, 64 x 64 pixels.
};
enum class ScanoutStage : uint8_t
{
    Idle,
    Precondition,  // Boot state does not match the supported route.
    Aperture,      // BAR 1 does not expose video memory without translation.
    Memory,        // The instance block write did not complete.
    Ownership,     // Firmware did not release the display. The display state is unknown from here.
    Channel,
    Command,  // The display engine reported a command error.
    Update,
    Ready,
};
struct ScanoutState
{
    ScanoutStage stage;
    bool owned;      // The module owns the core channel.
    bool confirmed;  // The armed head state shows the last surface.
    uint8_t head;
    uint8_t stages;  // Supervisor stages of the last call, one bit for each stage.
    // Hardware pointer: the image is in video memory, the position channel runs, the head shows it.
    bool cursor_image, cursor_channel, cursor_visible;
    Surface surface;    // The surface that the head shows.
    uint32_t put;       // Next free byte in the command page.
    uint32_t restored;  // Firmware method values that the first update sent again.
    // One bit for each of these methods. Bit 0 is the SOR control. The other bits follow the
    // order of the kept head methods.
    uint32_t restored_mask;
    uint32_t supervisor_requests, supervisor_head;
    uint32_t supervisor_stage[3];  // Last head request of each supervisor stage.
    // Engine polls of the last call, and the poll that acknowledged each supervisor stage.
    // One poll takes a minimum of 100 microseconds.
    uint32_t polls, stage_poll[3];
    // Words of the first command error of the last call, and the number of errors in that call.
    uint32_t error[3], errors, channel_control, armed_context, armed_offset;
    // The engine refused only the UPDATE of the last call. The head keeps its surface, and the
    // next call can use the channel.
    bool recoverable;
};
struct ProbeIo
{
    void* context;
    uint32_t (*read)(void*, unsigned);
    void (*write)(void*, unsigned, uint32_t);
    void (*delay_us)(uint64_t);
};
bool probe(const ProbeIo&, Snapshot&);
// Show one surface on the boot head with the firmware timing. The first call takes the display from
// firmware.
bool scanout(const ProbeIo&, const Snapshot&, const ScanoutLayout&, const Surface&, ScanoutState&);
// Show the hardware pointer with its tip at a surface position, or hide it. The head must show a
// surface. Each scanout call hides the pointer.
bool cursor(const ProbeIo&,
            const Snapshot&,
            const ScanoutLayout&,
            ScanoutState&,
            uint32_t x,
            uint32_t y,
            bool visible);
bool parse_edid(const uint8_t*, unsigned blocks, Output&);

// Video memory that the module reserves in BAR 1. BAR 1 must have `aperture_bytes` bytes.
inline constexpr uint32_t native_offset = 0x04000000, console_offset = 0x06000000;
inline constexpr uint32_t instance_offset = 0x06400000, push_offset = 0x06410000;
inline constexpr uint32_t check_offset = 0x06411000, cursor_offset = 0x06412000;
inline constexpr uint32_t aperture_bytes = 0x06416000;

// Write the modes that the module can show: the native mode, then each smaller monitor mode as a
// scaled surface. Return the number of modes. Zero means that the boot state has no support.
unsigned offer_modes(const Snapshot&,
                     const os1_framebuffer_surface& boot,
                     os1_display_mode* modes,
                     unsigned capacity);

#if defined(OS1_HOST_TEST)
// A host test supplies the register access of the module driver table.
inline const ProbeIo* test_io = nullptr;
#endif

// PCI identity and probe result for the reflection tree.
struct Identity
{
    const char* status = "no matching PCI device";
    uint32_t vendor{}, device{}, subsystem_vendor{}, subsystem_device{}, revision{};
    uint32_t segment{}, bus{}, slot{}, function{};
    uint64_t bar0_bytes{};
    uint32_t boot0{};
    bool boot0_read{};
};
// Answer one query for the display subtree. The snapshot and the identity can be null.
long reflect(Os1HardwareQuery&, const os1_display_summary&, const Snapshot*, const Identity*);
}  // namespace gm206
