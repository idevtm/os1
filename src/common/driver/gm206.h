#pragma once
#include <stdint.h>

#define OS1_GM206_MODULE_BASE UINT64_C(0xffffffffa0040000)
#define OS1_GM206_ABI 3u

namespace gm206
{
constexpr unsigned channels = 8, heads = 4, mode_limit = 16;
constexpr unsigned register_bytes = 0x641000;
struct Mode
{
    uint32_t width, height, millihz, pixel_hz;
    uint32_t horizontal_total, vertical_total;
    uint32_t horizontal_sync_start, horizontal_sync_end, vertical_sync_start, vertical_sync_end;
    bool preferred, positive_hsync, positive_vsync;
};
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
struct BootState
{
    uint32_t route, sor_control;
    uint32_t core[sizeof(boot_registers) / sizeof(*boot_registers)];
    uint32_t head[sizeof(head_registers) / sizeof(*head_registers)];
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
struct ProbeIo
{
    void* context;
    uint32_t (*read)(void*, unsigned);
    void (*write)(void*, unsigned, uint32_t);
    void (*delay_us)(uint64_t);
};
struct Exports
{
    uint32_t version, bytes;
    bool (*probe)(const ProbeIo&, Snapshot&);
};
using Entry = const Exports* (*)();
bool probe(const ProbeIo&, Snapshot&);
bool parse_edid(const uint8_t*, unsigned blocks, Output&);
}  // namespace gm206
