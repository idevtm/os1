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
}  // namespace gm206

extern "C" const gm206::Exports* os1_driver_init()
{
    static constexpr gm206::Exports exports{OS1_GM206_ABI, sizeof(gm206::Exports), gm206::probe};
    return &exports;
}
