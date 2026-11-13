#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>
#include <vector>

#include "driver/gm206.h"
#include "drivers/display/reflection.hpp"

namespace
{
void checksum(uint8_t* block)
{
    uint8_t sum = 0;
    for(unsigned i = 0; i < 127; ++i)
        sum += block[i];
    block[127] = uint8_t(0 - sum);
}

struct AuxFixture
{
    uint8_t edid[256]{};
    uint32_t control = 0, status = 0x10000000, address = 0, tx = 0, autodpcd = 0x10000;
    unsigned writes = 0, position = 0, last_read = 0;
    unsigned chunk_limit = 16;
    bool timeout = false, defer_offset = false;
    unsigned offset_updates = 0;
    uint32_t identity = 0x12600000, route = 0x12;
    unsigned head = 0;
    mutable std::vector<unsigned> reads;

    uint32_t read(unsigned reg) const
    {
        reads.push_back(reg);
        if(reg == 0x610490)
            return 0x01000013;
        if(reg == 0x64046c)
            return 0x10203040;
        if(reg == 0x6194e8)
            return 0x3;
        if(reg == 0x001704)
            return 0x80000123;
        if(reg == 0x11020c)
            return 2048;
        if(reg == 0x6104a0 + head * 0x10)
            return 0x48000088;
        if(reg == 0x641408 + head * 0x1000)
            return (768u << 16) | 1024u;
        if(reg == 0x661408 + head * 0x1000)
            return (1080u << 16) | 1920u;
        if(reg == gm206::board_route)
            return route;
        if(reg >= 0xd980 && reg <= 0xd9cc)
            reg -= 0x50;
        else if(reg >= 0xd930 && reg <= 0xd97c)
            return 0;
        if(reg == 0)
            return identity;
        if(reg == 0x612004)
            return 0x200 | (1u << head);
        if(reg == 0x640220)
            return 0x900 | (1u << head);
        if(reg == 0x640414 + head * 0x300)
            return (2222u << 16) | 4000u;
        if(reg == 0x640450 + head * 0x300)
            return 533250000;
        switch(reg)
        {
            case 0xd954:
                return control;
            case 0xd958:
                return status;
            case 0xd950:
                return address;
            case 0xd930:
                return tx;
            case 0xd968:
                return autodpcd;
            case 0xd970:
                return 2;
            default:
                break;
        }
        if(reg >= 0xd940 && reg <= 0xd94c)
        {
            const unsigned offset = reg - 0xd940;
            if(address == 0)
                return offset == 0 ? 0x041412 : 0;
            if(address == 0x100)
                return 0x8414;
            if(address == 0x202)
                return offset == 0 ? 0x01017777 : 0x0000;
            uint32_t word = 0;
            for(unsigned i = 0; i < 4; ++i)
                word |= uint32_t(edid[position - last_read + offset + i]) << (8 * i);
            return word;
        }
        return 0;
    }

    void write(unsigned reg, uint32_t value)
    {
        ++writes;
        if(reg >= 0xd980 && reg <= 0xd9cc)
            reg -= 0x50;
        switch(reg)
        {
            case 0xd954: {
                control = value & ~0x87000000u;
                if(value & 0x00100000)
                    control |= 0x01000000;
                if(!(value & 0x10000) || timeout)
                    return;
                control &= ~0x10000u;
                const unsigned command = (value >> 12) & 15;
                status = 0x10000000 | ((value & 0x100) ? 0 : (value & 15) + 1);
                if(command == 4)
                {
                    position = tx;
                    if(defer_offset)
                        status |= 0x80000;
                }
                if(command == 6)
                    ++offset_updates;
                if(command == 5)
                {
                    last_read = status & 31;
                    if(last_read > chunk_limit)
                        last_read = chunk_limit;
                    status = 0x10000000 | last_read;
                    position += last_read;
                }
                EXPECT_TRUE(command == 4 || command == 5 || command == 1 || command == 9 ||
                            command == 6);
                return;
            }
            case 0xd950:
                address = value;
                return;
            case 0xd930:
                tx = value;
                return;
            case 0xd968:
                autodpcd = value;
                return;
            default:
                ADD_FAILURE() << "unexpected register write " << reg;
        }
    }

