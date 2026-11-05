#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "interface/parameter_binding.hpp"
#include "interface/parameter_demo.hpp"
namespace
{
namespace p = os1::parameters;
namespace demo = os1::parameter_demo;
constexpr auto schema = p::fixture::make();
bool rendered(const char* raw, demo::Rendered& out)
{
    p::Diagnostic d{};
    p::Value parsed{}, decoded{};
    p::Wire wire{};
    return p::parse(schema, raw, std::strlen(raw), parsed, d) &&
           p::encode(schema, parsed, wire, d) &&
           p::decode(schema, wire.data, wire.size, decoded, d) && demo::render(decoded, out);
}
TEST(ParameterRuntime, RenderContractThroughActualWire)
{
    struct Example
    {
        const char* raw;
        uint64_t count;
        const char* text;
    };
    Example cases[] = {
        {"", 1, "!"},
        {"initial:4 ceiling:8 mode.thorough source.text:'ab c' options:{prefix:'>', suffix:null} "
         "label:\"L \"",
         8,
         "L >AB C"},
        {"mode:none source.file:{path:'ABC', offset:1} options.fallback.fast options.suffix:null",
         2,
         "bc"},
        {"label:\"null\" source.none options.suffix:null", 4, "null"},
        {"label:null source.none options.suffix:null", 0, ""},
        {"source.text:'AbC' options:{enabled:false,limit:2}", 3, "Ab!"},
        {"source.text:'a\"b' options.suffix:null", 3, "a\"b"},
        {"initial:4 source.text:'Hi there' options.suffix:null", 12, "Hi there"},
        {"mode.thorough source.text:\"hello world\" options:{limit:5,suffix:null}", 5, "HELLO"}};
    for(const auto& c : cases)
    {
        demo::Rendered output{};
        ASSERT_TRUE(rendered(c.raw, output)) << c.raw;
        EXPECT_EQ(output.count, c.count) << c.raw;
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(output.bytes), output.size), c.text)
            << c.raw;
        uint8_t wire[256]{};
        EXPECT_EQ(demo::encode_result(output, wire), output.size + 8);
        EXPECT_EQ(os1::constructor::decode(wire, 8), c.count);
    }
    demo::Rendered output{};
    EXPECT_FALSE(rendered("initial:101", output));
}
TEST(ParameterRuntime, ExactTransportBoundAndEmbeddedNull)
{
    std::string prefix = "source.text:'" + std::string(64, 'A') + "' options.prefix:'" +
                         std::string(64, 'p') + "' label:'";
    bool full = false;
    for(unsigned length = 0; length <= 64; ++length)
    {
        auto raw = prefix + std::string(length, 'l') + "'";
        p::Diagnostic d{};
        p::Value v{};
        p::Wire wire{};
        if(p::parse(schema, raw.data(), raw.size(), v, d) && p::encode(schema, v, wire, d) &&
           wire.size == 256)
        {
            full = true;
            p::Value decoded{};
            ASSERT_TRUE(p::decode(schema, wire.data, wire.size, decoded, d));
            demo::Rendered out{};
            ASSERT_TRUE(demo::render(decoded, out));
            EXPECT_LE(out.size, 248u);
            raw = prefix + std::string(length + 1, 'l') + "'";
            EXPECT_FALSE(p::parse(schema, raw.data(), raw.size(), v, d));
            EXPECT_EQ(d.code, p::Error::Limit);
            break;
        }
    }
    EXPECT_TRUE(full);
    p::Diagnostic d{};
    p::Value v{};
    const char raw[] = "source.text:'a\0b' options.suffix:null";
    ASSERT_TRUE(p::parse(schema, raw, sizeof(raw) - 1, v, d));
    demo::Rendered out{};
    ASSERT_TRUE(demo::render(v, out));
    ASSERT_EQ(out.size, 3u);
    EXPECT_EQ(out.bytes[1], 0u);
    EXPECT_EQ(out.bytes[2], 'b');
}
TEST(ParameterRuntime, BindingIdentityIsIndependentOfNames)
{
    p::Binding binding{p::binding_magic,
                       1,
                       sizeof(p::Binding),
                       schema.schema_id,
                       schema.schema_version,
                       1,
                       demo::interface_id,
                       demo::interface_version,
                       1,
                       {demo::Render}};
    ASSERT_TRUE(p::validate_binding(binding, schema));
    EXPECT_TRUE(p::binds(binding, demo::metadata, demo::Render));
    EXPECT_FALSE(p::binds(binding, demo::metadata, 2));
    auto changed = binding;
    changed.schema_version++;
    EXPECT_FALSE(p::validate_binding(changed, schema));
    changed = binding;
    changed.member_count = 2;
    changed.members[1] = demo::Render;
    EXPECT_FALSE(p::validate_binding(changed, schema));
    changed = binding;
    changed.members[7] = 9;
    EXPECT_FALSE(p::validate_binding(changed, schema));
    changed = binding;
    changed.interface_id++;
    EXPECT_FALSE(p::binds(changed, demo::metadata, demo::Render));
    auto api = demo::metadata;
    api.members[0].kind = os1::interface::Event;
    EXPECT_FALSE(p::binds(binding, api, demo::Render));
}
}  // namespace
