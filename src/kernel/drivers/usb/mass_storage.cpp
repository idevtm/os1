#include "drivers/usb/mass_storage.hpp"

#include "util/memory.h"

namespace usb::storage
{
namespace
{
uint16_t le16(const uint8_t* p)
{
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
uint32_t le32(const uint8_t* p)
{
    return uint32_t(le16(p)) | uint32_t(le16(p + 2)) << 16;
}
uint32_t be32(const uint8_t* p)
{
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void put_le32(uint8_t* p, uint32_t n)
{
    for(unsigned i = 0; i < 4; ++i)
        p[i] = uint8_t(n >> (i * 8));
}
void put_be32(uint8_t* p, uint32_t n)
{
    for(unsigned i = 0; i < 4; ++i)
        p[i] = uint8_t(n >> (24 - i * 8));
}
bool complete(const Interface& i)
{
    return i.in && i.out;
}
}  // namespace

bool parse_configuration(const void* buffer, size_t bytes, Interface& out)
{
    out = {};
    const auto* p = static_cast<const uint8_t*>(buffer);
    if(!p || bytes < 9 || p[0] != 9 || p[1] != 2 || le16(p + 2) != bytes || !p[5])
        return false;
    Interface candidate{};
    bool selected = false;
    uint8_t last_endpoint = 0;
    for(size_t at = 9; at < bytes;)
    {
        if(bytes - at < 2 || p[at] < 2 || p[at] > bytes - at)
            return false;
        const auto* d = p + at;
        if(d[1] == 4)
        {
            if(d[0] < 9)
                return false;
            if(selected && !complete(candidate))
                return false;
            if(selected && !out.in)
                out = candidate;
            selected = d[3] == 0 && d[5] == 8 && d[6] == 6 && d[7] == 0x50;
            candidate = {};
            if(selected)
            {
                if(d[4] != 2)
                    return false;
                candidate.configuration = p[5];
                candidate.number = d[2];
            }
            last_endpoint = 0;
        }
        else if(selected && d[1] == 5)
        {
            if(d[0] < 7 || d[3] != 2 || !(d[2] & 15) || (d[2] & 0x70))
                return false;
            uint16_t packet = le16(d + 4);
            if(packet != 8 && packet != 16 && packet != 32 && packet != 64 && packet != 512 &&
               packet != 1024)
                return false;
            bool in = d[2] & 0x80;
            if(in ? candidate.in != 0 : candidate.out != 0)
                return false;
            if(in)
            {
                candidate.in = d[2];
                candidate.in_packet = packet;
            }
            else
            {
                candidate.out = d[2];
                candidate.out_packet = packet;
            }
            last_endpoint = d[2];
        }
        else if(selected && d[1] == 48)
        {
            if(d[0] != 6 || !last_endpoint || d[2] > 15 || d[3] || le16(d + 4))
                return false;
            if(last_endpoint & 0x80)
            {
                candidate.in_burst = d[2];
                candidate.in_companion = true;
            }
            else
            {
                candidate.out_burst = d[2];
                candidate.out_companion = true;
            }
            last_endpoint = 0;
        }
        else
            last_endpoint = 0;
        at += d[0];
    }
    if(selected && !complete(candidate))
        return false;
    if(selected && !out.in)
        out = candidate;
    return complete(out);
}

void Device::invalidate()
{
    __atomic_store_n(&alive, false, __ATOMIC_RELEASE);
}
Result Device::command(const uint8_t* cdb, uint8_t cdb_bytes, bool in, void* data, uint32_t bytes)
{
    if(!__atomic_load_n(&alive, __ATOMIC_ACQUIRE) || !cdb || !cdb_bytes || cdb_bytes > 16 ||
       bytes > 4096 || (bytes && !data))
        return Result::TransportFailed;
    uint8_t cbw[31]{};
    put_le32(cbw, 0x43425355);
    put_le32(cbw + 4, ++tag);
    put_le32(cbw + 8, bytes);
    cbw[12] = in ? 0x80 : 0;
    cbw[14] = cdb_bytes;
    memcpy(cbw + 15, cdb, cdb_bytes);
    auto fail = [&]() {
        // Never replay a command whose write effect is uncertain.
        (void)transport.reset(transport.context);
        invalidate();
        return Result::TransportFailed;
    };
    Transfer t = transport.bulk(transport.context, false, cbw, sizeof(cbw));
    if(t.status != TransferStatus::Ok || t.bytes != sizeof(cbw))
        return fail();
    uint32_t transferred = 0;
    bool stalled = false;
    if(bytes)
    {
        t = transport.bulk(transport.context, in, data, bytes);
        if(t.status == TransferStatus::Failed || t.bytes > bytes)
            return fail();
        transferred = t.bytes;
        stalled = t.status == TransferStatus::Stall;
        if(stalled && !transport.clear_halt(transport.context, in))
            return fail();
    }
    uint8_t csw[13]{};
    t = transport.bulk(transport.context, true, csw, sizeof(csw));
    if(t.status == TransferStatus::Stall && transport.clear_halt(transport.context, true))
        t = transport.bulk(transport.context, true, csw, sizeof(csw));
    if(t.status != TransferStatus::Ok || t.bytes != sizeof(csw) || le32(csw) != 0x53425355 ||
       le32(csw + 4) != tag || csw[12] > 1 || le32(csw + 8) > bytes)
        return fail();
    if(csw[12] == 1)
        return Result::CommandFailed;
    if(stalled || le32(csw + 8) || transferred != bytes)
        return fail();
    return Result::Ok;
}
bool Device::initialize(Transport t)
{
    *this = {};
    transport = t;
    if(!t.bulk || !t.clear_halt || !t.reset)
        return false;
    alive = true;
    uint8_t inquiry[36]{}, command_bytes[16]{};
    command_bytes[0] = 0x12;
    command_bytes[4] = sizeof(inquiry);
    if(command(command_bytes, 6, true, inquiry, sizeof(inquiry)) != Result::Ok ||
       (inquiry[0] & 0x1f) != 0 || (inquiry[0] & 0xe0))
    {
        invalidate();
        return false;
    }
    bool ready = false;
    for(unsigned retry = 0; retry < 3 && __atomic_load_n(&alive, __ATOMIC_ACQUIRE); ++retry)
    {
        memset(command_bytes, 0, sizeof(command_bytes));
        Result result = command(command_bytes, 6, false, nullptr, 0);
        if(result == Result::Ok)
        {
            ready = true;
            break;
        }
        if(result != Result::CommandFailed)
            break;
        uint8_t sense[18]{};
        command_bytes[0] = 3;
        command_bytes[4] = sizeof(sense);
        if(command(command_bytes, 6, true, sense, sizeof(sense)) != Result::Ok ||
           (sense[2] & 15) != 6)
            break;
    }
    uint8_t capacity[8]{}, mode[4]{};
    memset(command_bytes, 0, sizeof(command_bytes));
    command_bytes[0] = 0x25;
    if(!ready || command(command_bytes, 10, true, capacity, 8) != Result::Ok ||
       be32(capacity + 4) != 512 || be32(capacity) == 0xffffffff)
    {
        invalidate();
        return false;
    }
    sectors = uint64_t(be32(capacity)) + 1;
    memset(command_bytes, 0, sizeof(command_bytes));
    command_bytes[0] = 0x1a;
    command_bytes[1] = 8;
    command_bytes[2] = 0x3f;
    command_bytes[4] = 4;
    if(command(command_bytes, 6, true, mode, 4) != Result::Ok || (mode[2] & 0x80) || !flush())
    {
        invalidate();
        return false;
    }
    return true;
}
bool Device::read_write(bool write, uint64_t sector, void* data, uint32_t count)
{
    if(!count || count > 8 || sector >= sectors || count > sectors - sector || sector > 0xffffffff)
        return false;
    uint8_t cdb[10]{};
    cdb[0] = write ? 0x2a : 0x28;
    put_be32(cdb + 2, uint32_t(sector));
    cdb[7] = count >> 8;
    cdb[8] = count;
    bool ok = command(cdb, 10, !write, data, count * 512) == Result::Ok;
    if(!ok)
        invalidate();
    return ok;
}
bool Device::flush()
{
    uint8_t cdb[10]{};
    cdb[0] = 0x35;
    bool ok = command(cdb, 10, false, nullptr, 0) == Result::Ok;
    if(!ok)
        invalidate();
    return ok;
}
}  // namespace usb::storage