    gm206::ProbeIo io()
    {
        return {this,
                [](void* p, unsigned reg) { return static_cast<AuxFixture*>(p)->read(reg); },
                [](void* p, unsigned reg, uint32_t value) {
                    static_cast<AuxFixture*>(p)->write(reg, value);
                },
                [](uint64_t) {}};
    }

    void make_edid()
    {
        constexpr uint8_t dtd[] = {0x4d,
                                   0xd0,
                                   0x00,
                                   0xa0,
                                   0xf0,
                                   0x70,
                                   0x3e,
                                   0x80,
                                   0x30,
                                   0x20,
                                   0x35,
                                   0x00,
                                   0x55,
                                   0x50,
                                   0x21,
                                   0x00,
                                   0x00,
                                   0x1a};
        for(unsigned i = 1; i < 7; ++i)
            edid[i] = 255;
        edid[8] = 0x10;
        edid[9] = 0xac;
        edid[24] = 2;
        edid[75] = 0xfc;
        edid[77] = 'D';
        edid[78] = 'E';
        edid[79] = 'L';
        edid[80] = 'L';
        edid[81] = '\n';
        edid[126] = 1;
        for(unsigned i = 0; i < sizeof(dtd); ++i)
            edid[54 + i] = dtd[i];
        checksum(edid);
        edid[128] = 2;
        edid[129] = 3;
        edid[130] = 7;
        edid[132] = 0x42;
        edid[133] = 16;
        edid[134] = 4;
        checksum(edid + 128);
    }
};
}  // namespace

TEST(Gm206, ReportsLiveScanoutAndBoundedSinkModes)
{
    AuxFixture fixture;
    fixture.make_edid();
    fixture.chunk_limit = 8;
    fixture.defer_offset = true;
    gm206::Snapshot state{};
    ASSERT_TRUE(gm206::probe(fixture.io(), state));
    ASSERT_EQ(state.head_count, 1u);
    EXPECT_TRUE(state.boot.route_matched);
    EXPECT_EQ(state.boot.sor, 1);
    EXPECT_EQ(state.boot.link, 2);
    EXPECT_EQ(state.boot.head_index, 0);
    EXPECT_TRUE(state.boot.link_config_read);
    EXPECT_EQ(state.boot.link_config[0], 0x14);
    EXPECT_EQ(state.boot.link_config[1], 0x84);
    EXPECT_TRUE(state.boot.link_status_read);
    EXPECT_EQ(state.boot.link_status[0], 0x77);
    EXPECT_EQ(state.scanouts[0].pixel_hz, 533250000u);
    ASSERT_EQ(state.output_count, 1u);
    const auto& output = state.outputs[0];
    EXPECT_EQ(output.dpcd_revision, 0x12);
    EXPECT_EQ(output.max_lanes, 4);
    EXPECT_STREQ(output.manufacturer, "DEL");
    EXPECT_STREQ(output.name, "DELL");
    EXPECT_EQ(output.edid_read_blocks, 2);
    ASSERT_EQ(output.mode_count, 5);
    EXPECT_EQ(output.modes[0].width, 3840u);
    EXPECT_EQ(output.modes[0].height, 2160u);
    EXPECT_EQ(output.modes[0].millihz, 59997u);
    EXPECT_TRUE(output.modes[0].preferred);
    EXPECT_EQ(output.modes[1].millihz, 60000u);
    EXPECT_EQ(output.modes[2].millihz, 59940u);
    EXPECT_EQ(fixture.offset_updates, 2u);
    EXPECT_EQ(fixture.control, 0u);
    EXPECT_EQ(fixture.address, 0u);
    EXPECT_EQ(fixture.autodpcd, 0x10000u);
}

