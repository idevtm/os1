#pragma once
#include <os1/input.h>
#include <os1/resource.h>
#include <stdint.h>

namespace os1::hardware::test_run
{
constexpr unsigned run_limit = 256;
inline bool usable_input_event(const os1_input_event& event, unsigned bytes, uint64_t device)
{
    return bytes == sizeof(event) && event.device == device &&
           (event.kind == OS1_INPUT_KEYBOARD || event.kind == OS1_INPUT_POINTER) &&
           !(event.flags & (OS1_INPUT_LOST | OS1_INPUT_REMOVED));
}
// No I/O or key payload retention. The caller reports these counters after polling.
struct InputFailureSample
{
    uint64_t observed_ns{}, sequence{}, event_ns{}, device{}, lost{};
    int64_t status{};
    uint32_t bytes{}, flags{}, kind{};
};
struct InputFailures
{
    uint64_t total{}, lost_events{}, lost_records{}, removed{}, invalid{}, closed{}, errors{};
    InputFailureSample first{}, last{};
    bool accept(long status,
                unsigned bytes,
                const os1_input_event& event,
                uint64_t device,
                unsigned kind,
                uint64_t now)
    {
        if(status == OS1_ERR_WOULD_BLOCK)
            return false;
        bool valid = status == 0 && bytes == sizeof(event);
        InputFailureSample sample{};
        sample.observed_ns = now;
        sample.status = status;
        sample.bytes = bytes;
        if(valid)
        {
            sample.sequence = event.sequence;
            sample.event_ns = event.time_ns;
            sample.device = event.device;
            sample.lost = event.lost;
            sample.flags = event.flags;
            sample.kind = event.kind;
        }
        if(status == OS1_ERR_CLOSED)
            ++closed;
        else if(status != 0)
            ++errors;
        else if(!valid || event.device != device || event.kind != kind ||
                (event.flags & ~(OS1_INPUT_CHANGED | OS1_INPUT_REMOVED | OS1_INPUT_LOST |
                                 OS1_INPUT_COARSE_CLOCK)))
            ++invalid;
        else if(event.flags & (OS1_INPUT_LOST | OS1_INPUT_REMOVED))
        {
            if(event.flags & OS1_INPUT_LOST)
            {
                ++lost_events;
                lost_records += event.lost;
            }
            removed += bool(event.flags & OS1_INPUT_REMOVED);
        }
        else
            return true;
        if(!total++)
            first = sample;
        last = sample;
        return false;
    }
};
inline bool label_valid(const char* s)
{
    unsigned n = 0;
    for(; s[n]; ++n)
        if(n == 24 || !((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= 'A' && s[n] <= 'Z') ||
                        (s[n] >= '0' && s[n] <= '9') || s[n] == '-' || s[n] == '_'))
            return false;
    return n != 0;
}
struct Hash
{
    uint64_t value = UINT64_C(14695981039346656037);
    void byte(unsigned char c)
    {
        value = (value ^ c) * UINT64_C(1099511628211);
    }
    void number(uint64_t n)
    {
        for(unsigned i = 0; i < 8; ++i, n >>= 8)
            byte(n);
    }
    void text(const char* s)
    {
        while(*s)
            byte(*s++);
        byte(0);
    }
};
inline void hex(uint64_t value, char* out)
{
    for(unsigned i = 0; i < 16; ++i)
        out[i] = "0123456789abcdef"[(value >> (60 - 4 * i)) & 15];
    out[16] = 0;
}
inline void run_name(uint64_t boot, unsigned run, char (&out)[40])
{
    out[0] = 'b';
    hex(boot, out + 1);
    out[17] = '-';
    out[18] = 'r';
    hex(run, out + 19);
}
// POSIX ustar header for bounded, flat capture members.
inline bool tar_header(const char* name, unsigned bytes, char (&out)[512])
{
    for(auto& c : out)
        c = 0;
    unsigned n = 0;
    while(name[n])
    {
        if(n == 99 || name[n] == '/' || name[n] == '\\')
            return false;
        out[n] = name[n];
        ++n;
    }
    if(!n || bytes > 262144)
        return false;
    auto octal = [&](unsigned at, unsigned width, unsigned value) {
        for(unsigned i = 1; i < width; ++i, value >>= 3)
            out[at + width - 1 - i] = '0' + (value & 7);
    };
    octal(100, 8, 0644);
    octal(108, 8, 0);
    octal(116, 8, 0);
    octal(124, 12, bytes);
    octal(136, 12, 0);
    for(unsigned i = 148; i < 156; ++i)
        out[i] = ' ';
    out[156] = '0';
    const char magic[] = "ustar";
    for(unsigned i = 0; i < 5; ++i)
        out[257 + i] = magic[i];
    out[263] = out[264] = '0';
    unsigned sum = 0;
    for(unsigned char c : out)
        sum += c;
    octal(148, 7, sum);
    out[155] = ' ';
    return true;
}
}  // namespace os1::hardware::test_run
