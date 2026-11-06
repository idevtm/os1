#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "drivers/usb/mass_storage.hpp"

namespace
{
using namespace usb::storage;
uint32_t le32(const uint8_t* p)
{
    return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void le(uint8_t* p, uint32_t n)
{
    for(unsigned i = 0; i < 4; ++i)
        p[i] = n >> (8 * i);
}
void be(uint8_t* p, uint32_t n)
{
    for(unsigned i = 0; i < 4; ++i)
        p[i] = n >> (24 - 8 * i);
}
struct WireDevice
{
    enum Fault
    {
        None,
        Signature,
        Tag,
        Phase,
        ShortStatus,
        Residue,
        ShortData,
        ConsistentShortData,
        FailedData,
        StallStatus,
        StallData,
        CommandFailure
    } fault = None;
    std::array<uint8_t, 31> cbw{};
    std::array<uint8_t, 4096> disk{};
    unsigned stage = 0, commands = 0, resets = 0, clears = 0;
    bool readonly = false, no_flush = false, unit_attention = false, sense_error = false;
    bool no_mode = false;
    uint8_t fail_data_opcode = 0xff;
    uint8_t sense_key = 6, sense_asc = 0, sense_ascq = 0, sense_format = 0x70;
    unsigned sense_bytes = 18, cache_bytes = 24;
    bool cache_rejected = false, fail_flush_transport = false;
    uint32_t response_residue = 0;
    std::array<uint8_t, 64> cache_mode{};
    uint32_t sector_bytes = 512;
    std::vector<uint8_t> opcodes;
    static Transfer bulk(void* context, bool in, void* data, uint32_t size)
    {
        auto& f = *static_cast<WireDevice*>(context);
        auto* p = static_cast<uint8_t*>(data);
        if(f.stage == 0)
        {
            EXPECT_FALSE(in);
            EXPECT_EQ(size, 31u);
            std::memcpy(f.cbw.data(), p, 31);
            EXPECT_EQ(le32(p), 0x43425355u);
            EXPECT_EQ(p[13], 0);
            EXPECT_EQ(p[14],
                      p[15] == 0x28 || p[15] == 0x2a || p[15] == 0x25 || p[15] == 0x35 ? 10 : 6);
            ++f.commands;
            f.opcodes.push_back(p[15]);
            f.response_residue = 0;
            if(f.fail_flush_transport && p[15] == 0x35)
                return {TransferStatus::Failed, 0};
            f.stage = le32(p + 8) ? 1 : 2;
            return {TransferStatus::Ok, size};
        }
        uint8_t op = f.cbw[15];
        if(f.stage == 1)
        {
            f.stage = 2;
            EXPECT_EQ(size, le32(f.cbw.data() + 8));
            EXPECT_EQ(in, bool(f.cbw[12] & 0x80));
            if(f.fault == FailedData || op == f.fail_data_opcode)
                return {TransferStatus::Failed, 0};
            if(f.fault == StallData)
                return {TransferStatus::Stall, 0};
            if(in)
                std::memset(p, 0, size);
            if(op == 0x12)
            {
                p[0] = 0;
                p[4] = 31;
            }
            if(op == 0x25)
            {
                be(p, 65535);
                be(p + 4, f.sector_bytes);
            }
            if(op == 0x1a)
                p[2] = f.readonly ? 0x80 : 0;
            if(op == 3)
            {
                p[0] = f.sense_format;
                if(f.sense_format == 0x72)
                {
                    p[1] = f.sense_key;
                    p[2] = f.sense_asc;
                    p[3] = f.sense_ascq;
                }
                else
                {
                    p[2] = f.sense_error ? 3 : f.sense_key;
                    p[7] = 10;
                    p[12] = f.sense_asc;
                    p[13] = f.sense_ascq;
                }
                const auto actual = std::min<unsigned>(size, f.sense_bytes);
                f.response_residue = size - actual;
                return {TransferStatus::Ok, actual};
            }
            if(op == 0x1a && f.cbw[17] == 8)
            {
                const auto actual = std::min<unsigned>(size, f.cache_bytes);
                std::memcpy(p, f.cache_mode.data(), actual);
                f.response_residue = size - actual;
                return {TransferStatus::Ok, actual};
            }
            if(op == 0x28)
                std::memcpy(p, f.disk.data(), size);
            if(op == 0x2a)
                std::memcpy(f.disk.data(), p, size);
            if(f.fault == ConsistentShortData)
                f.response_residue = 1;
            return {TransferStatus::Ok,
                    f.fault == ShortData || f.fault == ConsistentShortData ? size - 1 : size};
        }
        EXPECT_TRUE(in);
        EXPECT_EQ(size, 13u);
        if(f.fault == StallStatus)
        {
            f.fault = None;
            return {TransferStatus::Stall, 0};
        }
        f.stage = 0;
        std::memset(p, 0, 13);
        le(p, f.fault == Signature ? 0 : 0x53425355);
        le(p + 4, le32(f.cbw.data() + 4) + (f.fault == Tag));
        le(p + 8, f.fault == Residue ? le32(f.cbw.data() + 8) + 1 : f.response_residue);
        p[12] = f.fault == Phase ? 2
                                 : (f.fault == CommandFailure || (f.no_flush && op == 0x35) ||
                                    (f.no_mode && op == 0x1a) || (f.unit_attention && op == 0) ||
                                    (f.cache_rejected && op == 0x1a && f.cbw[17] == 8));
        if(f.unit_attention && op == 0)
            f.unit_attention = false;
        return {TransferStatus::Ok, f.fault == ShortStatus ? 12u : 13u};
    }
    static bool clear(void* context, bool)
    {
        ++static_cast<WireDevice*>(context)->clears;
        return true;
    }
    static bool reset(void* context)
    {
        auto& f = *static_cast<WireDevice*>(context);
        ++f.resets;
        f.stage = 0;
        return true;
    }
    Transport transport()
    {
        return {this, bulk, clear, reset};
    }
};
TEST(UsbStorage, InitializesWritableGeometryAndRequiresFlush)
{
    WireDevice wire;
    Device device;
    ASSERT_TRUE(device.initialize(wire.transport()));
    EXPECT_STREQ(device.probe_stage, "storage-ready");
    EXPECT_EQ(device.probe_opcode, 0x35);
    EXPECT_EQ(device.probe_result, Result::Ok);
    EXPECT_EQ(device.sectors, 65536u);
    EXPECT_EQ(wire.opcodes, (std::vector<uint8_t>{0x12, 0, 0x25, 0x1a, 0x35}));
    std::array<uint8_t, 4096> written{}, read{};
    for(size_t i = 0; i < written.size(); ++i)
        written[i] = i * 37;
    ASSERT_TRUE(device.read_write(true, 17, written.data(), 8));
    EXPECT_EQ(wire.cbw[17], 0);
    EXPECT_EQ(wire.cbw[20], 17);
    EXPECT_EQ(wire.cbw[23], 8);
    ASSERT_TRUE(device.flush());
    ASSERT_TRUE(device.read_write(false, 17, read.data(), 8));
    EXPECT_EQ(read, written);
    unsigned calls = wire.commands;
    EXPECT_FALSE(device.read_write(false, device.sectors, read.data(), 1));
    EXPECT_FALSE(device.read_write(false, device.sectors - 1, read.data(), 2));
    EXPECT_FALSE(device.read_write(false, 0, read.data(), 9));
    EXPECT_EQ(wire.commands, calls);
}
TEST(UsbStorage, RejectsUnsupportedGeometryReadonlyAndMissingDurability)
{
    for(unsigned scenario = 0; scenario < 3; ++scenario)
    {
        WireDevice wire;
        Device device;
        wire.sector_bytes = scenario == 0 ? 4096 : 512;
        wire.readonly = scenario == 1;
        wire.no_flush = scenario == 2;
        EXPECT_FALSE(device.initialize(wire.transport()));
        EXPECT_FALSE(device.alive);
        EXPECT_STREQ(device.probe_stage,
                     scenario == 0   ? "storage-capacity"
                     : scenario == 1 ? "storage-write-protect"
                                     : "storage-flush-rejected");
        EXPECT_EQ(device.probe_result, Result::Ok);
        EXPECT_EQ(device.probe_opcode, scenario == 0 ? 0x25 : scenario == 1 ? 0x1a : 3);
        EXPECT_EQ(device.probe_mode, scenario == 1 ? 0x80 : 0);
    }
}
void write_through_device(WireDevice& wire)
{
    wire.no_flush = true;
    wire.sense_key = 5;
    wire.sense_asc = 0x20;
    wire.cache_mode[0] = 23;
    wire.cache_mode[4] = 8;
    wire.cache_mode[5] = 18;
}
TEST(UsbStorage, UnsupportedFlushRequiresExplicitCurrentWriteThroughPolicy)
{
    for(bool descriptor : {false, true})
    {
        WireDevice wire;
        write_through_device(wire);
        wire.sense_format = descriptor ? 0x72 : 0x70;
        wire.sense_bytes = descriptor ? 8 : 18;
        Device device;
        ASSERT_TRUE(device.initialize(wire.transport()));
        EXPECT_TRUE(device.write_through);
        EXPECT_STREQ(device.probe_stage, "storage-write-through");
        EXPECT_EQ(wire.opcodes, (std::vector<uint8_t>{0x12, 0, 0x25, 0x1a, 0x35, 3, 0x1a}));
        EXPECT_EQ(wire.cbw[16], 8);
        EXPECT_EQ(wire.cbw[17], 8);
        std::array<uint8_t, 512> bytes{}, read{};
        bytes[17] = 0x45;
        ASSERT_TRUE(device.read_write(true, 0, bytes.data(), 1));
        const auto count = wire.commands;
        ASSERT_TRUE(device.flush());
        EXPECT_EQ(wire.commands, count);
        ASSERT_TRUE(device.read_write(false, 0, read.data(), 1));
        EXPECT_EQ(read, bytes);
        device.invalidate();
        EXPECT_FALSE(device.flush());
        EXPECT_EQ(wire.resets, 0u);
    }
}
TEST(UsbStorage, WriteThroughFallbackRejectsUnknownUnsafeOrMalformedMetadata)
{
    for(unsigned scenario = 0; scenario < 13; ++scenario)
    {
        WireDevice wire;
        write_through_device(wire);
        switch(scenario)
        {
            case 0:
                wire.cache_mode[6] = 4;
                break;  // WCE enabled.
            case 1:
                wire.cache_rejected = true;
                break;
            case 2:
                wire.cache_mode[4] = 0;
                break;
            case 3:
                wire.cache_mode[4] = 0x48;
                break;  // Unsupported subpage format.
            case 4:
                wire.cache_mode[0] = 64;
                break;
            case 5:
                wire.cache_mode[3] = 255;
                break;
            case 6:
                wire.cache_bytes = 6;
                break;
            case 7:
                wire.cache_mode[5] = 17;
                break;
            case 8:
                wire.cache_mode[5] = 255;
                break;
            case 9:
                wire.cache_mode[2] = 0x80;
                break;
            case 10:
                wire.sense_format = 0x71;
                break;  // Deferred error.
            case 11:
                wire.sense_bytes = 13;
                break;
            case 12:
                wire.sense_asc = 0x24;
                break;  // Invalid field, not invalid opcode.
        }
        Device device;
        EXPECT_FALSE(device.initialize(wire.transport())) << scenario;
        EXPECT_FALSE(device.alive) << scenario;
        EXPECT_FALSE(device.write_through) << scenario;
        EXPECT_FALSE(device.flush()) << scenario;
        EXPECT_EQ(wire.commands, scenario >= 10 ? 6u : 7u) << scenario;
    }
}
TEST(UsbStorage, FlushTransportFailureDoesNotAttemptCompatibilityFallback)
{
    WireDevice wire;
    write_through_device(wire);
    wire.fail_flush_transport = true;
    Device device;
    EXPECT_FALSE(device.initialize(wire.transport()));
    EXPECT_FALSE(device.alive);
    EXPECT_EQ(wire.opcodes, (std::vector<uint8_t>{0x12, 0, 0x25, 0x1a, 0x35}));
    EXPECT_EQ(wire.resets, 1u);
    EXPECT_STREQ(device.probe_stage, "storage-flush");
    EXPECT_EQ(device.probe_result, Result::TransportFailed);
}
TEST(UsbStorage, ShortInputRequiresExplicitLengthAndMatchingResidue)
{
    for(unsigned scenario = 0; scenario < 4; ++scenario)
    {
        WireDevice wire;
        Device device;
        ASSERT_TRUE(device.initialize(wire.transport()));
        wire.fault = scenario == 3 ? WireDevice::ShortData : WireDevice::ConsistentShortData;
        uint8_t cdb[10]{}, data[32]{};
        cdb[0] = scenario == 2 ? 0x2a : 0x28;
        uint32_t actual = 99;
        auto result = device.command(
            cdb, 10, scenario != 2, data, sizeof(data), scenario == 0 ? nullptr : &actual);
        EXPECT_EQ(result, scenario == 1 ? Result::Ok : Result::TransportFailed);
        if(scenario != 0)
            EXPECT_EQ(actual, scenario == 1 ? 31u : 0u);
        EXPECT_EQ(wire.resets, scenario == 1 ? 0u : 1u);
    }
}
TEST(UsbStorage, CachePolicyAcceptsBoundedBlockDescriptorsAndPageSaveFlag)
{
    WireDevice wire;
    write_through_device(wire);
    wire.cache_mode = {};
    wire.cache_bytes = 32;
    wire.cache_mode[0] = 31;
    wire.cache_mode[3] = 8;
    wire.cache_mode[12] = 0x88;
    wire.cache_mode[13] = 18;
    Device device;
    ASSERT_TRUE(device.initialize(wire.transport()));
    EXPECT_TRUE(device.write_through);
}
TEST(UsbStorage, CachePolicyRejectsMisalignedBlockDescriptorLength)
{
    WireDevice wire;
    write_through_device(wire);
    wire.cache_mode = {};
    wire.cache_bytes = 25;
    wire.cache_mode[0] = 24;
    wire.cache_mode[3] = 1;
    wire.cache_mode[5] = 8;
    wire.cache_mode[6] = 18;
    Device device;
    EXPECT_FALSE(device.initialize(wire.transport()));
    EXPECT_FALSE(device.write_through);
    EXPECT_FALSE(device.alive);
    EXPECT_FALSE(device.flush());
    EXPECT_STREQ(device.probe_stage, "storage-cache-policy");
}
TEST(UsbStorage, BootProbeDistinguishesCommandRejectionFromTransportFailure)
{
    for(bool transport_failure : {false, true})
    {
        WireDevice wire;
        Device device;
        wire.no_mode = !transport_failure;
        wire.fail_data_opcode = transport_failure ? 0x1a : 0xff;
        EXPECT_FALSE(device.initialize(wire.transport()));
        EXPECT_FALSE(device.alive);
        EXPECT_EQ(device.sectors, 65536u);
        EXPECT_STREQ(device.probe_stage, "storage-mode-sense");
        EXPECT_EQ(device.probe_opcode, 0x1a);
        EXPECT_EQ(device.probe_result,
                  transport_failure ? Result::TransportFailed : Result::CommandFailed);
        EXPECT_EQ(wire.resets, transport_failure ? 1u : 0u);
        EXPECT_EQ(wire.opcodes, (std::vector<uint8_t>{0x12, 0, 0x25, 0x1a}));
    }
}
TEST(UsbStorage, BootProbeEvidenceRemainsStableAfterRuntimeFailure)
{
    WireDevice wire;
    Device device;
    ASSERT_TRUE(device.initialize(wire.transport()));
    wire.fault = WireDevice::CommandFailure;
    EXPECT_FALSE(device.flush());
    EXPECT_STREQ(device.probe_stage, "storage-ready");
    EXPECT_EQ(device.probe_opcode, 0x35);
    EXPECT_EQ(device.probe_result, Result::Ok);
    EXPECT_EQ(device.probe_mode, 0);
}
TEST(UsbStorage, BoundedUnitAttentionUsesRequestSense)
{
    WireDevice wire;
    Device device;
    wire.unit_attention = true;
    EXPECT_TRUE(device.initialize(wire.transport()));
    EXPECT_EQ(wire.opcodes, (std::vector<uint8_t>{0x12, 0, 3, 0, 0x25, 0x1a, 0x35}));
    WireDevice bad;
    bad.unit_attention = true;
    bad.sense_error = true;
    EXPECT_FALSE(device.initialize(bad.transport()));
}
TEST(UsbStorage, MalformedStatusAndShortTransfersRetireWithoutReplayingWrites)
{
    for(auto fault : {WireDevice::Signature,
                      WireDevice::Tag,
                      WireDevice::Phase,
                      WireDevice::ShortStatus,
                      WireDevice::Residue,
                      WireDevice::ShortData,
                      WireDevice::FailedData,
                      WireDevice::StallData})
    {
        WireDevice wire;
        Device device;
        ASSERT_TRUE(device.initialize(wire.transport()));
        wire.fault = fault;
        unsigned before = wire.commands;
        std::array<uint8_t, 512> data{};
        EXPECT_FALSE(device.read_write(true, 0, data.data(), 1));
        EXPECT_FALSE(device.alive);
        EXPECT_EQ(wire.commands, before + 1);
        EXPECT_EQ(wire.resets, 1u);
        EXPECT_FALSE(device.flush());
        EXPECT_EQ(wire.commands, before + 1);
    }
}
TEST(UsbStorage, RetriesOnlyStalledStatusAndFailsCommandWithoutReset)
{
    WireDevice wire;
    Device device;
    ASSERT_TRUE(device.initialize(wire.transport()));
    wire.fault = WireDevice::StallStatus;
    EXPECT_TRUE(device.flush());
    EXPECT_EQ(wire.clears, 1u);
    EXPECT_EQ(wire.resets, 0u);
    wire.fault = WireDevice::CommandFailure;
    EXPECT_FALSE(device.flush());
    EXPECT_FALSE(device.alive);
    EXPECT_EQ(wire.resets, 0u);
}
TEST(UsbStorage, DisconnectedStateCannotIssueFurtherCommands)
{
    WireDevice wire;
    Device device;
    ASSERT_TRUE(device.initialize(wire.transport()));
    auto calls = wire.commands;
    device.invalidate();
    EXPECT_FALSE(device.flush());
    EXPECT_EQ(wire.commands, calls);
}
std::vector<uint8_t> configuration()
{
    return {9,    2, 32, 0, 1,    1, 0, 0x80, 50, 9, 4, 0, 0, 2, 8, 6,
            0x50, 0, 7,  5, 0x81, 2, 0, 2,    0,  7, 5, 2, 2, 0, 2, 0};
}
TEST(UsbStorage, SelectsOnlyBoundedBotDescriptors)
{
    auto bytes = configuration();
    Interface out;
    ASSERT_TRUE(parse_configuration(bytes.data(), bytes.size(), out));
    EXPECT_EQ(out.in, 0x81);
    EXPECT_EQ(out.out, 2);
    EXPECT_EQ(out.in_packet, 512);
    for(size_t cut = 0; cut < bytes.size(); ++cut)
        EXPECT_FALSE(parse_configuration(bytes.data(), cut, out));
    for(auto change : std::vector<std::pair<size_t, uint8_t>>{
            {9, 0}, {16, 0x62}, {13, 3}, {20, 0x80}, {21, 3}, {27, 0x82}, {22, 0xff}})
    {
        bytes = configuration();
        bytes[change.first] = change.second;
        EXPECT_FALSE(parse_configuration(bytes.data(), bytes.size(), out));
    }
}
TEST(UsbStorage, ValidatesSuperspeedCompanionsWithoutStreams)
{
    auto bytes = configuration();
    bytes[23] = 4;
    bytes[30] = 4;
    bytes.insert(bytes.begin() + 25, {6, 48, 15, 0, 0, 0});
    bytes.insert(bytes.end(), {6, 48, 15, 0, 0, 0});
    bytes[2] = bytes.size();
    Interface out;
    ASSERT_TRUE(parse_configuration(bytes.data(), bytes.size(), out));
    EXPECT_TRUE(out.in_companion);
    EXPECT_TRUE(out.out_companion);
    EXPECT_EQ(out.out_burst, 15);
    bytes[28] = 1;
    EXPECT_FALSE(parse_configuration(bytes.data(), bytes.size(), out));
}
}  // namespace