TEST(Gm206, RejectsOtherChipAndRestoresAuxStateOnTimeout)
{
    AuxFixture fixture;
    gm206::Snapshot state{};
    fixture.identity = UINT32_MAX;
    EXPECT_FALSE(gm206::probe(fixture.io(), state));
    EXPECT_EQ(fixture.writes, 0u);
    fixture.identity = 0x12600000;
    fixture.timeout = true;
    EXPECT_TRUE(gm206::probe(fixture.io(), state));
    EXPECT_EQ(state.output_count, 0u);
    EXPECT_EQ(fixture.control, 0u);
    EXPECT_EQ(fixture.address, 0u);
    EXPECT_EQ(fixture.autodpcd, 0x10000u);
}

TEST(Gm206, RejectsBadEdidAndIgnoresCorruptExtension)
{
    AuxFixture fixture;
    fixture.make_edid();
    gm206::Output output{};
    fixture.edid[135] ^= 1;
    EXPECT_TRUE(gm206::parse_edid(fixture.edid, 2, output));
    EXPECT_EQ(output.edid_blocks, 1);
    EXPECT_EQ(output.mode_count, 1);
    fixture.edid[0] ^= 1;
    EXPECT_FALSE(gm206::parse_edid(fixture.edid, 2, output));
    EXPECT_EQ(output.mode_count, 0);
}

TEST(Gm206, PagesTimingFieldsAndKeepsFirmwareModesSeparate)
{
    AuxFixture fixture;
    fixture.make_edid();
    gm206::Snapshot state{};
    ASSERT_TRUE(gm206::probe(fixture.io(), state));
    const native_display::Reflection reflection{true, false, 1024, 768, 32, 1, 1, &state};
    auto children = [&](unsigned id, unsigned start = 0) {
        Os1HardwareQuery query{};
        query.version = OS1_HARDWARE_VERSION;
        query.operation = OS1_HARDWARE_CHILDREN;
        query.id = id;
        query.start = start;
        query.generation = 1;
        EXPECT_EQ(native_display::query_reflection(query, reflection), 0);
        return query;
    };
    auto root = children(OS1_HARDWARE_DISPLAY_ROOT);
    ASSERT_EQ(root.count, 7u);
    EXPECT_STREQ(root.nodes[5].name, "mode_count");
    EXPECT_EQ(root.nodes[5].integer, 1u);
    ASSERT_STREQ(root.nodes[6].name, "gm206");
    auto probe = children(root.nodes[6].id);
    ASSERT_EQ(probe.count, 5u);
    EXPECT_STREQ(probe.nodes[4].name, "boot_state");
    auto boot = children(probe.nodes[4].id);
    ASSERT_EQ(boot.count, 8u);
    EXPECT_STREQ(boot.nodes[2].name, "route_matched");
    EXPECT_EQ(boot.nodes[2].integer, 1u);
    auto link = children(probe.nodes[4].id, boot.next);
    EXPECT_EQ(link.nodes[1].integer, 4u);
    auto page = link;
    unsigned fields = boot.count + link.count;
    bool core_seen = false, storage_seen = false, owner_seen = false, base_seen = false;
    while(!page.end)
    {
        const auto start = page.next;
        page = children(probe.nodes[4].id, start);
        ASSERT_GT(page.next, start);
        ASSERT_LE(page.next, 68u);
        fields += page.count;
        for(unsigned i = 0; i < page.count; ++i)
        {
            const auto& node = page.nodes[i];
            EXPECT_EQ(node.parent, probe.nodes[4].id);
            if(std::string_view(node.name) == "core_channel_control")
            {
                EXPECT_EQ(node.integer, 0x01000013u);
                core_seen = true;
            }
            if(std::string_view(node.name) == "vbios_display_owner")
            {
                EXPECT_EQ(node.integer, 3u);
                owner_seen = true;
            }
            if(std::string_view(node.name) == "base_surface_size_raw")
            {
                EXPECT_EQ(node.integer, (768u << 16) | 1024u);
                base_seen = true;
            }
            if(std::string_view(node.name) == "head_surface_storage_raw")
            {
                EXPECT_EQ(node.integer, 0x10203040u);
                storage_seen = true;
            }
        }
    }
    EXPECT_EQ(fields, 68u);
    EXPECT_TRUE(core_seen);
    EXPECT_TRUE(storage_seen);
    EXPECT_TRUE(owner_seen);
    EXPECT_TRUE(base_seen);
    EXPECT_STREQ(probe.nodes[2].name, "diagnostics");
    auto outputs = children(probe.nodes[0].id);
    ASSERT_EQ(outputs.count, 1u);
    auto output = children(outputs.nodes[0].id);
    ASSERT_EQ(output.count, 8u);
    EXPECT_STREQ(output.nodes[6].name, "manufacturer");
    auto tail = children(outputs.nodes[0].id, output.next);
    ASSERT_EQ(tail.count, 3u);
    auto edid = children(tail.nodes[2].id);
    ASSERT_EQ(edid.count, 8u);
    EXPECT_EQ(edid.nodes[0].integer, 2u);
    EXPECT_STREQ(edid.nodes[1].name, "hex_0");
    EXPECT_EQ(edid.nodes[1].text[0], '0');
    auto edid_tail = children(tail.nodes[2].id, edid.next);
    ASSERT_EQ(edid_tail.count, 1u);
    EXPECT_STREQ(edid_tail.nodes[0].name, "hex_7");
    constexpr char digits[] = "0123456789abcdef";
    for(unsigned chunk = 0; chunk < 8; ++chunk)
    {
        const auto& node = chunk < 7 ? edid.nodes[chunk + 1] : edid_tail.nodes[0];
        char expected[65]{};
        for(unsigned i = 0; i < 32; ++i)
        {
            const auto byte = fixture.edid[chunk * 32 + i];
            expected[i * 2] = digits[byte >> 4];
            expected[i * 2 + 1] = digits[byte & 15];
        }
        EXPECT_STREQ(node.text, expected);
    }
    auto modes = children(output.nodes[5].id);
    ASSERT_EQ(modes.count, 5u);
    const auto mode = modes.nodes[0].id;
    auto first = children(mode);
    ASSERT_EQ(first.count, 8u);
    EXPECT_EQ(first.end, 0u);
    EXPECT_EQ(first.nodes[0].integer, 3840u);
    auto rest = children(mode, first.next);
    ASSERT_EQ(rest.count, 5u);
    EXPECT_EQ(rest.end, 1u);
    EXPECT_EQ(rest.nodes[0].parent, mode);
    EXPECT_NE(first.nodes[7].id, rest.nodes[0].id);
    EXPECT_STREQ(rest.nodes[4].name, "vertical_sync_end");
    Os1HardwareQuery describe{};
    describe.version = OS1_HARDWARE_VERSION;
    describe.operation = OS1_HARDWARE_DESCRIBE;
    describe.id = rest.nodes[4].id;
    ASSERT_EQ(native_display::query_reflection(describe, reflection), 0);
    EXPECT_EQ(describe.nodes[0].integer, rest.nodes[4].integer);
}

