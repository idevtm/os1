#include <gtest/gtest.h>
#include <os1/resource.h>
#include <os1/vfs.h>

#include <algorithm>
#include <map>
#include <set>
#include <string_view>
#include <vector>

#include "driver/gm206.h"
#include "drivers/display/reflection.hpp"

namespace
{
// The display state that the kernel passes to the reflection of the module.
constexpr os1_display_summary summary{
    true, "firmware-framebuffer", "Fixed firmware display", 1024, 768, 32, 1, 1, 0, 0, 0};
struct Reflection
{
    const gm206::Snapshot* snapshot;
    const gm206::Identity* identity = nullptr;
};
long query_reflection(Os1HardwareQuery& query, const Reflection& reflection)
{
    return gm206::reflect(query, summary, reflection.snapshot, reflection.identity);
}
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
    const Reflection reflection{&state};
    auto children = [&](unsigned id, unsigned start = 0) {
        Os1HardwareQuery query{};
        query.version = OS1_HARDWARE_VERSION;
        query.operation = OS1_HARDWARE_CHILDREN;
        query.id = id;
        query.start = start;
        query.generation = 1;
        EXPECT_EQ(query_reflection(query, reflection), 0);
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
    ASSERT_EQ(query_reflection(describe, reflection), 0);
    EXPECT_EQ(describe.nodes[0].integer, rest.nodes[4].integer);
}

TEST(Gm206, ReportsRejectedChipRegisterWithoutPublishingSinkData)
{
    AuxFixture fixture;
    fixture.identity = 0x125000a1;
    gm206::Snapshot snapshot{};
    ASSERT_FALSE(gm206::probe(fixture.io(), snapshot));
    EXPECT_EQ(snapshot.boot0, fixture.identity);
    gm206::Identity identity{};
    identity.status = "chip identity rejected";
    identity.boot0 = snapshot.boot0;
    identity.boot0_read = true;
    const Reflection reflection{nullptr, &identity};
    auto children = [&](unsigned id, unsigned start = 0) {
        Os1HardwareQuery query{};
        query.version = OS1_HARDWARE_VERSION;
        query.operation = OS1_HARDWARE_CHILDREN;
        query.id = id;
        query.start = start;
        EXPECT_EQ(query_reflection(query, reflection), 0);
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
    const Reflection reflection{&unmatched};
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
        ASSERT_EQ(query_reflection(page, reflection), 0);
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

namespace
{
// Model of the display engine parts that the scanout path uses.
struct EngineFixture
{
    static constexpr uint32_t instance = 0x06400000, push = 0x06410000, check = 0x06411000;
    std::map<unsigned, uint32_t> registers, armed, assembly;
    std::map<uint32_t, uint32_t> memory;
    std::vector<std::pair<unsigned, uint32_t>> writes;
    std::vector<unsigned> accesses;
    std::vector<std::pair<uint32_t, uint32_t>> memory_writes;
    uint32_t check_word = 0, get = 0;
    bool translated = false, supervisor = false, command_error = false, pending_update = false;
    unsigned supervisor_stage = 0, stage1_polls = 0, stage3_polls = 0;
    bool stage3_never = false, stale_before_stage1 = false, cursor_channel_dead = false;
    uint32_t refused_method = ~0u, resume_get = 0, resume_put = ~0u;
    static constexpr uint32_t cursor = 0x06412000;
    bool point(uint32_t x, uint32_t y, bool visible)
    {
        return gm206::cursor(
            io(), snapshot, {instance, push, check, &check_word, cursor}, state, x, y, visible);
    }
    bool armed_hidden = false, retains_pending = false, installed = false;
    uint32_t get_flags = 0;
    // Active firmware state of the desktop card: SOR 1 control, then methods of the boot head.
    // The second desktop test recorded these values. A new core channel drops the timing values.
    struct Firmware
    {
        unsigned method;
        uint32_t value;
        bool timing;
    };
    static constexpr Firmware firmware[] = {
        {0x220, 0x901, true},
        {0x404, 0x150, true},
        {0x408, 0x21ec4000, false},
        {0x414, 0x08ae0fa0, true},
        {0x418, 0x0004001f, true},
        {0x41c, 0x003a006f, true},
        {0x420, 0x08aa0f6f, true},
        {0x424, 0x04910d01, true},
        {0x450, 0x1fc8bfd0, true},
        {0x454, 0x01200000, true},
        {0x470, 0xe900, false},
        {0x4d0, 0x00110300, false},
        // Offsets without a method show a filler value. The last one differs between the states.
        {0x18c, 0x552ac409, false},
        {0x43c, 0x552ac409, false},
        {0x6ec, 0xc3041400, false},
    };
    static constexpr unsigned no_method[] = {0x18c, 0x43c, 0x6ec};
    // Map a method of SOR 1 or of head 0 to the boot SOR or the boot head.
    unsigned head_method(unsigned method) const
    {
        if(method == 0x220)
            return 0x200 + snapshot.boot.sor * 0x20;
        return method < 0x400 ? method : method + snapshot.boot.head_index * 0x300;
    }
    gm206::Snapshot snapshot{};
    gm206::ScanoutState state{};

    EngineFixture()
    {
        registers[0x6100ac] = 0x100;
        registers[0x6194e8] = 0x803;
        registers[0x612004] = 0x700f2f;
        registers[0x001700] = 0x200;
        registers[0x610490] = 0x48070088;
        registers[0x616104] = 0x11;
        registers[0x61c800] = 0x22;
        auto& boot = snapshot.boot;
        boot.route_matched = true;
        boot.sor = 1;
        boot.head[15] = 0x08700f00;
        const auto index = [](std::string_view name) {
            for(unsigned i = 0; i < std::size(gm206::boot_registers); ++i)
                if(name == gm206::boot_registers[i].name)
                    return i;
            ADD_FAILURE() << name;
            return 0u;
        };
        boot.core[index("vram_fbp_count")] = 2;
        boot.core[index("vram_fbpa_per_fbp")] = 1;
        boot.core[index("vram_fbpa0_mib")] = 1024;
        boot.core[index("vram_fbpa1_mib")] = 1024;
        boot.core[index("vram_fbpa2_mib")] = 0xbadf2012;
    }
    uint32_t& word(uint32_t address)
    {
        return address == check && !translated ? check_word : memory[address];
    }
    void arm()
    {
        for(const auto& [method, value] : assembly)
            armed[method] = value;
        pending_update = false;
    }
    void fetch(uint32_t put)
    {
        while(get != put)
        {
            const uint32_t header = memory[push + get];
            if(header >> 29 == 1)
            {
                get = header & 0xffc;
                continue;
            }
            const unsigned count = header >> 18 & 0x3ff, method = header & 0xffc;
            for(unsigned i = 0; i < count; ++i)
            {
                // The engine refuses the firmware clock flag, as the second desktop test showed.
                // It then stops. The refused value does not reach the pending state.
                // The refusal of an offset without a method is an assumption of this model.
                const unsigned target = method + i * 4;
                const uint32_t value = memory[push + get + 4 + i * 4];
                const bool reserved =
                    std::any_of(std::begin(no_method), std::end(no_method), [&](unsigned offset) {
                        return head_method(offset) == target;
                    });
                if(reserved || target == refused_method ||
                   (target == head_method(0x454) && value >> 24 & 1))
                {
                    // The engine stops at the refused command. It continues after the error
                    // acknowledgment. This continuation is an assumption of the model.
                    resume_get = get + 4 + count * 4;
                    resume_put = put;
                    registers[0x61009c] = 1;
                    registers[0x6101f0] = (reserved ? 0x3000 : 0x4000) | target;
                    registers[0x6101f4] = value;
                    registers[0x6101f8] = 0x10;
                    return;
                }
                assembly[target] = value;
            }
            get += 4 + count * 4;
            if(method != 0x80)
                continue;
            // The engine refuses an update that has no timing or no output resource.
            bool valid = true;
            for(const auto& entry : firmware)
                valid &= !entry.timing || entry.method == 0x454 ||
                         assembly[head_method(entry.method)] == entry.value;
            valid &= assembly[head_method(0x454)] == 0x00200000;
            if(command_error || !valid)
            {
                registers[0x61009c] = 1;
                registers[0x6101f0] = 0x5080;
                registers[0x6101f4] = command_error ? 0x1234 : 0;
                registers[0x6101f8] = command_error ? 0 : 4;
            }
            else if(supervisor)
            {
                pending_update = true;
                supervisor_stage = 0;
                // The earlier stage 3 is pending together with stage 1, or stage 1 starts one
                // poll later.
                if(stale_before_stage1)
                    stage1_polls = 2;
                else
                    registers[0x6100ac] |= 1;
                registers[0x6101d4] = 0x11100;
            }
            else
                arm();
        }
    }
    uint32_t read(unsigned offset)
    {
        accesses.push_back(offset);
        if(offset >= 0x700000 && offset < 0x800000)
            return word((registers[0x001700] << 16) + (offset - 0x700000));
        if(offset == 0x640004)
            return get | get_flags;
        if(offset == 0x6100ac && stage1_polls && !--stage1_polls)
            registers[0x6100ac] |= 1;
        if(offset == 0x6100ac && stage3_polls && !--stage3_polls)
        {
            registers[0x6100ac] |= 4;
            registers[0x6101d4] = 0x11100;
        }
        // A hidden active state reads as zero after the module starts the core channel.
        if(offset > 0x640004 && offset < 0x641000)
            return armed_hidden && registers[0x610490] == 0x01000013 ? 0 : armed[offset - 0x640000];
        if(offset >= 0x660000 && offset < 0x661000)
        {
            const auto pending = assembly.find(offset - 0x660000);
            return pending == assembly.end() ? 0 : pending->second;
        }
        return registers[offset];
    }
    void write(unsigned offset, uint32_t value)
    {
        accesses.push_back(offset);
        writes.push_back({offset, value});
        if(offset >= 0x700000 && offset < 0x800000)
        {
            const uint32_t address = (registers[0x001700] << 16) + (offset - 0x700000);
            memory_writes.push_back({address, value});
            word(address) = value;
        }
        else if(offset == 0x070000)
            registers[offset] = 0;
        else if(offset == 0x6194e8)
            registers[offset] = value & 1 ? value : value & ~3u;
        else if(offset == 0x6100ac || offset == 0x61009c)
            registers[offset] &= ~value;
        else if(offset == 0x6101f0 && resume_put != ~0u)
        {
            registers[offset] = value;
            get = resume_get;
            const uint32_t put = resume_put;
            resume_put = ~0u;
            fetch(put);
        }
        else if(offset == 0x6101d0 && pending_update)
        {
            // Three supervisor stages complete one update, as the third desktop test showed.
            // The armed state shows the new values after stage 2. Stage 3 starts later.
            if(supervisor_stage == 0)
            {
                registers[0x6100ac] |= 2;
                registers[0x6101d4] = 0x11100;
            }
            else if(supervisor_stage == 1)
            {
                for(const auto& [method, value] : assembly)
                    armed[method] = value;
                stage3_polls = stage3_never ? 0 : 300;
            }
            else
                pending_update = false;
            ++supervisor_stage;
        }
        else if(offset == 0x640000)
            fetch(value);
        else if(offset >= 0x610560 && offset < 0x6105a0 && !cursor_channel_dead)
            // The position channel of a head reports the started state.
            registers[offset] = value | 0x00010000;
        else if(offset == 0x610490 && value == 0x01000013)
        {
            // A new core channel keeps the pending state but drops the timing values, unless the
            // test keeps them.
            assembly = armed;
            assembly[head_method(0x6ec)] = 0x85104806;
            for(const auto& entry : firmware)
                if(entry.timing && !retains_pending)
                    assembly.erase(head_method(entry.method));
            registers[offset] = value;
        }
        else
            registers[offset] = value;
    }
    gm206::ProbeIo io()
    {
        return {this,
                [](void* self, unsigned offset) {
                    return static_cast<EngineFixture*>(self)->read(offset);
                },
                [](void* self, unsigned offset, uint32_t value) {
                    static_cast<EngineFixture*>(self)->write(offset, value);
                },
                [](uint64_t) {}};
    }
    bool show(const gm206::Surface& surface)
    {
        if(!installed)
            for(const auto& entry : firmware)
                armed[head_method(entry.method)] = entry.value;
        installed = true;
        return gm206::scanout(io(), snapshot, {instance, push, check, &check_word}, surface, state);
    }
    unsigned count(unsigned offset) const
    {
        return unsigned(std::count_if(writes.begin(), writes.end(), [&](const auto& write) {
            return write.first == offset;
        }));
    }
};
constexpr gm206::Surface native_surface{0x04000000, 3840, 2160, 15360};
constexpr gm206::Surface console_surface{0x06000000, 1024, 768, 4096};
}  // namespace

TEST(Gm206, TakesDisplayAndShowsNativeSurface)
{
    EngineFixture engine;
    ASSERT_TRUE(engine.show(native_surface));
    EXPECT_EQ(engine.state.stage, gm206::ScanoutStage::Ready);
    EXPECT_TRUE(engine.state.owned);
    EXPECT_TRUE(engine.state.confirmed);
    EXPECT_EQ(engine.state.supervisor_requests, 0u);
    // Firmware released the display and the engine uses the module instance and command page.
    EXPECT_EQ(engine.registers[0x6100ac] & 0x100, 0u);
    EXPECT_EQ(engine.registers[0x6194e8] & 1, 0u);
    EXPECT_EQ(engine.registers[0x610010], 0x064009u);
    EXPECT_EQ(engine.registers[0x610494], 0x064101u);
    EXPECT_EQ(engine.registers[0x610498], 0x00010000u);
    EXPECT_EQ(engine.registers[0x61049c], 1u);
    EXPECT_EQ(engine.registers[0x610490], 0x01000013u);
    EXPECT_EQ(engine.registers[0x6100b0], 0x307u);
    EXPECT_EQ(engine.registers[0x000140], 0u);
    EXPECT_EQ(engine.registers[0x6101b4], 0x11u);
    EXPECT_EQ(engine.registers[0x6309c4], 0x22u);
    EXPECT_EQ(engine.registers[0x001700], 0x200u);
    // Hash entry 0x1f names the memory object at instance offset 0x1000.
    EXPECT_EQ(engine.memory[EngineFixture::instance + 0xf8], 0xf0000001u);
    EXPECT_EQ(engine.memory[EngineFixture::instance + 0xfc], 0x00200001u);
    EXPECT_EQ(engine.memory[EngineFixture::instance + 0x1000], 0x49u);
    EXPECT_EQ(engine.memory[EngineFixture::instance + 0x1004], 0u);
    EXPECT_EQ(engine.memory[EngineFixture::instance + 0x1008], 0x7fffffu);
    // Armed head state.
    EXPECT_EQ(engine.armed[0x460], 0x040000u);
    EXPECT_EQ(engine.armed[0x468], 0x08700f00u);
    EXPECT_EQ(engine.armed[0x46c], 0x01003c00u);
    EXPECT_EQ(engine.armed[0x470], 0xcf00u);
    EXPECT_EQ(engine.armed[0x474], 0xf0000001u);
    EXPECT_EQ(engine.armed[0x4b8], 0x08700f00u);
    EXPECT_EQ(engine.armed[0x4c0], 0x08700f00u);
    EXPECT_EQ(engine.armed[0x4d0], 0u);
    EXPECT_EQ(engine.armed[0x4d4], 0x100u);
    // The new channel dropped the timing values. The module sent the firmware values again.
    // It also sent the clock limit. Bit 0 is the SOR control. Bit 4 is method 0x414.
    EXPECT_EQ(engine.state.restored, 10u);
    EXPECT_EQ(engine.state.restored_mask, 0x1df3u);
    for(const auto& entry : EngineFixture::firmware)
        if(entry.timing && entry.method != 0x454)
            EXPECT_EQ(engine.armed[entry.method], entry.value) << entry.method;
    // The clock configuration has no firmware flag. The clock limit is equal to the clock.
    EXPECT_EQ(engine.armed[0x454], 0x00200000u);
    EXPECT_EQ(engine.armed[0x458], 0x1fc8bfd0u);
    EXPECT_EQ(engine.armed[0x408], 0x21ec4000u);
    // The module sends no method for a value that the firmware did not set.
    for(const unsigned method : {0x200u, 0x240u, 0x18cu + 4, 0x6f0u})
        EXPECT_FALSE(engine.assembly.contains(method)) << method;
    EXPECT_EQ(engine.registers[0x6101f0], 0u);
    // The aperture check completes before the first display register write.
    const auto display = [](const auto& write) {
        return write.first >= 0x610000 && write.first < 0x700000;
    };
    const auto checked =
        std::find_if(engine.writes.begin(), engine.writes.end(), [](const auto& w) {
            return w.first >= 0x700000 && w.second == 0x6d32064fu;
        });
    ASSERT_NE(checked, engine.writes.end());
    EXPECT_EQ(std::find_if(engine.writes.begin(), checked, display), checked);
    EXPECT_NE(std::find_if(checked, engine.writes.end(), display), engine.writes.end());
    EXPECT_EQ(engine.check_word, ~0x6d32064fu);
}

TEST(Gm206, SendsOnlyChangedValuesIfThePendingStateHoldsTheTiming)
{
    EngineFixture engine;
    engine.retains_pending = true;
    ASSERT_TRUE(engine.show(native_surface));
    // The clock configuration without the firmware flag, and the clock limit.
    EXPECT_EQ(engine.state.restored, 2u);
    EXPECT_EQ(engine.state.restored_mask, 0x1800u);
    EXPECT_EQ(engine.armed[0x220], 0x901u);
    EXPECT_EQ(engine.armed[0x454], 0x00200000u);
    EXPECT_EQ(engine.armed[0x470], 0xcf00u);
}

TEST(Gm206, ShowsMovesAndHidesTheHardwarePointer)
{
    EngineFixture engine;
    // The pointer needs a surface on the head.
    EXPECT_FALSE(engine.point(10, 10, true));
    EXPECT_TRUE(engine.accesses.empty());
    ASSERT_TRUE(engine.show(console_surface));
    // A request to hide a hidden pointer accesses no register.
    const auto accesses = engine.accesses.size();
    ASSERT_TRUE(engine.point(1, 1, false));
    EXPECT_EQ(engine.accesses.size(), accesses);
    ASSERT_TRUE(engine.point(512, 384, true));
    // The image is 64 x 64 pixels. The tip is opaque black and the next pixel is transparent.
    EXPECT_EQ(engine.memory[EngineFixture::cursor], 0xff000000u);
    EXPECT_EQ(engine.memory[EngineFixture::cursor + 4], 0u);
    EXPECT_EQ(engine.memory[EngineFixture::cursor + 2 * 64 * 4 + 4], 0xffffffffu);
    EXPECT_EQ(engine.memory[EngineFixture::cursor + 63 * 64 * 4 + 63 * 4], 0u);
    // The position channel of head 0 runs. The position is in output pixels of 3840 x 2160.
    EXPECT_EQ(engine.registers[0x610560], 0x00010001u);
    EXPECT_EQ(engine.registers[0x64d084], 1080u << 16 | 1920u);
    EXPECT_EQ(engine.count(0x64d080), 1u);
    EXPECT_EQ(engine.armed[0x480], 0x85000000u);
    EXPECT_EQ(engine.armed[0x484], EngineFixture::cursor >> 8);
    EXPECT_EQ(engine.armed[0x48c], 0xf0000001u);
    EXPECT_TRUE(engine.state.cursor_visible);
    // A move writes only the position. It sends no command.
    const uint32_t put = engine.state.put;
    ASSERT_TRUE(engine.point(1023, 767, true));
    EXPECT_EQ(engine.registers[0x64d084], (767u * 2160 / 768) << 16 | (1023u * 3840 / 1024));
    EXPECT_EQ(engine.state.put, put);
    EXPECT_EQ(engine.count(0x610560), 1u);
    EXPECT_FALSE(engine.point(1024, 0, true));
    ASSERT_TRUE(engine.point(0, 0, false));
    EXPECT_EQ(engine.armed[0x480], 0x05000000u);
    EXPECT_EQ(engine.armed[0x48c], 0u);
    // A new surface hides the pointer. The next request shows it again.
    ASSERT_TRUE(engine.point(1, 1, true));
    ASSERT_TRUE(engine.show(native_surface));
    EXPECT_FALSE(engine.state.cursor_visible);
    EXPECT_EQ(engine.armed[0x480], 0x05000000u);
    ASSERT_TRUE(engine.point(3839, 2159, true));
    EXPECT_EQ(engine.registers[0x64d084], 2159u << 16 | 3839u);
    EXPECT_EQ(engine.armed[0x480], 0x85000000u);
    EXPECT_EQ(engine.registers[0x001700], 0x200u);
    for(const auto offset : engine.accesses)
    {
        bool mapped = false;
        for(const auto& range : gm206::register_ranges)
            mapped |= offset >= range.offset && offset - range.offset <= range.bytes - 4;
        EXPECT_TRUE(mapped) << "unmapped access " << offset;
    }
}

TEST(Gm206, ReportsAPointerChannelThatDoesNotStart)
{
    EngineFixture engine;
    engine.cursor_channel_dead = true;
    ASSERT_TRUE(engine.show(native_surface));
    EXPECT_FALSE(engine.point(1, 1, true));
    EXPECT_FALSE(engine.state.cursor_visible);
    EXPECT_FALSE(engine.armed.contains(0x484));
    // The surface stays in use.
    ASSERT_TRUE(engine.show(console_surface));
}

TEST(Gm206, KeepsFirmwareClockLimitAndAddressesTheBootSor)
{
    EngineFixture engine;
    engine.snapshot.boot.sor = 3;
    engine.armed[0x458] = 0x20000000;
    ASSERT_TRUE(engine.show(native_surface));
    // The module kept the firmware limit. It sent the control of SOR 3, not of SOR 1.
    EXPECT_EQ(engine.armed[0x458], 0x20000000u);
    EXPECT_EQ(engine.armed[0x260], 0x901u);
    EXPECT_FALSE(engine.assembly.contains(0x220));
    EXPECT_EQ(engine.state.restored_mask, 0x0df3u);
}

TEST(Gm206, SendsFirmwareValuesOnlyWithTheFirstUpdate)
{
    EngineFixture engine;
    // Values outside the kept methods. The second desktop test showed such differences.
    engine.armed[0x438] = 0xf0000002;
    engine.armed[0x440] = 0x80000000;
    engine.armed[0x4bc] = 0x00010002;
    ASSERT_TRUE(engine.show(native_surface));
    EXPECT_EQ(engine.state.restored, 10u);
    const auto sent = [&](uint32_t header) {
        unsigned count = 0;
        for(const auto& [address, value] : engine.memory_writes)
            count += address >= EngineFixture::push && address < EngineFixture::push + 0x1000 &&
                     value == header;
        return count;
    };
    EXPECT_EQ(sent(1u << 18 | 0x220), 1u);
    for(const unsigned method : {0x438u, 0x440u, 0x4bcu, 0x18cu, 0x43cu, 0x6ecu})
        EXPECT_EQ(sent(1u << 18 | method), 0u) << method;
    // The active state now reads as zero. A later update must not send it as firmware state.
    engine.armed_hidden = true;
    ASSERT_TRUE(engine.show(console_surface));
    ASSERT_TRUE(engine.show(native_surface));
    EXPECT_EQ(sent(1u << 18 | 0x220), 1u);
    EXPECT_EQ(engine.state.restored, 10u);
}

TEST(Gm206, RefusesTranslatedApertureBeforeDisplayWrites)
{
    EngineFixture engine;
    engine.translated = true;
    EXPECT_FALSE(engine.show(native_surface));
    EXPECT_EQ(engine.state.stage, gm206::ScanoutStage::Aperture);
    EXPECT_FALSE(engine.state.owned);
    // The module did not write through BAR 1.
    EXPECT_EQ(engine.check_word, 0u);
    for(const auto& [offset, value] : engine.writes)
        EXPECT_TRUE(offset == 0x001700 || offset == 0x070000 || offset >= 0x700000) << offset;
    EXPECT_EQ(engine.registers[0x6100ac], 0x100u);
    EXPECT_EQ(engine.registers[0x001700], 0x200u);
}

TEST(Gm206, AcknowledgesSupervisorStagesWithoutClockOrLinkWrites)
{
    EngineFixture engine;
    engine.supervisor = true;
    ASSERT_TRUE(engine.show(native_surface));
    EXPECT_EQ(engine.state.supervisor_requests, 0x124u);
    EXPECT_EQ(engine.state.supervisor_head, 0x11100u);
    EXPECT_EQ(engine.state.supervisor_stage[0], 0x11100u);
    EXPECT_EQ(engine.state.supervisor_stage[1], 0x11100u);
    EXPECT_EQ(engine.state.supervisor_stage[2], 0x11100u);
    EXPECT_LT(engine.state.stage_poll[0], engine.state.stage_poll[1]);
    EXPECT_GT(engine.state.stage_poll[2], engine.state.stage_poll[1] + 100);
    EXPECT_GE(engine.state.polls, engine.state.stage_poll[2]);
    // The module waited for the late third stage. No request stays pending.
    EXPECT_EQ(engine.state.stages, 7u);
    EXPECT_TRUE(engine.state.confirmed);
    EXPECT_EQ(engine.registers[0x6100ac] & 7, 0u);
    EXPECT_FALSE(engine.pending_update);
    // A later call without a supervisor request does not repeat the stage values.
    engine.supervisor = false;
    ASSERT_TRUE(engine.show(console_surface));
    EXPECT_EQ(engine.state.supervisor_stage[0], 0u);
    EXPECT_EQ(engine.count(0x6101d0), 3u);
    EXPECT_EQ(engine.armed[0x474], 0xf0000001u);
    for(const auto& [offset, value] : engine.writes)
    {
        // No PLL, SOR clock, SOR route, or DisplayPort register.
        EXPECT_FALSE(offset >= 0x612200 && offset < 0x616000) << offset;
        EXPECT_FALSE(offset >= 0x61c000 && offset < 0x61e000) << offset;
        EXPECT_FALSE(offset >= 0x00d000 && offset < 0x00f000) << offset;
        EXPECT_FALSE(offset >= 0x137000 && offset < 0x138000) << offset;
    }
}

TEST(Gm206, DoesNotTakeAnEarlierStageThreeAsTheEndOfTheUpdate)
{
    // The third desktop test left stage 3 of one update pending at the start of the next call.
    for(const bool early : {false, true})
    {
        EngineFixture engine;
        engine.supervisor = true;
        engine.registers[0x6100ac] |= 4;
        engine.stale_before_stage1 = early;
        ASSERT_TRUE(engine.show(native_surface)) << early;
        EXPECT_FALSE(engine.pending_update) << early;
        EXPECT_EQ(engine.registers[0x6100ac] & 7, 0u) << early;
        EXPECT_EQ(engine.state.stages, 7u) << early;
        // Two consecutive updates with supervisor stages complete.
        ASSERT_TRUE(engine.show(console_surface)) << early;
        EXPECT_FALSE(engine.pending_update) << early;
        EXPECT_EQ(engine.armed[0x460], console_surface.offset >> 8) << early;
    }
}

TEST(Gm206, FailsIfTheLastSupervisorStageDoesNotStart)
{
    EngineFixture engine;
    engine.supervisor = true;
    engine.stage3_never = true;
    EXPECT_FALSE(engine.show(native_surface));
    EXPECT_EQ(engine.state.stage, gm206::ScanoutStage::Update);
    EXPECT_EQ(engine.state.stages, 3u);
    EXPECT_FALSE(engine.state.confirmed);
    EXPECT_EQ(engine.registers[0x001700], 0x200u);
}

TEST(Gm206, KeepsTheFirstErrorAndDoesNotRecoverFromARefusedMethod)
{
    EngineFixture engine;
    ASSERT_TRUE(engine.show(console_surface));
    // The engine refuses the surface size and then also the UPDATE of the same request.
    engine.refused_method = 0x468;
    engine.command_error = true;
    EXPECT_FALSE(engine.show(native_surface));
    EXPECT_EQ(engine.state.stage, gm206::ScanoutStage::Command);
    // The words of the first error identify the cause.
    EXPECT_EQ(engine.state.error[0], 0x4468u);
    EXPECT_EQ(engine.state.errors, 2u);
    // The display state is not known. The kernel must stop the display service.
    EXPECT_FALSE(engine.state.recoverable);
    EXPECT_EQ(engine.registers[0x61009c], 0u);
    EXPECT_EQ(engine.registers[0x001700], 0x200u);
    // One refused method without a second error is also not recoverable.
    EngineFixture single;
    ASSERT_TRUE(single.show(console_surface));
    single.refused_method = 0x468;
    EXPECT_FALSE(single.show(native_surface));
    EXPECT_EQ(single.state.error[0], 0x4468u);
    EXPECT_EQ(single.state.errors, 1u);
    EXPECT_FALSE(single.state.recoverable);
}

TEST(Gm206, RecoversFromARefusedUpdate)
{
    EngineFixture engine;
    ASSERT_TRUE(engine.show(console_surface));
    engine.command_error = true;
    EXPECT_FALSE(engine.show(native_surface));
    EXPECT_EQ(engine.state.stage, gm206::ScanoutStage::Command);
    EXPECT_EQ(engine.state.error[0], 0x5080u);
    EXPECT_TRUE(engine.state.recoverable);
    // The head keeps the console surface. The next request works and clears the error words.
    EXPECT_EQ(engine.armed[0x460], console_surface.offset >> 8);
    EXPECT_EQ(engine.get, engine.state.put);
    engine.command_error = false;
    ASSERT_TRUE(engine.show(native_surface));
    EXPECT_EQ(engine.state.error[0], 0u);
    EXPECT_FALSE(engine.state.recoverable);
    EXPECT_EQ(engine.armed[0x460], native_surface.offset >> 8);
}

TEST(Gm206, ReportsCommandErrorAndStops)
{
    EngineFixture engine;
    engine.command_error = true;
    EXPECT_FALSE(engine.show(native_surface));
    EXPECT_EQ(engine.state.stage, gm206::ScanoutStage::Command);
    EXPECT_EQ(engine.state.error[0], 0x5080u);
    EXPECT_EQ(engine.state.error[1], 0x1234u);
    EXPECT_EQ(engine.registers[0x61009c], 0u);
    EXPECT_EQ(engine.registers[0x6101f0], 0x90000000u);
    EXPECT_EQ(engine.registers[0x001700], 0x200u);
}

TEST(Gm206, ReusesChannelAndWrapsCommandPage)
{
    EngineFixture engine;
    for(unsigned i = 0; i < 64; ++i)
    {
        const auto& surface = i % 2 ? console_surface : native_surface;
        ASSERT_TRUE(engine.show(surface)) << i;
        EXPECT_EQ(engine.armed[0x460], surface.offset >> 8);
        EXPECT_EQ(engine.armed[0x468], surface.height << 16 | surface.width);
        EXPECT_EQ(engine.armed[0x46c], 0x01000000 | surface.pitch);
        EXPECT_EQ(engine.armed[0x4b8], surface.height << 16 | surface.width);
        EXPECT_EQ(engine.armed[0x4c0], 0x08700f00u);
        EXPECT_LE(engine.state.put, 0xf00u);
        EXPECT_EQ(engine.get, engine.state.put);
    }
    // One ownership change and one channel start serve all updates.
    EXPECT_EQ(engine.count(0x6194e8), 1u);
    EXPECT_EQ(engine.count(0x610494), 1u);
    EXPECT_EQ(engine.count(0x610010), 1u);
}

TEST(Gm206, RejectsUnsupportedScanoutRequestsWithoutRegisterAccess)
{
    const auto rejected = [](auto change) {
        EngineFixture engine;
        gm206::Surface surface = native_surface;
        change(engine, surface);
        EXPECT_FALSE(engine.show(surface));
        EXPECT_EQ(engine.state.stage, gm206::ScanoutStage::Precondition);
        EXPECT_TRUE(engine.accesses.empty());
    };
    rejected([](auto& engine, auto&) { engine.snapshot.boot.route_matched = false; });
    rejected([](auto& engine, auto&) { engine.snapshot.boot.head[15] = 0; });
    rejected([](auto& engine, auto&) { engine.snapshot.boot.head_index = gm206::heads; });
    rejected([](auto& engine, auto&) { engine.snapshot.boot.core[13] = 0; });
    rejected([](auto& engine, auto&) { engine.snapshot.boot.core[9] = 0x80000000; });
    rejected([](auto&, auto& surface) { surface.width = 3841; });
    rejected([](auto&, auto& surface) { surface.height = 2161; });
    rejected([](auto&, auto& surface) { surface.pitch = 15361; });
    rejected([](auto&, auto& surface) { surface.pitch = 3840 * 4 - 256; });
    rejected([](auto&, auto& surface) { surface.offset = 0x04000100; });
    rejected([](auto&, auto& surface) { surface.offset = 0x7f000000; });
}

TEST(Gm206, AcceptsUpdateWithoutReadableArmedStateOrCleanGetBits)
{
    // Nouveau reads only bits 11:2 of GET and does not read the armed state for completion.
    EngineFixture engine;
    engine.armed_hidden = true;
    engine.get_flags = 0x80000000;
    ASSERT_TRUE(engine.show(native_surface));
    EXPECT_EQ(engine.state.stage, gm206::ScanoutStage::Ready);
    EXPECT_FALSE(engine.state.confirmed);
    EXPECT_EQ(engine.assembly[0x474], 0xf0000001u);
}

TEST(Gm206, AddressesMethodsOfTheBootHead)
{
    EngineFixture engine;
    engine.snapshot.boot.head_index = 1;
    ASSERT_TRUE(engine.show(native_surface));
    EXPECT_TRUE(engine.state.confirmed);
    EXPECT_EQ(engine.armed[0x460 + 0x300], 0x040000u);
    EXPECT_EQ(engine.armed[0x474 + 0x300], 0xf0000001u);
    EXPECT_EQ(engine.armed[0x4c8 + 0x300], 0x08700f00u);
    EXPECT_EQ(engine.state.restored, 10u);
    EXPECT_EQ(engine.armed[0x414 + 0x300], 0x08ae0fa0u);
    EXPECT_EQ(engine.armed[0x454 + 0x300], 0x00200000u);
    EXPECT_FALSE(engine.armed.contains(0x460));
}

TEST(Gm206, ScanoutUsesOnlyMappedRegisterRanges)
{
    EngineFixture engine;
    engine.supervisor = true;
    ASSERT_TRUE(engine.show(native_surface));
    ASSERT_TRUE(engine.show(console_surface));
    for(const auto offset : engine.accesses)
    {
        bool mapped = false;
        for(const auto& range : gm206::register_ranges)
            mapped |= offset >= range.offset && offset - range.offset <= range.bytes - 4;
        EXPECT_TRUE(mapped) << "unmapped access " << offset;
    }
}

TEST(Gm206, OffersTheNativeModeAndSmallerMonitorModesAsScaledModes)
{
    gm206::Snapshot snapshot{};
    snapshot.boot.route_matched = true;
    snapshot.boot.head[gm206::head_viewport_out] = 0x08700f00;
    auto& sink = snapshot.outputs[1];
    const gm206::Mode sizes[] = {{3840, 2160},
                                 {2560, 1440},
                                 {1920, 1080},
                                 {1024, 768},
                                 {800, 600},
                                 {1920, 1080},
                                 {1280, 720},
                                 {4096, 2160},
                                 {640, 400},
                                 {1600, 900}};
    for(const auto& size : sizes)
        sink.modes[sink.mode_count++] = size;
    const os1_framebuffer_surface boot{nullptr, 1024, 768, 4096, 4, 16, 8, 0};
    os1_display_mode modes[12]{};
    // The native mode is first. The list has no size two times and no size of the boot mode.
    // A width of 800 gives a pitch that is not a multiple of 256 bytes.
    ASSERT_EQ(gm206::offer_modes(snapshot, boot, modes, 12), 5u);
    const uint32_t expected[][2] = {
        {3840, 2160}, {2560, 1440}, {1920, 1080}, {1280, 720}, {1600, 900}};
    for(unsigned i = 0; i < 5; ++i)
    {
        EXPECT_EQ(modes[i].width, expected[i][0]) << i;
        EXPECT_EQ(modes[i].height, expected[i][1]) << i;
        EXPECT_EQ(modes[i].bits_per_pixel, 32u) << i;
    }
    // The capacity limits the list.
    EXPECT_EQ(gm206::offer_modes(snapshot, boot, modes, 2), 2u);
    EXPECT_EQ(gm206::offer_modes(snapshot, boot, modes, 0), 0u);
    // A boot state without support gives no mode.
    auto refused = [&](auto change) {
        auto state = snapshot;
        auto surface = boot;
        change(state, surface);
        EXPECT_EQ(gm206::offer_modes(state, surface, modes, 12), 0u);
    };
    refused([](auto& state, auto&) { state.boot.route_matched = false; });
    refused([](auto& state, auto&) { state.boot.head[gm206::head_viewport_out] = 0; });
    refused([](auto& state, auto&) { state.boot.head[gm206::head_viewport_out] = 0x03000400; });
    refused([](auto&, auto& surface) { surface.pixel_bytes = 3; });
    refused([](auto&, auto& surface) { surface.red_shift = 0; });
    refused([](auto&, auto& surface) { surface.pitch = 4100; });
    refused([](auto&, auto& surface) { surface.pitch = 4096 * 1024; });
}

TEST(DisplayReflection, KernelReportsTheGenericNodesWithoutADriverReflection)
{
    const auto ask = [](unsigned operation, unsigned id, const os1_display_summary& display) {
        Os1HardwareQuery query{};
        query.version = OS1_HARDWARE_VERSION;
        query.operation = operation;
        query.id = id;
        const long status = native_display::query_reflection(query, display);
        return std::pair{status, query};
    };
    const os1_display_summary shown{
        true, "bochs-display", "QEMU runtime display", 1024, 768, 32, 3, 1, 0, 0, 0};
    auto [status, root] = ask(OS1_HARDWARE_DESCRIBE, OS1_HARDWARE_DISPLAY_ROOT, shown);
    ASSERT_EQ(status, 0);
    EXPECT_STREQ(root.nodes[0].name, "display");
    EXPECT_STREQ(root.nodes[0].text, "QEMU runtime display");
    auto [listed, children] = ask(OS1_HARDWARE_CHILDREN, OS1_HARDWARE_DISPLAY_ROOT, shown);
    ASSERT_EQ(listed, 0);
    ASSERT_EQ(children.count, 6u);
    EXPECT_TRUE(children.end);
    EXPECT_STREQ(children.nodes[0].name, "available");
    EXPECT_EQ(children.nodes[0].integer, 1u);
    EXPECT_STREQ(children.nodes[1].name, "driver");
    EXPECT_STREQ(children.nodes[1].text, "bochs-display");
    EXPECT_STREQ(children.nodes[5].name, "mode_count");
    EXPECT_EQ(children.nodes[5].integer, 3u);
    for(unsigned i = 0; i < children.count; ++i)
        EXPECT_EQ(children.nodes[i].parent, unsigned(OS1_HARDWARE_DISPLAY_ROOT));
    // The kernel has no node for one display device.
    EXPECT_EQ(ask(OS1_HARDWARE_DESCRIBE, OS1_HARDWARE_GM206_ROOT, shown).first, OS1_ERR_NOT_FOUND);
    // A display that is not available has only the availability node.
    auto stopped = shown;
    stopped.available = false;
    auto [hidden, single] = ask(OS1_HARDWARE_CHILDREN, OS1_HARDWARE_DISPLAY_ROOT, stopped);
    ASSERT_EQ(hidden, 0);
    ASSERT_EQ(single.count, 1u);
    EXPECT_EQ(single.nodes[0].integer, 0u);
    EXPECT_EQ(ask(OS1_HARDWARE_DESCRIBE, OS1_HARDWARE_DISPLAY_ROOT + 2, stopped).first,
              OS1_ERR_NOT_FOUND);
    // A query for a different generation is cancelled.
    Os1HardwareQuery stale{};
    stale.version = OS1_HARDWARE_VERSION;
    stale.operation = OS1_HARDWARE_DESCRIBE;
    stale.id = OS1_HARDWARE_DISPLAY_ROOT;
    stale.generation = 7;
    EXPECT_EQ(native_display::query_reflection(stale, shown), OS1_ERR_CANCELLED);
}

extern "C" const os1_display_driver* os1_driver_init();

namespace
{
// Kernel services for the driver table of the module. The register access goes to a probe model.
struct ModuleHost
{
    static inline ModuleHost* self = nullptr;
    struct Map
    {
        uint32_t bar;
        uint64_t offset, bytes;
        uint32_t flags;
    };
    AuxFixture card;
    uint32_t viewport = 0x08700f00;
    bool present = true;
    // A bit for each BAR that the kernel refuses to give, and for each BAR without a mapping.
    unsigned refused_claims = 0, refused_maps = 0;
    // The BARs that the module holds, as (PCI index, BAR) pairs.
    std::set<std::pair<uint32_t, uint32_t>> held;
    // Releases and mappings of a BAR that the module does not hold.
    unsigned wrong_uses = 0;
    std::vector<Map> maps;
    uint8_t memory[16]{};
    os1_display_pci pci{};
    os1_display_host host{};
    gm206::ProbeIo io{};
    const os1_display_driver* driver = os1_driver_init();

    ModuleHost()
    {
        self = this;
        card.make_edid();
        pci.vendor = 0x10de;
        pci.device = 0x1402;
        pci.class_code = 3;
        pci.command = 2;
        pci.subsystem = 0x36c51458;
        pci.bars[0] = {0xf6000000, 0x1000000, true};
        pci.bars[1] = {0xe0000000, 0x10000000, true};
        pci.bars[3] = {0xf0000000, 0x2000000, true};
        host.version = 1;
        host.bytes = sizeof(host);
        host.boot = {memory, 1024, 768, 4096, 4, 16, 8, 0};
        host.boot_physical = 0xf1000000;
        host.drawing = true;
        host.pci_count = 2;
        host.pci = [](uint32_t index, os1_display_pci* out) {
            // Index 0 is a device of a different vendor.
            *out = index == 1 && self->present ? self->pci : os1_display_pci{};
            return true;
        };
        host.claim = [](uint32_t index, uint32_t bar) {
            return !(self->refused_claims & (1u << bar)) && self->held.insert({index, bar}).second;
        };
        host.release = [](uint32_t index, uint32_t bar) {
            self->wrong_uses += !self->held.erase({index, bar});
        };
        host.map = [](uint32_t index, uint32_t bar, uint64_t offset, uint64_t bytes, uint32_t flags)
                       ->volatile uint8_t*
        {
            self->wrong_uses += !self->held.contains({index, bar});
            self->maps.push_back({bar, offset, bytes, flags});
            return self->refused_maps & (1u << bar) ? nullptr : self->memory;
        };
        host.allocate = [](uint64_t) -> void* { return nullptr; };
        host.delay_us = [](uint64_t) {};
        host.log = [](const char*, const uint32_t*, uint32_t) {};
        // The probe model has no output viewport. The native mode needs one.
        io = {this,
              [](void* p, unsigned reg) {
                  auto& host = *static_cast<ModuleHost*>(p);
                  return reg == 0x6404c0 ? host.viewport : host.card.read(reg);
              },
              [](void* p, unsigned reg, uint32_t value) {
                  static_cast<ModuleHost*>(p)->card.write(reg, value);
              },
              [](uint64_t) {}};
        gm206::test_io = &io;
    }
    ~ModuleHost()
    {
        gm206::test_io = nullptr;
        self = nullptr;
    }
    // The probe status of the reflection tree.
    std::string status() const
    {
        Os1HardwareQuery query{};
        query.version = OS1_HARDWARE_VERSION;
        query.operation = OS1_HARDWARE_DESCRIBE;
        query.id = OS1_HARDWARE_DISPLAY_ROOT + 0x900;
        EXPECT_EQ(driver->reflect(&query, &summary), 0);
        return query.nodes[0].text;
    }
};
}  // namespace

TEST(Gm206Module, OffersModesOnTheSupportedBoardAndKeepsItsClaims)
{
    ModuleHost machine;
    ASSERT_TRUE(machine.driver->probe(&machine.host));
    EXPECT_EQ(machine.status(), "probe complete; native mode offered");
    EXPECT_STREQ(machine.driver->text(0), "gm206");
    os1_display_mode mode{};
    ASSERT_TRUE(machine.driver->mode(0, &mode));
    EXPECT_EQ(mode.width, 3840u);
    EXPECT_EQ(mode.height, 2160u);
    EXPECT_EQ(mode.bits_per_pixel, 32u);
    // The module keeps BAR 0 and BAR 1.
    const std::set<std::pair<uint32_t, uint32_t>> both{{1, 0}, {1, 1}};
    EXPECT_EQ(machine.held, both);
    EXPECT_EQ(machine.wrong_uses, 0u);
    // It maps each register range, then the surfaces with write combining, then the rest.
    const auto ranges = std::size(gm206::register_ranges);
    ASSERT_EQ(machine.maps.size(), ranges + 2);
    for(unsigned i = 0; i < ranges; ++i)
    {
        EXPECT_EQ(machine.maps[i].bar, 0u);
        EXPECT_EQ(machine.maps[i].offset, gm206::register_ranges[i].offset);
        EXPECT_EQ(machine.maps[i].bytes, gm206::register_ranges[i].bytes);
        EXPECT_EQ(machine.maps[i].flags, 0u);
    }
    EXPECT_EQ(machine.maps[ranges].bar, 1u);
    EXPECT_EQ(machine.maps[ranges].offset, 0x04000000u);
    EXPECT_EQ(machine.maps[ranges].bytes, 0x02400000u);
    EXPECT_EQ(machine.maps[ranges].flags, uint32_t(OS1_DISPLAY_MAP_WRITE_COMBINE));
    EXPECT_EQ(machine.maps[ranges + 1].bar, 1u);
    EXPECT_EQ(machine.maps[ranges + 1].offset, 0x06400000u);
    EXPECT_EQ(machine.maps[ranges + 1].bytes, 0x16000u);
    EXPECT_EQ(machine.maps[ranges + 1].flags, 0u);
    // A request outside the mode list changes nothing. No surface is on the head.
    os1_framebuffer_surface surface{};
    EXPECT_EQ(machine.driver->set_mode(99, &surface), OS1_DISPLAY_REFUSED);
    EXPECT_FALSE(machine.driver->cursor(1, 1, true));
    // Before the first module surface, the console keeps the boot framebuffer.
    EXPECT_EQ(machine.driver->restore(&surface), OS1_DISPLAY_DONE);
    EXPECT_EQ(surface.pixels, machine.memory);
    EXPECT_EQ(surface.width, 1024u);
}

TEST(Gm206Module, ReportsEachProbeFailureAndReturnsItsClaims)
{
    const auto run = [](auto change, const char* status, bool found = true) {
        ModuleHost machine;
        change(machine);
        EXPECT_EQ(machine.driver->probe(&machine.host), found) << status;
        if(found)
            EXPECT_EQ(machine.status(), status);
        os1_display_mode mode{};
        EXPECT_FALSE(machine.driver->mode(0, &mode)) << status;
        EXPECT_TRUE(machine.held.empty()) << status;
        EXPECT_EQ(machine.wrong_uses, 0u) << status;
        return machine.maps.size();
    };
    run([](auto& machine) { machine.present = false; }, "", false);
    EXPECT_EQ(run([](auto& machine) { machine.pci.subsystem = 0x36c51043; },
                  "unsupported subsystem or MMIO disabled"),
              0u);
    run([](auto& machine) { machine.pci.command = 0; }, "unsupported subsystem or MMIO disabled");
    run([](auto& machine) { machine.pci.bars[0].bytes = 0x400000; }, "invalid BAR 0");
    run([](auto& machine) { machine.refused_claims = 1; }, "BAR 0 claim or mapping failed");
    EXPECT_EQ(run([](auto& machine) { machine.refused_maps = 1; }, "BAR 0 claim or mapping failed"),
              1u);
    run([](auto& machine) { machine.card.identity = 0x125000a1; }, "chip identity rejected");
    // The module reads the card but offers no mode. It then keeps no BAR.
    const auto ranges = std::size(gm206::register_ranges);
    EXPECT_EQ(run([](auto& machine) { machine.pci.bars[1].bytes = 0x4000000; },
                  "probe complete; BAR 1 not usable"),
              ranges);
    EXPECT_EQ(run([](auto& machine) { machine.host.drawing = false; },
                  "probe complete; no drawing driver"),
              ranges);
    // The boot framebuffer is on a different display device, or one byte outside a BAR.
    for(const uint64_t address : {0xc0000000ull, 0xe0000000ull - 1, 0xf2000000ull, 0ull})
        EXPECT_EQ(run([&](auto& machine) { machine.host.boot_physical = address; },
                      "probe complete; boot framebuffer not on the card"),
                  ranges)
            << address;
    EXPECT_EQ(
        run([](auto& machine) { machine.refused_claims = 2; }, "probe complete; BAR 1 not usable"),
        ranges);
    EXPECT_EQ(run([](auto& machine) { machine.refused_maps = 2; },
                  "probe complete; BAR 1 mapping failed"),
              ranges + 1);
    EXPECT_EQ(
        run([](auto& machine) { machine.viewport = 0; }, "probe complete; native mode not offered"),
        ranges + 2);
    EXPECT_EQ(run([](auto& machine) { machine.host.boot.pixel_bytes = 3; },
                  "probe complete; native mode not offered"),
              ranges + 2);
    // The first and the last byte of BAR 3 are on the card.
    for(const uint64_t address : {0xf0000000ull, 0xf2000000ull - 1})
    {
        ModuleHost machine;
        machine.host.boot_physical = address;
        ASSERT_TRUE(machine.driver->probe(&machine.host));
        EXPECT_EQ(machine.status(), "probe complete; native mode offered") << address;
    }
}
