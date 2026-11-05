#pragma once
#include "interface/constructor.hpp"
#include "interface/parameter_fixture.hpp"

namespace os1::parameter_demo
{
constexpr uint64_t interface_id = 0x314f4d454450534full;
constexpr uint32_t interface_version = 1, Render = 1;
// One rich schema is bound to the constructor and Render by this interface version.
constexpr interface::Metadata metadata{
    interface::magic,
    1,
    sizeof(interface::Metadata),
    interface_id,
    interface_version,
    1,
    1,
    1,
    "ParameterDemo",
    "parameters",
    {{interface::Record, 256, 0, 1, "CanonicalBytes"}},
    {{interface::U8, 0, 256, 0, "bytes"}},
    {{Render, interface::Method, interface::implemented, 1, 1, 0, "render"}}};
struct Rendered
{
    uint64_t count{};
    unsigned size{};
    uint8_t bytes[248]{};
};
// Operates only on a tree admitted by the shared decoder. Paths remain text.
inline bool render(const parameters::Value& v, Rendered& out)
{
    using namespace parameters;
    auto field = [&](uint16_t parent, unsigned id) -> const Node& {
        return v.nodes[child_key(v, parent, id)];
    };
    const uint64_t initial = field(0, 1).number, ceiling = field(0, 2).number;
    if(initial > ceiling)
        return false;
    uint64_t mode = field(0, 3).number;
    uint16_t options = child_key(v, 0, 5);
    const auto& fallback = field(options, 4);
    if(mode == 4 && fallback.number)
        mode = v.nodes[fallback.first].number;
    out = {};
    auto append = [&](const Node& text, bool convert, uint64_t limit) {
        for(unsigned i = 0; i < text.size && i < limit && out.size < sizeof(out.bytes); ++i)
        {
            auto c = static_cast<uint8_t>(v.strings[text.begin + i]);
            if(convert && mode == 1 && c >= 'A' && c <= 'Z')
                c += 'a' - 'A';
            if(convert && mode == 3 && c >= 'a' && c <= 'z')
                c -= 'a' - 'A';
            out.bytes[out.size++] = c;
        }
    };
    auto optional = [&](const Node& n) {
        if(n.number)
            append(v.nodes[n.first], false, 64);
    };
    optional(field(0, 6));
    optional(field(options, 3));
    const auto& source = field(0, 4);
    if(source.number == 1)
        append(v.nodes[source.first], field(options, 2).number, field(options, 1).number);
    if(source.number == 2)
    {
        const auto& path = field(source.first, 1);
        Node sliced = path;
        uint64_t skip = field(source.first, 2).number;
        if(skip > sliced.size)
            skip = sliced.size;
        sliced.begin += skip;
        sliced.size -= skip;
        append(sliced, field(options, 2).number, field(options, 1).number);
    }
    optional(field(options, 5));
    out.count = out.size > ceiling - initial ? ceiling : initial + out.size;
    return true;
}
inline unsigned encode_result(const Rendered& value, uint8_t* bytes)
{
    constructor::encode(bytes, 8, value.count);
    for(unsigned i = 0; i < value.size; ++i)
        bytes[8 + i] = value.bytes[i];
    return 8 + value.size;
}
}  // namespace os1::parameter_demo
