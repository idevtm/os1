#pragma once
#include <os1/log.h>
#include <stddef.h>
namespace os1::log
{
constexpr uint64_t frame_magic = UINT64_C(0x31474f4c31534f);
constexpr unsigned frame_limit = 65536, segment_limit = 262144;
struct Frame
{
    uint64_t magic = frame_magic;
    uint32_t version = 1, bytes = sizeof(Frame), record_bytes = 0, count = 0, stream = 0,
             checksum = 0;
    uint64_t boot = 0, session = 0, segment = 0, first = 0, last = 0;
    uint64_t monotonic_ns = 0, wall_ns = 0, resolution_ns = 0, overwritten = 0, dropped = 0;
    uint32_t diagnostic = 0, boot_mode = 0;
    char revision[40]{}, image[64]{};
    uint8_t reserved[32]{};
};
static_assert(sizeof(Frame) == 256);
static_assert(offsetof(Frame, checksum) == 28);
inline uint32_t crc(const void* data, size_t bytes)
{
    const auto* p = static_cast<const uint8_t*>(data);
    uint32_t value = ~0u;
    for(size_t i = 0; i < bytes; ++i)
    {
        value ^= (i >= 28 && i < 32) ? 0 : p[i];
        for(unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
    }
    return ~value;
}
inline bool header_valid(const Frame& h)
{
    if(h.magic != frame_magic || h.version != 1 || h.bytes < sizeof(Frame) ||
       h.bytes > frame_limit || (h.stream != 1 && h.stream != 2) || !h.boot || h.diagnostic > 1 ||
       h.boot_mode > 2)
        return false;
    unsigned size = h.stream == 1 ? sizeof(os1_log_record) : sizeof(os1_log_history_record);
    if(h.record_bytes != size || h.count > (frame_limit - sizeof(Frame)) / size ||
       h.bytes != sizeof(Frame) + h.count * size || (h.count && (!h.first || h.last < h.first)) ||
       (!h.count && (h.first || h.last)) || (h.stream == 1 && h.session))
        return false;
    for(auto byte : h.reserved)
        if(byte)
            return false;
    return true;
}
inline bool valid(const Frame& h, size_t available)
{
    return header_valid(h) && h.bytes <= available && h.checksum == crc(&h, h.bytes);
}
struct Name
{
    uint64_t boot = 0, session = 0, segment = 0;
    unsigned stream = 0;
};
inline void hex(char*& p, uint64_t value)
{
    for(int shift = 60; shift >= 0; shift -= 4)
        *p++ = "0123456789abcdef"[(value >> shift) & 15];
}
inline void filename(const Name& n, char (&out)[64])
{
    char* p = out;
    const char* prefix = n.stream == 1 ? "kernel-" : "shell-";
    while(*prefix)
        *p++ = *prefix++;
    hex(p, n.boot);
    *p++ = '-';
    if(n.stream == 2)
    {
        hex(p, n.session);
        *p++ = '-';
    }
    hex(p, n.segment);
    for(char c : ".bin")
        *p++ = c;
}
inline bool parse_name(const char* text, size_t size, Name& n)
{
    n = {};
    const char* prefix = size == 44 ? "kernel-" : size == 60 ? "shell-" : nullptr;
    if(!prefix)
        return false;
    n.stream = size == 44 ? 1 : 2;
    size_t at = 0;
    while(*prefix)
        if(text[at++] != *prefix++)
            return false;
    auto number = [&](uint64_t& value) {
        for(unsigned i = 0; i < 16; ++i)
        {
            char c = text[at++];
            unsigned digit = c >= '0' && c <= '9'   ? c - '0'
                             : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                    : 99;
            if(digit > 15)
                return false;
            value = (value << 4) | digit;
        }
        return true;
    };
    if(!number(n.boot) || text[at++] != '-')
        return false;
    if(n.stream == 2 && (!number(n.session) || text[at++] != '-'))
        return false;
    return number(n.segment) && text[at++] == '.' && text[at++] == 'b' && text[at++] == 'i' &&
           text[at++] == 'n' && at == size && n.boot;
}
}  // namespace os1::log
