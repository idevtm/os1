#pragma once
#include "interface/metadata.hpp"
namespace os1::textprocessor
{
constexpr uint64_t interface_id = 0x545850524f433031ull;
constexpr uint32_t interface_version = 1;
enum : uint32_t
{
    Lowercase = 1,
    Uppercase = 2,
    GetEnabled = 3,
    SetEnabled = 4,
    GetProcessed = 5,
    Progress = 6,
    LowercaseBuffer = 7,
    UppercaseBuffer = 8
};
constexpr int32_t revision_conflict = -100;
struct Text
{
    uint32_t length;
    uint8_t bytes[252];
};
struct Settings
{
    uint64_t revision;
    uint32_t enabled, reserved;
};
struct ProgressValue
{
    uint64_t processed, total;
};
// Attachment index selects the Buffer. The receiver can inspect its entire storage.
struct BufferRange
{
    uint32_t attachment, reserved;
    uint64_t offset, length;
};
inline bool valid_range(const BufferRange& r, uint32_t count, uint64_t bytes)
{
    return !r.reserved && r.attachment < count && r.offset <= bytes && r.length <= bytes - r.offset;
}
constexpr interface::Metadata metadata{
    interface::magic,
    1,
    sizeof(interface::Metadata),
    interface_id,
    interface_version,
    5,
    11,
    8,
    "textprocessor",
    "TextProcessor",
    {{interface::U64, 8, 0, 0, "u64"},
     {interface::Record, 256, 0, 2, "Text"},
     {interface::Record, 16, 2, 3, "Settings"},
     {interface::Record, 16, 5, 2, "Progress"},
     {interface::Record, 24, 7, 4, "BufferRange"}},
    {{interface::U32, 0, 1, 0, "length"},
     {interface::U8, 4, 252, 0, "bytes"},
     {interface::U64, 0, 1, 0, "revision"},
     {interface::U32, 8, 1, 0, "enabled"},
     {interface::U32, 12, 1, 0, "reserved"},
     {interface::U64, 0, 1, 0, "processed"},
     {interface::U64, 8, 1, 0, "total"},
     {interface::U32, 0, 1, 0, "attachment"},
     {interface::U32, 4, 1, 0, "reserved"},
     {interface::U64, 8, 1, 0, "offset"},
     {interface::U64, 16, 1, 0, "length"}},
    {{Lowercase, interface::Method, 1, 2, 2, 0, "lowercase"},
     {Uppercase, interface::Method, 1, 2, 2, 0, "uppercase"},
     {GetEnabled, interface::PropertyGet, 1, 0, 3, 0, "enabled"},
     {SetEnabled, interface::PropertySet, 1, 3, 3, 0, "enabled"},
     {GetProcessed, interface::PropertyGet, 1, 0, 1, 0, "processed"},
     {Progress, interface::Event, 1, 4, 0, 0, "progress"},
     {LowercaseBuffer, interface::Method, 1, 5, 5, 0, "lowercase_buffer_in_place"},
     {UppercaseBuffer, interface::Method, 1, 5, 5, 0, "uppercase_buffer_out_of_place"}}};
static_assert(sizeof(Text) == 256 && sizeof(Settings) == 16 && sizeof(BufferRange) == 24);
}  // namespace os1::textprocessor
