#include "driver/gm206.h"

namespace gm206
{
namespace
{
bool block_valid(const uint8_t* block)
{
    uint8_t sum = 0;
    for(unsigned i = 0; i < 128; ++i)
        sum += block[i];
    return sum == 0;
}

void add_mode(Output& out, const Mode& mode)
{
    HeadTiming timing{};
    if(!prepare_head_timing(mode, timing))
        return;
    for(unsigned i = 0; i < out.mode_count; ++i)
    {
        const auto& old = out.modes[i];
        if(old.width == mode.width && old.height == mode.height && old.pixel_hz == mode.pixel_hz &&
           old.horizontal_total == mode.horizontal_total &&
           old.vertical_total == mode.vertical_total &&
           old.horizontal_sync_start == mode.horizontal_sync_start &&
           old.horizontal_sync_end == mode.horizontal_sync_end &&
           old.vertical_sync_start == mode.vertical_sync_start &&
           old.vertical_sync_end == mode.vertical_sync_end &&
           old.positive_hsync == mode.positive_hsync && old.positive_vsync == mode.positive_vsync)
            return;
    }
    if(out.mode_count < mode_limit)
        out.modes[out.mode_count++] = mode;
}

void detailed_timing(Output& out, const uint8_t* d, bool preferred)
{
    const unsigned pixel_hz = (d[0] + 256u * d[1]) * 10'000;
    // This profile represents progressive timings with separate digital sync.
    if(!pixel_hz || (d[17] & 0x98) != 0x18)
        return;
    const unsigned width = d[2] + ((d[4] & 0xf0) << 4);
    const unsigned height = d[5] + ((d[7] & 0xf0) << 4);
    const unsigned hblank = d[3] + ((d[4] & 15) << 8);
    const unsigned vblank = d[6] + ((d[7] & 15) << 8);
    const unsigned hfront = d[8] + ((d[11] & 0xc0) << 2);
    const unsigned hsync = d[9] + ((d[11] & 0x30) << 4);
    const unsigned vfront = (d[10] >> 4) + ((d[11] & 0x0c) << 2);
    const unsigned vsync = (d[10] & 15) + ((d[11] & 3) << 4);
    if(!width || !height || !hsync || !vsync || hfront + hsync > hblank || vfront + vsync > vblank)
        return;
    const unsigned htotal = width + hblank, vtotal = height + vblank;
    const auto millihz =
        uint32_t((uint64_t(pixel_hz) * 1000 + htotal * vtotal / 2) / (htotal * vtotal));
    add_mode(out,
             {width,
              height,
              millihz,
              pixel_hz,
              htotal,
              vtotal,
              width + hfront,
              width + hfront + hsync,
              height + vfront,
              height + vfront + vsync,
              preferred,
              bool(d[17] & 2),
              bool(d[17] & 4)});
}

void cta_video(Output& out, unsigned code)
{
    // CTA VICs 4 and 16 cover the initial qualification candidates.
    // CTA permits both refresh families for these codes.
    if(code == 4)
    {
        add_mode(out,
                 {1280, 720, 60000, 74250000, 1650, 750, 1390, 1430, 725, 730, false, true, true});
        add_mode(out,
                 {1280, 720, 59940, 74175824, 1650, 750, 1390, 1430, 725, 730, false, true, true});
    }
    else if(code == 16)
    {
        add_mode(
            out,
            {1920, 1080, 60000, 148500000, 2200, 1125, 2008, 2052, 1084, 1089, false, true, true});
        add_mode(
            out,
            {1920, 1080, 59940, 148351648, 2200, 1125, 2008, 2052, 1084, 1089, false, true, true});
    }
}

class Aux
{
    const ProbeIo& io;
    const unsigned base;
    static constexpr unsigned request = 0x10000, reply_mask = 0x07000000;
    static constexpr unsigned control = 0xd954, status = 0xd958, address = 0xd950;
    uint32_t read(unsigned reg) const
    {
        return io.read(io.context, reg + base);
    }
    void write(unsigned reg, uint32_t value) const
    {
        io.write(io.context, reg + base, value);
    }

    bool wait(unsigned mask, unsigned value, unsigned timeout) const
    {
        for(unsigned i = 0; i < timeout; ++i)
        {
            if((read(control) & mask) == value)
                return true;
            io.delay_us(1);
        }
        return false;
    }

public:
    Aux(const ProbeIo& io, unsigned channel) : io(io), base(channel * 0x50) {}

    bool present() const
    {
        const auto state = read(status);
        return state != UINT32_MAX && (state & 0x10000000);
    }

    bool enabled() const
    {
        return (read(0xd970) & 0xc003) == 2 && !(read(0xd97c) & 1);
    }

