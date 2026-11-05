#pragma once
#include "interface/constructor.hpp"
namespace os1::counter
{
constexpr uint64_t interface_id = 0x434f554e54455231ull;
constexpr uint32_t interface_version = 1;
enum : uint32_t
{
    Read = 1,
    Add = 2,
    Reset = 3,
    GetValue = 4,
    Changed = 5,
    Construct = 6
};
struct Args
{
    uint64_t initial, ceiling;
};
struct Snapshot
{
    uint64_t value, revision;
};
constexpr constructor::Metadata constructor_metadata{
    constructor::magic,
    1,
    sizeof(constructor::Metadata),
    interface_id,
    1,
    16,
    2,
    0,
    {{0, interface::U64, 0, "initial", "initial"},
     {8, interface::U64, UINT64_MAX, "ceiling", "ceiling"}}};
constexpr interface::Metadata metadata{interface::magic,
                                       1,
                                       sizeof(interface::Metadata),
                                       interface_id,
                                       1,
                                       3,
                                       4,
                                       6,
                                       "counter",
                                       "Counter",
                                       {{interface::U64, 8, 0, 0, "u64"},
                                        {interface::Record, 16, 0, 2, "Snapshot"},
                                        {interface::Record, 16, 2, 2, "Args"}},
                                       {{interface::U64, 0, 1, 0, "value"},
                                        {interface::U64, 8, 1, 0, "revision"},
                                        {interface::U64, 0, 1, 0, "initial"},
                                        {interface::U64, 8, 1, 0, "ceiling"}},
                                       {{Read, interface::Method, 1, 0, 2, 0, "read"},
                                        {Add, interface::Method, 1, 1, 2, 0, "add"},
                                        {Reset, interface::Method, 1, 0, 2, 0, "reset"},
                                        {GetValue, interface::PropertyGet, 1, 0, 1, 0, "value"},
                                        {Changed, interface::Event, 1, 2, 0, 0, "changed"},
                                        {Construct, interface::Method, 1, 3, 2, 0, "construct"}}};
}  // namespace os1::counter