TEST(Gm206, ReportsRejectedChipRegisterWithoutPublishingSinkData)
{
    AuxFixture fixture;
    fixture.identity = 0x125000a1;
    gm206::Snapshot snapshot{};
    ASSERT_FALSE(gm206::probe(fixture.io(), snapshot));
    EXPECT_EQ(snapshot.boot0, fixture.identity);
    native_display::Gm206Identity identity{};
    identity.status = "chip identity rejected";
    identity.boot0 = snapshot.boot0;
    identity.boot0_read = true;
    const native_display::Reflection reflection{
        true, false, 1024, 768, 32, 1, 1, nullptr, &identity};
    auto children = [&](unsigned id, unsigned start = 0) {
        Os1HardwareQuery query{};
        query.version = OS1_HARDWARE_VERSION;
        query.operation = OS1_HARDWARE_CHILDREN;
        query.id = id;
        query.start = start;
        EXPECT_EQ(native_display::query_reflection(query, reflection), 0);
        return query;
    };
    auto probe = children(OS1_HARDWARE_GM206_ROOT);
    ASSERT_EQ(probe.count, 4u);
    EXPECT_EQ(children(probe.nodes[0].id).count, 0u);
    const auto gpu = probe.nodes[3].id;
    auto first = children(gpu);
    ASSERT_EQ(first.count, 8u);
    EXPECT_STREQ(first.nodes[0].text, identity.status);
    auto second = children(gpu, first.next);
    ASSERT_EQ(second.count, 8u);
    EXPECT_STREQ(second.nodes[3].name, "boot0");
    EXPECT_EQ(second.nodes[3].integer, fixture.identity);
    auto third = children(gpu, second.next);
    ASSERT_EQ(third.count, 1u);
    EXPECT_STREQ(third.nodes[0].name, "boot0_read");
    EXPECT_EQ(third.nodes[0].integer, 1u);
}

