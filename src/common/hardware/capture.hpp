#pragma once
#include <stdint.h>
namespace os1::hardware::capture
{
constexpr unsigned version = 1, record_limit = 160;
struct Header
{
    char magic[8] = {'O', 'S', '1', 'C', 'A', 'T', '1', 0};
    uint32_t version = capture::version, record_bytes = 360, count = 0, complete = 0;
    uint64_t generation = 0;
};
struct Record
{
    uint32_t id, parent, source, backing;
    uint64_t known;
    uint64_t values[22];  // Hardware fields 8 through 29; known uses original field bits.
    char name[96], state[32], driver[32];
};
static_assert(sizeof(Header) == 32);
static_assert(sizeof(Record) == 360);
}  // namespace os1::hardware::capture
