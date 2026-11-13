#pragma once
#include <stdint.h>

#define OS1_GM206_MODULE_BASE UINT64_C(0xffffffffa0040000)
#define OS1_GM206_ABI 2u

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
struct Snapshot
{
    unsigned output_count, head_count;
    uint32_t boot0, resources;
    uint8_t aux_present_mask, aux_enabled_mask, dpcd_failed_mask;
    Output outputs[channels];
    Head scanouts[heads];
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