TEST(Gm206, DoesNotAssociateMismatchedRouteWithHead)
{
    AuxFixture fixture;
    fixture.route = 0x02;
    gm206::Snapshot state{};
    ASSERT_TRUE(gm206::probe(fixture.io(), state));
    ASSERT_EQ(state.head_count, 1u);
    EXPECT_FALSE(state.boot.route_matched);
    EXPECT_EQ(state.boot.sor, 1);
    EXPECT_EQ(state.boot.link, 1);
}

TEST(Gm206, CapturesOwnershipStateWithoutRegisterWrites)
{
    AuxFixture fixture;
    fixture.status = 0;
    fixture.head = 3;
    gm206::Snapshot state{};
    ASSERT_TRUE(gm206::probe(fixture.io(), state));
    // No sink is present, so the capture must not write a register.
    EXPECT_EQ(fixture.writes, 0u);
    ASSERT_TRUE(state.boot.route_matched);
    EXPECT_EQ(state.boot.head_index, 3);
    const auto core = [](const gm206::Snapshot& snapshot, std::string_view name) {
        for(unsigned i = 0; i < std::size(gm206::boot_registers); ++i)
            if(name == gm206::boot_registers[i].name)
                return snapshot.boot.core[i];
        ADD_FAILURE() << "missing register " << name;
        return 0u;
    };
    const auto base = [&](std::string_view name) {
        for(unsigned i = 0; i < std::size(gm206::base_registers); ++i)
            if(name == gm206::base_registers[i].name)
                return state.boot.base[i];
        ADD_FAILURE() << "missing register " << name;
        return 0u;
    };
    const auto was_read = [&](unsigned offset) {
        return std::find(fixture.reads.begin(), fixture.reads.end(), offset) != fixture.reads.end();
    };
    EXPECT_EQ(core(state, "vbios_display_owner"), 3u);
    EXPECT_EQ(core(state, "bar1_instance"), 0x80000123u);
    EXPECT_EQ(core(state, "vram_fbpa0_mib"), 2048u);
    // Each offset comes from the Nouveau sources that the plan cites.
    constexpr unsigned ownership[] = {0x6194e8,
                                      0x001700,
                                      0x001704,
                                      0x001714,
                                      0x021c14,
                                      0x021d38,
                                      0x022438,
                                      0x022458,
                                      0x11020c,
                                      0x11120c,
                                      0x11220c,
                                      0x11320c};
    constexpr unsigned first = std::size(gm206::boot_registers) - std::size(ownership);
    ASSERT_STREQ(gm206::boot_registers[first].name, "vbios_display_owner");
    for(unsigned i = 0; i < std::size(ownership); ++i)
    {
        EXPECT_EQ(gm206::boot_registers[first + i].offset, ownership[i]);
        EXPECT_TRUE(was_read(ownership[i])) << gm206::boot_registers[first + i].name;
    }
    // Control registers and user areas use the selected head with different strides.
    EXPECT_EQ(base("base_channel_control"), 0x48000088u);
    EXPECT_EQ(base("base_surface_size_raw"), (768u << 16) | 1024u);
    EXPECT_EQ(base("base_surface_size_next_raw"), (1080u << 16) | 1920u);
    constexpr unsigned selected[] = {0x6104d0,
                                     0x6104d4,
                                     0x6104d8,
                                     0x6104dc,
                                     0x644000,
                                     0x6440c0,
                                     0x644400,
                                     0x644408,
                                     0x64440c,
                                     0x644410,
                                     0x6640c0,
                                     0x664400,
                                     0x664408,
                                     0x66440c,
                                     0x664410};
    ASSERT_EQ(std::size(selected), std::size(gm206::base_registers));
    for(unsigned i = 0; i < std::size(selected); ++i)
    {
        EXPECT_EQ(gm206::base_registers[i].offset + 3 * gm206::base_registers[i].stride,
                  selected[i]);
        EXPECT_TRUE(was_read(selected[i])) << gm206::base_registers[i].name;
    }

    // A route without a matching head must not read or publish base channel state.
    AuxFixture other;
    other.status = 0;
    other.route = 0x02;
    gm206::Snapshot unmatched{};
    ASSERT_TRUE(gm206::probe(other.io(), unmatched));
    EXPECT_FALSE(unmatched.boot.route_matched);
    EXPECT_EQ(core(unmatched, "vbios_display_owner"), 3u);
    for(const auto value : unmatched.boot.base)
        EXPECT_EQ(value, 0u);
    for(const auto offset : other.reads)
        EXPECT_FALSE((offset >= 0x6104a0 && offset < 0x6104e0) || offset >= 0x641000) << offset;
    const native_display::Reflection reflection{true, false, 1024, 768, 32, 1, 1, &unmatched};
    unsigned fields = 0;
    Os1HardwareQuery page{};
    do
    {
        const auto start = page.next;
        page = {};
        page.version = OS1_HARDWARE_VERSION;
        page.operation = OS1_HARDWARE_CHILDREN;
        page.id = OS1_HARDWARE_DISPLAY_ROOT + 12;
        page.start = start;
        page.generation = 1;
        ASSERT_EQ(native_display::query_reflection(page, reflection), 0);
        for(unsigned i = 0; i < page.count; ++i)
            EXPECT_NE(std::string_view(page.nodes[i].name).substr(0, 5), "base_");
        fields += page.count;
    } while(!page.end && page.count);
    EXPECT_EQ(fields, 16u + std::size(gm206::boot_registers));
}