    int transfer(unsigned command, unsigned target, uint8_t* data, unsigned size) const
    {
        if(!wait(reply_mask | request, 0, 1000))
            return -1;
        const auto original = read(control);
        const auto autodpcd = read(0xd968);
        const auto old_address = read(address);
        const auto selected = (original & ~0x0071f1ffu) | 0x00100000;
        write(control, selected);
        int transferred = -1;
        if(wait(reply_mask, 0x01000000, 1000))
        {
            write(0xd968, autodpcd & ~0x10000u);
            auto operation = selected | (command << 12) | (size ? size - 1 : 0x100);
            const auto tx = !(command & 1) && size;
            const auto old_data = tx ? read(0xd930) : 0;
            if(tx)
                write(0xd930, data[0]);
            write(address, target);
            for(unsigned attempt = 0; attempt < 4; ++attempt)
            {
                write(control, operation | 0x80000000);
                write(control, operation);
                write(control, operation | request);
                if(!wait(request, 0, 2000))
                    break;
                const auto result = read(status);
                const auto reply = (result >> 16) & 15;
                if(result & 0xf00)
                    break;
                if(reply == 2 || reply == 8)
                {
                    if(reply == 8 && command == 4)
                        operation = (operation & ~0xf000u) | (6u << 12);
                    io.delay_us(400);
                    continue;
                }
                if(reply != 0 || !(result & 0x10000000))
                    break;
                const unsigned count = (command & 1) && size ? result & 31 : size;
                if(count > size || (size && !count))
                    break;
                transferred = int(count);
                if(command & 1)
                    for(unsigned i = 0; i < count; i += 4)
                    {
                        const auto word = read(0xd940 + i);
                        for(unsigned byte = 0; byte < 4 && i + byte < count; ++byte)
                            data[i + byte] = uint8_t(word >> (8 * byte));
                    }
                break;
            }
            if(read(control) & request)
                write(control, operation | 0x80000000);
            if(tx)
                write(0xd930, old_data);
            write(address, old_address);
            write(0xd968, autodpcd);
        }
        write(control, original & ~reply_mask);
        return transferred;
    }

