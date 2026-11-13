#include <gtest/gtest.h>

#include <string_view>

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

    uint32_t read(unsigned reg) const
    {
        if(reg == 0x610490)
            return 0x01000013;
        if(reg == 0x64046c)
            return 0x10203040;
        if(reg == gm206::board_route)
            return route;
        if(reg >= 0xd980 && reg <= 0xd9cc)
            reg -= 0x50;
        else if(reg >= 0xd930 && reg <= 0xd97c)
            return 0;
        if(reg == 0)
            return identity;
        if(reg == 0x612004)
            return 0x201;
        if(reg == 0x640220)
            return 0x901;
        if(reg == 0x640414)
            return (2222u << 16) | 4000u;
        if(reg == 0x640450)
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
    bool core_seen = false, storage_seen = false;
    while(!page.end)
    {
        const auto start = page.next;
        page = children(probe.nodes[4].id, start);
        ASSERT_GT(page.next, start);
        ASSERT_LE(page.next, 41u);
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
            if(std::string_view(node.name) == "head_surface_storage_raw")
            {
                EXPECT_EQ(node.integer, 0x10203040u);
                storage_seen = true;
            }
        }
    }
    EXPECT_EQ(fields, 41u);
    EXPECT_TRUE(core_seen);
    EXPECT_TRUE(storage_seen);
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