TEST(Gm206, ReadsOnlyMappedRegisterRanges)
{
    for(unsigned head = 0; head < gm206::heads; ++head)
    {
        AuxFixture fixture;
        fixture.make_edid();
        fixture.head = head;
        gm206::Snapshot state{};
        ASSERT_TRUE(gm206::probe(fixture.io(), state));
        ASSERT_TRUE(state.boot.route_matched);
        ASSERT_EQ(state.output_count, 1u);
        for(const auto offset : fixture.reads)
        {
            bool mapped = false;
            for(const auto& range : gm206::register_ranges)
                mapped |= offset >= range.offset && offset - range.offset <= range.bytes - 4;
            EXPECT_TRUE(mapped) << "unmapped read " << offset;
        }
    }
    for(const auto& range : gm206::register_ranges)
    {
        EXPECT_EQ(range.offset % 4096, 0u);
        EXPECT_EQ(range.bytes % 4096, 0u);
        EXPECT_LE(range.offset + range.bytes, gm206::register_bytes);
    }
    // The table also covers the last AUX channel, SOR, and head that a probe can address.
    for(const unsigned offset :
        {0xd930u + 7 * 0x50, 0xd97cu + 7 * 0x50, 0x640200u + 7 * 0x20, 0x6404c8u + 3 * 0x300})
    {
        bool mapped = false;
        for(const auto& range : gm206::register_ranges)
            mapped |= offset >= range.offset && offset - range.offset <= range.bytes - 4;
        EXPECT_TRUE(mapped) << offset;
    }
}