    bool edid_block(unsigned block, uint8_t* data) const
    {
        // EDID byte offset only. No sink capability or link register writes.
        uint8_t offset = uint8_t(128 * block);
        bool ok = transfer(4, 0x50, &offset, 1) == 1;
        for(unsigned i = 0; ok && i < 128;)
        {
            const unsigned length = 128 - i < 16 ? 128 - i : 16;
            const int received = transfer(5, 0x50, data + i, length);
            ok = received > 0;
            if(ok)
                i += unsigned(received);
        }
        const bool stopped = transfer(1, 0x50, nullptr, 0) == 0;
        return ok && stopped;
    }
};
}  // namespace

bool prepare_head_timing(const Mode& mode, HeadTiming& timing)
{
    const auto valid_axis = [](uint32_t visible, uint32_t start, uint32_t end, uint32_t total) {
        return visible && visible <= start && start < end && end <= total && total <= 0x7fffu;
    };
    if(!mode.pixel_hz || mode.pixel_hz > 0x7fffffffu ||
       !valid_axis(mode.width,
                   mode.horizontal_sync_start,
                   mode.horizontal_sync_end,
                   mode.horizontal_total) ||
       !valid_axis(
           mode.height, mode.vertical_sync_start, mode.vertical_sync_end, mode.vertical_total))
        return false;

    const auto pack = [](uint32_t h, uint32_t v) { return h | (v << 16); };
    const auto horizontal_blank_end = mode.horizontal_total - mode.horizontal_sync_start - 1;
    const auto vertical_blank_end = mode.vertical_total - mode.vertical_sync_start - 1;
    timing = {
        pack(mode.horizontal_total, mode.vertical_total),
        pack(mode.horizontal_sync_end - mode.horizontal_sync_start - 1,
             mode.vertical_sync_end - mode.vertical_sync_start - 1),
        pack(horizontal_blank_end, vertical_blank_end),
        pack(horizontal_blank_end + mode.width, vertical_blank_end + mode.height),
        mode.pixel_hz,
    };
    return true;
}

bool parse_edid(const uint8_t* data, unsigned blocks, Output& out)
{
    out.mode_count = out.edid_blocks = 0;
    constexpr uint8_t header[] = {0, 255, 255, 255, 255, 255, 255, 0};
    out.edid_read_blocks = 0;
    out.manufacturer[0] = out.name[0] = out.serial_text[0] = 0;
    if(!blocks || blocks > 2)
        return false;
    out.edid_read_blocks = uint8_t(blocks);
    for(unsigned i = 0; i < blocks * 128; ++i)
        out.edid[i] = data[i];
    if(!block_valid(data))
        return false;
    for(unsigned i = 0; i < sizeof(header); ++i)
        if(data[i] != header[i])
            return false;
    out.edid_blocks = 1;
    const unsigned vendor = (unsigned(data[8]) << 8) | data[9];
    bool valid_vendor = !(vendor & 0x8000);
    for(unsigned i = 0; i < 3; ++i)
    {
        const unsigned letter = (vendor >> (10 - i * 5)) & 31;
        valid_vendor &= letter >= 1 && letter <= 26;
        out.manufacturer[i] = char('A' + letter - 1);
    }
    out.manufacturer[valid_vendor ? 3 : 0] = 0;
    for(unsigned offset = 54; offset < 126; offset += 18)
    {
        const auto* descriptor = data + offset;
        if(descriptor[0] || descriptor[1] || descriptor[2] || descriptor[4])
            continue;
        char* text = descriptor[3] == 0xfc   ? out.name
                     : descriptor[3] == 0xff ? out.serial_text
                                             : nullptr;
        if(!text)
            continue;
        unsigned i = 0;
        for(; i < 13 && descriptor[5 + i] != '\n'; ++i)
        {
            const auto c = descriptor[5 + i];
            text[i] = c >= 32 && c < 127 ? char(c) : '?';
        }
        while(i && text[i - 1] == ' ')
            --i;
        text[i] = 0;
    }
    for(unsigned offset = 54; offset < 126; offset += 18)
        detailed_timing(out, data + offset, offset == 54 && (data[24] & 2));
    if(blocks == 2 && data[126] && block_valid(data + 128))
    {
        ++out.edid_blocks;
        const auto* cta = data + 128;
        const unsigned end = cta[2];
        if(cta[0] == 2 && cta[1] >= 3 && end >= 4 && end <= 127)
        {
            for(unsigned offset = 4; offset < end;)
            {
                const auto tag = cta[offset++];
                const unsigned size = tag & 31;
                if(size > end - offset)
                    break;
                if((tag >> 5) == 2)
                    for(unsigned i = 0; i < size; ++i)
                        cta_video(out, cta[offset + i] & 127);
                offset += size;
            }
            for(unsigned offset = end; offset + 18 <= 127; offset += 18)
                detailed_timing(out, cta + offset, false);
        }
    }
    return true;
}

bool probe(const ProbeIo& io, Snapshot& out)
{
    out.boot0 = io.read(io.context, 0);
    if(((out.boot0 >> 20) & 0x1ff) != 0x126)
        return false;
    out.output_count = out.head_count = 0;
    out.aux_present_mask = out.aux_enabled_mask = out.dpcd_failed_mask = 0;
    const auto resources = out.resources = io.read(io.context, 0x612004);
    for(unsigned sor = 0; sor < 8 && out.head_count < heads; ++sor)
    {
        if(!(resources & (0x100u << sor)))
            continue;
        const auto route = io.read(io.context, 0x640200 + sor * 0x20);
        const unsigned protocol = (route >> 8) & 15, mask = route & 15;
        if((protocol != 8 && protocol != 9) || !mask || (mask & (mask - 1)) || !(resources & mask))
            continue;
        unsigned head = 0;
        while(!(mask & (1u << head)))
            ++head;
        const auto totals = io.read(io.context, 0x640414 + head * 0x300);
        const auto clock = io.read(io.context, 0x640450 + head * 0x300);
        if((totals & 0xffff) && (totals >> 16) && clock && clock != UINT32_MAX)
            out.scanouts[out.head_count++] = {totals & 0xffff,
                                              totals >> 16,
                                              clock,
                                              uint8_t(head),
                                              uint8_t(sor),
                                              uint8_t(protocol - 7)};
    }
    for(unsigned ch = 0; ch < channels; ++ch)
    {
        const Aux aux(io, ch);
        if(aux.enabled())
            out.aux_enabled_mask |= uint8_t(1u << ch);
        if(!aux.present())
            continue;
        out.aux_present_mask |= uint8_t(1u << ch);
        if(!(out.aux_enabled_mask & (1u << ch)))
            continue;
        uint8_t caps[3];
        if(aux.transfer(9, 0, caps, sizeof(caps)) != int(sizeof(caps)) || caps[0] < 0x10 ||
           !caps[1] || !(caps[2] & 31))
        {
            out.dpcd_failed_mask |= uint8_t(1u << ch);
            continue;
        }
        auto& output = out.outputs[out.output_count++];
        output.edid_read_blocks = output.edid_blocks = output.mode_count = 0;
        output.channel = uint8_t(ch);
        output.dpcd_revision = caps[0];
        output.max_link_rate = caps[1];
        output.max_lanes = caps[2] & 31;
        uint8_t edid[256];
        if(aux.edid_block(0, edid))
        {
            const unsigned blocks = edid[126] && aux.edid_block(1, edid + 128) ? 2 : 1;
            (void)parse_edid(edid, blocks, output);
        }
    }
    auto& boot = out.boot;
    boot = {};
    boot.route = io.read(io.context, board_route);
    for(unsigned i = 0; i < sizeof(boot_registers) / sizeof(*boot_registers); ++i)
        boot.core[i] = io.read(io.context, boot_registers[i].offset);
    const unsigned selector = boot.route & 15;
    if(boot.route != UINT32_MAX && selector && selector <= 8)
    {
        boot.sor = uint8_t(selector - 1);
        boot.link = uint8_t(((boot.route >> 4) & 1) + 1);
        boot.sor_control = io.read(io.context, 0x640200 + boot.sor * 0x20);
        for(unsigned i = 0; i < out.head_count; ++i)
        {
            const auto& scanout = out.scanouts[i];
            if(scanout.sor != boot.sor || scanout.link != boot.link)
                continue;
            boot.route_matched = true;
            boot.head_index = scanout.index;
            for(unsigned reg = 0; reg < sizeof(head_registers) / sizeof(*head_registers); ++reg)
                boot.head[reg] =
                    io.read(io.context, head_registers[reg].offset + boot.head_index * 0x300);
            for(unsigned reg = 0; reg < sizeof(base_registers) / sizeof(*base_registers); ++reg)
                boot.base[reg] = io.read(
                    io.context,
                    base_registers[reg].offset + boot.head_index * base_registers[reg].stride);
            break;
        }
    }
    if(out.aux_present_mask & out.aux_enabled_mask & (1u << board_aux))
    {
        const Aux aux(io, board_aux);
        boot.link_config_read =
            aux.transfer(9, 0x100, boot.link_config, sizeof(boot.link_config)) ==
            int(sizeof(boot.link_config));
        boot.link_status_read =
            aux.transfer(9, 0x202, boot.link_status, sizeof(boot.link_status)) ==
            int(sizeof(boot.link_status));
    }
    return true;
}

namespace
{
// Register access for the display engine. Nouveau gf119 and gm200 are the reference.
struct Engine
{
    const ProbeIo& io;
    uint32_t rd(unsigned reg) const
    {
        return io.read(io.context, reg);
    }
    void wr(unsigned reg, uint32_t value) const
    {
        io.write(io.context, reg, value);
    }
    // Wait at most two seconds.
    bool wait(unsigned reg, uint32_t mask, uint32_t value) const
    {
        for(unsigned i = 0; i < 20000; ++i)
        {
            if((rd(reg) & mask) == value)
                return true;
            io.delay_us(100);
        }
        return false;
    }
    // Video memory through the PRAMIN window in BAR 0.
    unsigned window(uint32_t address) const
    {
        // Nouveau sets the window in 1 MiB steps.
        wr(0x001700, (address & ~0xfffffu) >> 16);
        return 0x700000 + (address & 0xfffff);
    }
    uint32_t memory(uint32_t address) const
    {
        return rd(window(address));
    }
    void memory(uint32_t address, uint32_t value) const
    {
        wr(window(address), value);
    }
    bool flush() const
    {
        wr(0x070000, 1);
        return wait(0x070000, 2, 0);
    }
    unsigned head_mask() const
    {
        return rd(0x612004) & 0xf;
    }
};

constexpr uint32_t memory_handle = 0xf0000001;
constexpr unsigned command_limit = 0xf00, instance_object = 0x1000;
// Head methods that the first update copies from the active firmware state: output resource,
// control, raster, and pixel clock. The offset is relative to the first method of the head.
// A new core channel does not hold all of them as pending state. The module sends no other
// firmware value. Offsets without a method show values that are not method values.
inline constexpr unsigned kept_head_methods[] = {
    0x04, 0x08, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x2c, 0x50, 0x54, 0x58};
constexpr unsigned kept_head_words = sizeof(kept_head_methods) / sizeof(*kept_head_methods);
// Entry 0 is the control method of the boot SOR.
constexpr unsigned kept_words = 1 + kept_head_words;
constexpr unsigned pixel_clock = 0x50, pixel_clock_config = 0x54, pixel_clock_max = 0x58;
constexpr unsigned kept_method(unsigned index, unsigned head, unsigned sor)
{
    return index ? 0x400 + head + kept_head_methods[index - 1] : 0x200 + sor * 0x20;
}

unsigned boot_index(const char* name)
{
    for(unsigned i = 0; i < sizeof(boot_registers) / sizeof(*boot_registers); ++i)
    {
        const char *left = boot_registers[i].name, *right = name;
        while(*left && *left == *right)
            ++left, ++right;
        if(*left == *right)
            return i;
    }
    return 0;
}

// Sum the enabled memory partitions. Return zero if the size is not usable.
uint64_t memory_bytes(const BootState& boot)
{
    const uint64_t parts = uint64_t(boot.core[boot_index("vram_fbp_count")]) *
                           boot.core[boot_index("vram_fbpa_per_fbp")];
    if(!parts || parts > 4)
        return 0;
    uint64_t mib = 0;
    for(unsigned i = 0; i < parts; ++i)
    {
        const uint32_t part = boot.core[boot_index("vram_fbpa0_mib") + i];
        if(!part || part > 16384)
            return 0;
        mib += part;
    }
    return mib << 20;
}

bool take_display(const Engine& e, const ScanoutLayout& layout, uint64_t bytes, ScanoutState& state)
{
    // Check BAR 1 in both directions before any display register changes.
    state.stage = ScanoutStage::Aperture;
    constexpr uint32_t pattern = 0x6d32064f;
    // The first write uses PRAMIN. BAR 1 gets one write only after the read direction is correct.
    e.memory(layout.check, pattern);
    if(!e.flush() || *layout.check_word != pattern)
        return false;
    *layout.check_word = ~pattern;
    if(!e.flush() || e.memory(layout.check) != ~pattern)
        return false;

    // Instance block: an empty hash table, one memory object, and one hash entry for its handle.
    state.stage = ScanoutStage::Memory;
    for(unsigned offset = 0; offset < 0x1000; offset += 4)
        e.memory(layout.instance + offset, 0);
    const uint32_t object[] = {0x49, 0, uint32_t((bytes - 1) >> 8), 0, 0, 0};
    for(unsigned i = 0; i < 6; ++i)
        e.memory(layout.instance + instance_object + i * 4, object[i]);
    unsigned hash = 0;
    for(uint32_t handle = memory_handle; handle; handle >>= 9)
        hash ^= handle & 0x1ff;
    e.memory(layout.instance + hash * 8, memory_handle);
    e.memory(layout.instance + hash * 8 + 4, instance_object << 9 | 1);
    if(!e.flush())
        return false;

    // Keep the interrupt line quiet. This module polls the display engine.
    e.wr(0x000140, 0);
    const unsigned resources = e.rd(0x612004);
    for(unsigned head = 0; head < heads; ++head)
        if(resources & (1u << head))
            for(unsigned word = 0; word < 12; word += 4)
                e.wr(0x6101b4 + head * 0x800 + word, e.rd(0x616104 + head * 0x800 + word));
    for(unsigned dac = 0; dac < 4; ++dac)
        if(resources & (0x10u << dac))
            e.wr(0x6101c0 + dac * 0x800, e.rd(0x61a000 + dac * 0x800));
    for(unsigned sor = 0; sor < 8; ++sor)
        if(resources & (0x100u << sor))
            e.wr(0x6301c4 + sor * 0x800, e.rd(0x61c000 + sor * 0x800));

    state.stage = ScanoutStage::Ownership;
    if(e.rd(0x6100ac) & 0x100)
    {
        e.wr(0x6100ac, 0x100);
        e.wr(0x6194e8, e.rd(0x6194e8) & ~1u);
        if(!e.wait(0x6194e8, 2, 0))
            return false;
    }
    e.wr(0x610010, layout.instance >> 8 | 9);
    e.wr(0x610090, 0);
    e.wr(0x6100a0, 0);
    e.wr(0x6100b0, 0x307);
    for(unsigned head = 0; head < heads; ++head)
        if(resources & (1u << head))
            e.wr(0x616308 + head * 0x800, (e.rd(0x616308 + head * 0x800) & ~0x111u) | 0x010);

    state.stage = ScanoutStage::Channel;
    e.wr(0x6100a0, e.rd(0x6100a0) | 1);
    e.wr(0x610494, layout.push >> 8 | 1);
    e.wr(0x610498, 0x00010000);
    e.wr(0x61049c, 1);
    e.wr(0x610490, e.rd(0x610490) | 0x10);
    e.wr(0x640000, 0);
    e.wr(0x610490, 0x01000013);
    state.channel_control = e.rd(0x610490);
    if(!e.wait(0x610490, 0x80000000, 0))
    {
        state.channel_control = e.rd(0x610490);
        return false;
    }
    state.owned = true;
    state.put = 0;
    return true;
}

// Acknowledge one pending supervisor request without a change to clock or link.
// Return false if the display engine reported a command error.
bool service(const Engine& e, ScanoutState& state)
{
    ++state.polls;
    if(e.rd(0x61009c) & 1)
    {
        // Keep the words of the first error of a call. They identify the cause.
        for(unsigned word = 0; word < 3 && !state.errors; ++word)
            state.error[word] = e.rd(0x6101f0 + word * 4);
        ++state.errors;
        e.wr(0x61009c, 1);
        e.wr(0x6101f0, 0x90000000);
        state.stage = ScanoutStage::Command;
        return false;
    }
    if(const uint32_t pending = e.rd(0x6100ac) & 7)
    {
        state.supervisor_requests = state.supervisor_requests << 4 | pending;
        // Stage 1 starts the sequence of this update. A stage 3 request that is pending at the
        // same time or earlier belongs to the update before it.
        state.stages = pending & 1 ? uint8_t(pending & 3) : uint8_t(state.stages | pending);
        for(unsigned stage = 0; stage < 3; ++stage)
            if(pending & (1u << stage))
                state.stage_poll[stage] = state.polls;
        const unsigned mask = e.head_mask();
        e.wr(0x6100ac, pending);
        for(unsigned index = 0; index < heads; ++index)
            if(mask & (1u << index))
            {
                if(index == state.head)
                {
                    const uint32_t request = e.rd(0x6101d4 + index * 0x800);
                    state.supervisor_head |= request;
                    for(unsigned stage = 0; stage < 3; ++stage)
                        if(pending & (1u << stage))
                            state.supervisor_stage[stage] = request;
                }
                e.wr(0x6101d4 + index * 0x800, 0);
            }
        e.wr(0x6101d0, 0x80000000);
    }
    return true;
}

// Run the queued commands. The engine must read all of them in two seconds.
bool run_commands(const Engine& e, ScanoutState& state, uint32_t put)
{
    e.wr(0x640000, put);
    state.put = put;
    for(unsigned i = 0; i < 20000; ++i)
    {
        if(!service(e, state))
        {
            // A refused UPDATE is the last command, and the head keeps its surface, as the first
            // desktop test showed. Then the next call can use the channel. A refused method
            // before the UPDATE leaves a display state and a command page that no test shows.
            for(unsigned polls = 0; polls < 100; ++polls)
            {
                service(e, state);
                e.io.delay_us(100);
            }
            state.recoverable = (state.error[0] & 0xffc) == 0x80 && state.errors == 1 &&
                                !(e.rd(0x6100ac) & 7) && (e.rd(0x640004) & 0xffc) == put;
            state.stage = ScanoutStage::Command;
            return false;
        }
        if(!(e.rd(0x6100ac) & 7) && (e.rd(0x640004) & 0xffc) == put)
            return true;
        e.io.delay_us(100);
    }
    state.channel_control = e.rd(0x610490);
    return false;
}

// Poll limit for the last supervisor stage: five seconds. No test shows the time that the engine
// needs.
constexpr unsigned stage_polls = 50000;

// Wait for the end of the update. The third desktop test showed that the engine changes the mode
// in three supervisor stages. The armed head state shows the surface before the last stage.
// Thus an update with a supervisor request is complete only after stage 3.
// An update without a request is complete when the armed state shows the surface for 20 ms.
// A missing armed state without a request is a diagnostic result and not a failure.
bool confirm(const Engine& e, ScanoutState& state, uint32_t offset)
{
    const unsigned head = state.head * 0x300;
    state.confirmed = false;
    unsigned quiet = 0;
    for(unsigned i = 0; i < stage_polls; ++i)
    {
        if(!service(e, state))
            return false;
        state.armed_context = e.rd(0x640474 + head);
        state.armed_offset = e.rd(0x640460 + head);
        const bool idle = !(e.rd(0x6100ac) & 7);
        const bool staged = state.stages & 1, last = (state.stages & 5) == 5;
        if(idle && state.armed_offset == offset && (last || (!staged && ++quiet >= 200)))
        {
            state.confirmed = true;
            return true;
        }
        if(!idle || state.armed_offset != offset)
            quiet = 0;
        if(idle && !staged && i >= 5000)
            return true;
        e.io.delay_us(100);
    }
    state.channel_control = e.rd(0x610490);
    return false;
}
}  // namespace

bool scanout(const ProbeIo& io,
             const Snapshot& snapshot,
             const ScanoutLayout& layout,
             const Surface& surface,
             ScanoutState& state)
{
    const auto& boot = snapshot.boot;
    const Engine e{io};
    const bool first = !state.owned;
    // The head keeps the firmware timing. The output size comes from the captured viewport.
    const uint32_t output = boot.head[head_viewport_out];
    const uint32_t out_width = output & 0x7fff, out_height = output >> 16 & 0x7fff;
    const uint64_t bytes = memory_bytes(boot);
    const uint64_t span = uint64_t(surface.pitch) * surface.height;
    if(first)
        state = {};
    for(auto& request : state.supervisor_stage)
        request = 0;
    state.stages = 0;
    state.errors = 0;
    state.recoverable = false;
    for(auto& word : state.error)
        word = 0;
    state.polls = 0;
    for(auto& poll : state.stage_poll)
        poll = 0;
    state.stage = ScanoutStage::Precondition;
    // Bit 31 of the BAR 1 instance register enables address translation.
    if(!boot.route_matched || boot.core[boot_index("bar1_instance")] >> 31 ||
       boot.head_index >= heads || !out_width || !out_height || !bytes || !surface.width ||
       !surface.height || surface.width > out_width || surface.height > out_height ||
       surface.pitch < surface.width * 4 || surface.pitch & 0xff || surface.pitch > 0x1fff00 ||
       surface.offset & 0xfff || surface.offset + span > bytes || layout.instance & 0xffff ||
       layout.push & 0xfff || layout.check & 3 || !layout.check_word ||
       uint64_t(layout.instance) + 0x10000 > bytes || uint64_t(layout.push) + 0x1000 > bytes ||
       uint64_t(layout.check) + 4 > bytes)
        return false;
    state.head = boot.head_index;
    const uint32_t saved_window = e.rd(0x001700);
    const unsigned head = state.head * 0x300;
    // The engine rejects an update that has no timing or output resource for an active head.
    // Read the active firmware state now. A new core channel does not keep it as pending state.
    uint32_t kept[kept_words];
    if(first)
        for(unsigned i = 0; i < kept_words; ++i)
        {
            kept[i] = e.rd(0x640000 + kept_method(i, head, boot.sor));
            const unsigned offset = i ? kept_head_methods[i - 1] : 0;
            // Firmware marks its clock as not set by a driver. The engine refuses this bit from
            // a channel. Firmware also sets no clock limit. Nouveau sends the clock as the limit.
            if(offset == pixel_clock_config)
                kept[i] &= ~(1u << 24);
            if(offset == pixel_clock_max && !kept[i])
                kept[i] = kept[i - 2];
        }
    bool ok = !first || take_display(e, layout, bytes, state);
    if(ok)
    {
        const uint32_t size = surface.height << 16 | surface.width;
        const uint32_t commands[] = {
            // Surface offset, then size, storage, format, and memory object.
            1u << 18 | (0x0460 + head),
            surface.offset >> 8,
            4u << 18 | (0x0468 + head),
            size,
            0x01000000 | surface.pitch,
            0xcf00,
            memory_handle,
            // Input viewport. The head scales it to the output size.
            1u << 18 | (0x04b0 + head),
            0,
            1u << 18 | (0x04b8 + head),
            size,
            3u << 18 | (0x04c0 + head),
            output,
            output,
            output,
            // No output table, no hardware cursor, no base or overlay channel.
            1u << 18 | (0x0448 + head),
            0,
            1u << 18 | (0x045c + head),
            0,
            1u << 18 | (0x0480 + head),
            0x05000000,
            1u << 18 | (0x048c + head),
            0,
            2u << 18 | (0x04d0 + head),
            0,
            0x100,
            // Update without a notifier.
            2u << 18 | 0x0080,
            0,
            0,
        };
        constexpr unsigned count = sizeof(commands) / sizeof(*commands);
        static_assert(kept_words * 8 + count * 4 <= command_limit);
        state.stage = ScanoutStage::Update;
        if(state.put + count * 4 > command_limit)
        {
            // Return to the start of the page. The engine has read all earlier commands.
            e.memory(layout.push + state.put, 0x20000000);
            ok = e.flush() && run_commands(e, state, 0);
        }
        if(ok)
        {
            uint32_t put = state.put;
            // Send each firmware value that the pending state does not hold. The clock
            // configuration and the clock limit can differ from the active state. Thus the engine
            // can request a mode change. The frequency and the raster do not change.
            for(unsigned i = 0; first && i < kept_words; ++i)
            {
                const unsigned method = kept_method(i, head, boot.sor);
                if(kept[i] == e.rd(0x660000 + method))
                    continue;
                e.memory(layout.push + put, 1u << 18 | method);
                e.memory(layout.push + put + 4, kept[i]);
                put += 8;
                ++state.restored;
                state.restored_mask |= 1u << i;
            }
            for(unsigned i = 0; i < count; ++i)
                e.memory(layout.push + put + i * 4, commands[i]);
            ok = e.flush() && run_commands(e, state, put + count * 4) &&
                 confirm(e, state, surface.offset >> 8);
        }
        if(ok)
        {
            state.stage = ScanoutStage::Ready;
            state.surface = surface;
            // The commands disabled the hardware pointer.
            state.cursor_visible = false;
        }
    }
    e.wr(0x001700, saved_window);
    return ok;
}

namespace
{
// Arrow pointer. 'X' is the outline and '.' is the body. The tip is the top-left pixel.
constexpr const char* pointer_rows[] = {
    "X",
    "XX",
    "X.X",
    "X..X",
    "X...X",
    "X....X",
    "X.....X",
    "X......X",
    "X.......X",
    "X........X",
    "X.........X",
    "X......XXXXX",
    "X...X..X",
    "X..XX..X",
    "X.X  X..X",
    "XX   X..X",
    "X     XX",
};
constexpr unsigned pointer_size = 64;
}  // namespace

bool cursor(const ProbeIo& io,
            const Snapshot& snapshot,
            const ScanoutLayout& layout,
            ScanoutState& state,
            uint32_t x,
            uint32_t y,
            bool visible)
{
    const Engine e{io};
    const uint64_t bytes = memory_bytes(snapshot.boot);
    if(!state.owned || state.stage != ScanoutStage::Ready || layout.cursor & 0xfff ||
       uint64_t(layout.cursor) + pointer_size * pointer_size * 4 > bytes ||
       x >= state.surface.width || y >= state.surface.height)
        return false;
    // A hidden pointer needs no register access.
    if(!visible && !state.cursor_visible)
        return true;
    const unsigned head = state.head * 0x300, user = 0x64d000 + state.head * 0x1000;
    const uint32_t saved_window = e.rd(0x001700);
    bool ok = true;
    if(visible && !state.cursor_image)
    {
        // Write the image one time. A pixel with alpha 0 is transparent.
        for(unsigned row = 0; row < pointer_size; ++row)
        {
            const char* pixels =
                row < sizeof(pointer_rows) / sizeof(*pointer_rows) ? pointer_rows[row] : "";
            bool end = false;
            for(unsigned col = 0; col < pointer_size; ++col)
            {
                end = end || !pixels[col];
                const char pixel = end ? ' ' : pixels[col];
                e.memory(layout.cursor + (row * pointer_size + col) * 4,
                         pixel == 'X'   ? 0xff000000
                         : pixel == '.' ? 0xffffffff
                                        : 0);
            }
        }
        ok = e.flush();
        state.cursor_image = ok;
    }
    if(ok && !state.cursor_channel)
    {
        // Start the position channel of the head. It needs no command page.
        const unsigned control = 0x610490 + (13 + state.head) * 0x10;
        e.wr(control, 1);
        ok = e.wait(control, 0x00030000, 0x00010000);
        state.cursor_channel = ok;
    }
    if(ok && visible)
    {
        // The position is in output pixels. The head scales a smaller surface to the output.
        const uint32_t output = snapshot.boot.head[head_viewport_out];
        const uint32_t out_x = uint64_t(x) * (output & 0x7fff) / state.surface.width;
        const uint32_t out_y = uint64_t(y) * (output >> 16 & 0x7fff) / state.surface.height;
        e.wr(user + 0x84, out_y << 16 | out_x);
        e.wr(user + 0x80, 0);
    }
    if(ok && visible != state.cursor_visible)
    {
        const uint32_t commands[] = {
            // Control with a 64 x 64 A8R8G8B8 image, then the image offset and its memory object.
            2u << 18 | (0x0480 + head),
            visible ? 0x85000000 : 0x05000000,
            layout.cursor >> 8,
            1u << 18 | (0x048c + head),
            visible ? memory_handle : 0,
            2u << 18 | 0x0080,
            0,
            0,
        };
        constexpr unsigned count = sizeof(commands) / sizeof(*commands);
        state.stages = 0;
        state.errors = 0;
        if(state.put + count * 4 > command_limit)
        {
            e.memory(layout.push + state.put, 0x20000000);
            ok = e.flush() && run_commands(e, state, 0);
        }
        if(ok)
        {
            for(unsigned i = 0; i < count; ++i)
                e.memory(layout.push + state.put + i * 4, commands[i]);
            ok = e.flush() && run_commands(e, state, state.put + count * 4) &&
                 confirm(e, state, state.surface.offset >> 8);
        }
        if(ok)
            state.cursor_visible = visible;
    }
    e.wr(0x001700, saved_window);
    return ok;
}
}  // namespace gm206

extern "C" const gm206::Exports* os1_driver_init()
{
    static constexpr gm206::Exports exports{
        OS1_GM206_ABI, sizeof(gm206::Exports), gm206::probe, gm206::scanout, gm206::cursor};
    return &exports;
}