TEST(Gm206, EncodesProgressive1080pHeadTiming)
{
    gm206::Mode mode{
        1920, 1080, 60000, 148500000, 2200, 1125, 2008, 2052, 1084, 1089, false, true, true};
    gm206::HeadTiming timing{};
    ASSERT_TRUE(gm206::prepare_head_timing(mode, timing));
    EXPECT_EQ(timing.raster, 0x04650898u);
    EXPECT_EQ(timing.sync_end, 0x0004002bu);
    EXPECT_EQ(timing.blank_end, 0x002800bfu);
    EXPECT_EQ(timing.blank_start, 0x0460083fu);
    EXPECT_EQ(timing.pixel_hz, 148500000u);
    mode.horizontal_sync_end = mode.horizontal_sync_start;
    EXPECT_FALSE(gm206::prepare_head_timing(mode, timing));
    mode.horizontal_sync_end = 2052;
    mode.vertical_total = 32768;
    EXPECT_FALSE(gm206::prepare_head_timing(mode, timing));
    mode.vertical_total = 1125;
    mode.pixel_hz = 0x80000000u;
    EXPECT_FALSE(gm206::prepare_head_timing(mode, timing));
}

TEST(Gm206, EncodesFirmware2160pHeadTimingFromDesktopCapture)
{
    // The expected words are the firmware head registers in the desktop boot report.
    const gm206::Mode mode{
        3840, 2160, 59997, 533250000, 4000, 2222, 3888, 3920, 2163, 2168, true, true, false};
    gm206::HeadTiming timing{};
    ASSERT_TRUE(gm206::prepare_head_timing(mode, timing));
    EXPECT_EQ(timing.raster, 145624992u);
    EXPECT_EQ(timing.sync_end, 262175u);
    EXPECT_EQ(timing.blank_end, 3801199u);
    EXPECT_EQ(timing.blank_start, 145362799u);
    EXPECT_EQ(timing.pixel_hz, 533250000u);
}

TEST(Gm206, RejectsHeadTimingOutsideEachAxisBound)
{
    const gm206::Mode valid{
        1920, 1080, 60000, 148500000, 2200, 1125, 2008, 2052, 1084, 1089, false, true, true};
    const gm206::HeadTiming untouched{1, 2, 3, 4, 5};
    const auto rejected = [&](auto change) {
        auto mode = valid;
        change(mode);
        auto timing = untouched;
        const bool ok = gm206::prepare_head_timing(mode, timing);
        // A rejected mode must not leave partial register values.
        return !ok && timing.raster == 1 && timing.sync_end == 2 && timing.blank_end == 3 &&
               timing.blank_start == 4 && timing.pixel_hz == 5;
    };
    EXPECT_TRUE(rejected([](gm206::Mode& m) { m.pixel_hz = 0; }));
    EXPECT_TRUE(rejected([](gm206::Mode& m) { m.width = 0; }));
    EXPECT_TRUE(rejected([](gm206::Mode& m) { m.height = 0; }));
    EXPECT_TRUE(rejected([](gm206::Mode& m) { m.horizontal_sync_start = 1919; }));
    EXPECT_TRUE(rejected([](gm206::Mode& m) { m.horizontal_sync_end = 2201; }));
    EXPECT_TRUE(rejected([](gm206::Mode& m) { m.horizontal_total = 32768; }));
    EXPECT_TRUE(rejected([](gm206::Mode& m) { m.vertical_sync_start = 1079; }));
    EXPECT_TRUE(rejected([](gm206::Mode& m) { m.vertical_sync_end = 1084; }));
    EXPECT_TRUE(rejected([](gm206::Mode& m) { m.vertical_sync_end = 1126; }));

    // Zero front and back porches and the largest totals remain valid.
    auto mode = valid;
    mode.horizontal_sync_start = 1920;
    mode.horizontal_sync_end = mode.horizontal_total = 32767;
    mode.vertical_sync_start = 1080;
    mode.vertical_sync_end = mode.vertical_total = 32767;
    mode.pixel_hz = 0x7fffffffu;
    gm206::HeadTiming timing{};
    ASSERT_TRUE(gm206::prepare_head_timing(mode, timing));
    EXPECT_EQ(timing.raster, 0x7fff7fffu);
    EXPECT_EQ(timing.sync_end, (uint32_t(32767 - 1080 - 1) << 16) | (32767 - 1920 - 1));
    EXPECT_EQ(timing.blank_end, timing.sync_end);
    EXPECT_EQ(timing.blank_start, (uint32_t(32766) << 16) | 32766u);
}
