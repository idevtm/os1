#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "interface/buffer_metadata.hpp"
#include "interface/counter.hpp"
#include "interface/textprocessor.hpp"
using namespace os1::interface;
namespace
{
std::vector<uint8_t> image()
{
    std::vector<uint8_t> bytes(8192);
    elf::Elf64Header h{};
    h.magic = elf::kMagic;
    h.ident[0] = 2;
    h.ident[1] = h.ident[2] = 1;
    h.type = 2;
    h.machine = 62;
    h.version = 1;
    h.ehsize = sizeof(h);
    h.shentsize = sizeof(Section);
    h.shoff = 64;
    h.shnum = 3;
    h.shstrndx = 1;
    Section sections[3]{};
    sections[1].type = 3;
    sections[1].offset = 512;
    sections[1].size = 15;
    sections[2].name = 1;
    sections[2].type = 1;
    sections[2].flags = 2;
    sections[2].offset = 1024;
    sections[2].size = sizeof(Metadata);
    std::memcpy(bytes.data(), &h, sizeof(h));
    std::memcpy(bytes.data() + 64, sections, sizeof(sections));
    std::memcpy(bytes.data() + 512, "\0.os.interface", 15);
    std::memcpy(bytes.data() + 1024, &os1::textprocessor::metadata, sizeof(Metadata));
    return bytes;
}
bool parse(const std::vector<uint8_t>& bytes)
{
    Metadata m{};
    return read_elf(
        [&](uint64_t offset, void* out, size_t size) {
            if(offset > bytes.size() || size > bytes.size() - offset)
                return false;
            std::memcpy(out, bytes.data() + offset, size);
            return true;
        },
        bytes.size(),
        m);
}
TEST(InterfaceMetadata, CanonicalSchemaAndNegativeRecords)
{
    auto m = os1::textprocessor::metadata;
    ASSERT_TRUE(validate(m));
    auto bad = m;
    bad.version = 2;
    EXPECT_FALSE(validate(bad));
    bad = m;
    bad.size--;
    EXPECT_FALSE(validate(bad));
    bad = m;
    bad.magic++;
    EXPECT_FALSE(validate(bad));
    bad = m;
    bad.type_count = 9;
    EXPECT_FALSE(validate(bad));
    bad = m;
    bad.field_count = 17;
    EXPECT_FALSE(validate(bad));
    bad = m;
    bad.member_count = 9;
    EXPECT_FALSE(validate(bad));
    bad = m;
    bad.members[1].id = bad.members[0].id;
    EXPECT_FALSE(validate(bad));
    bad = m;
    bad.members[0].input = 9;
    EXPECT_FALSE(validate(bad));
    bad = m;
    bad.members[5].flags = 2;
    EXPECT_FALSE(validate(bad));
    bad = m;
    bad.types[1].first_field = 16;
    EXPECT_FALSE(validate(bad));
    bad = m;
    bad.fields[1].count = 253;
    EXPECT_FALSE(validate(bad));
    bad = m;
    bad.fields[0].kind = Record;
    EXPECT_FALSE(validate(bad));
    bad = m;
    bad.fields[0].offset = 1;
    EXPECT_FALSE(validate(bad));
    bad = m;
    bad.class_name[31] = 'x';
    EXPECT_FALSE(validate(bad));
    bad = m;
    bad.interface_name[0] = '\n';
    EXPECT_FALSE(validate(bad));
    bad = m;
    bad.types[7].kind = U8;
    EXPECT_FALSE(validate(bad));
}
TEST(InterfaceMetadata, OfflineElfBoundsAndSectionUniqueness)
{
    auto bytes = image();
    ASSERT_TRUE(parse(bytes));
    for(size_t size : {0u, 63u, 255u, 526u, 1024u, 2727u})
    {
        auto truncated = bytes;
        truncated.resize(size);
        EXPECT_FALSE(parse(truncated)) << size;
    }
    auto bad = bytes;
    bad[4] = 1;
    EXPECT_FALSE(parse(bad));
    bad = bytes;
    auto* h = reinterpret_cast<elf::Elf64Header*>(bad.data());
    h->shoff = UINT64_MAX;
    EXPECT_FALSE(parse(bad));
    bad = bytes;
    h = reinterpret_cast<elf::Elf64Header*>(bad.data());
    h->shnum = 129;
    EXPECT_FALSE(parse(bad));
    bad = bytes;
    auto* s = reinterpret_cast<Section*>(bad.data() + 64);
    s[2].offset = UINT64_MAX;
    EXPECT_FALSE(parse(bad));
    bad = bytes;
    s = reinterpret_cast<Section*>(bad.data() + 64);
    s[0] = s[2];
    EXPECT_FALSE(parse(bad));
    bad = bytes;
    s = reinterpret_cast<Section*>(bad.data() + 64);
    s[2].flags = 3;
    EXPECT_FALSE(parse(bad));
    bad = bytes;
    bad[512 + 14] = 'x';
    EXPECT_FALSE(parse(bad));
}

TEST(ConstructorMetadata, CallerConversionDefaultsBoundsAndDuplicates)
{
    auto schema = os1::counter::constructor_metadata;
    ASSERT_TRUE(os1::constructor::validate(schema));
    os1_constructor_args args{};
    ASSERT_TRUE(os1::constructor::parse(schema, 0, nullptr, args));
    EXPECT_EQ(0u, os1::constructor::decode(args.payload, 8));
    EXPECT_EQ(UINT64_MAX, os1::constructor::decode(args.payload + 8, 8));
    const char* options[] = {"--initial", "10", "--ceiling", "100"};
    ASSERT_TRUE(os1::constructor::parse(schema, 4, options, args));
    EXPECT_EQ(10u, os1::constructor::decode(args.payload, 8));
    auto valid = args;
    EXPECT_TRUE(os1::constructor::matches(schema, args));
    for(const char* bad : {"-1", "+1", "1x", "18446744073709551616", ""})
    {
        const char* input[] = {"--initial", bad};
        EXPECT_FALSE(os1::constructor::parse(schema, 2, input, args));
        EXPECT_EQ(0, std::memcmp(&valid, &args, sizeof(args)));
    }
    const char* duplicates[] = {"--initial", "1", "--initial", "2"};
    EXPECT_FALSE(os1::constructor::parse(schema, 4, duplicates, args));
    const char* unknown[] = {"--other", "1"};
    EXPECT_FALSE(os1::constructor::parse(schema, 2, unknown, args));
    EXPECT_FALSE(os1::constructor::parse(schema, 1, options, args));
    args.schema_id++;
    EXPECT_FALSE(os1::constructor::matches(schema, args));
    args = valid;
    args.payload[255] = 1;
    EXPECT_FALSE(os1::constructor::matches(schema, args));
    schema.fields[1].offset = 0;
    EXPECT_FALSE(os1::constructor::validate(schema));
    schema = os1::counter::constructor_metadata;
    schema.field_count = 9;
    EXPECT_FALSE(os1::constructor::validate(schema));
    schema = os1::counter::constructor_metadata;
    schema.fields[0].kind = Record;
    EXPECT_FALSE(os1::constructor::validate(schema));
}
}  // namespace

#include <string>

#include "interface/parameter_fixture.hpp"
namespace
{
namespace rp = os1::parameters;
const rp::Metadata rich = rp::fixture::make();
bool rich_parse(const rp::Metadata& m, const char* text, rp::Value& v, rp::Diagnostic& d)
{
    return rp::parse(m, text, static_cast<unsigned>(std::strlen(text)), v, d);
}
rp::Wire rich_wire(const rp::Metadata& m, const char* text)
{
    rp::Value v{};
    rp::Diagnostic d{};
    rp::Wire w{};
    EXPECT_TRUE(rich_parse(m, text, v, d)) << text << ": " << d.path << " " << d.explanation;
    EXPECT_TRUE(rp::encode(m, v, w, d)) << d.explanation;
    return w;
}
void same_wire(const rp::Wire& a, const rp::Wire& b)
{
    ASSERT_EQ(a.size, b.size);
    EXPECT_EQ(0, std::memcmp(a.data, b.data, a.size));
}
TEST(RichParameters, SchemaAndSharedConstructorMethod)
{
    rp::Diagnostic d{};
    ASSERT_TRUE(rp::validate(rich, d)) << d.explanation;
    auto constructor =
        rich_wire(rich, "initial:10 source.text:\"hello\" options:{limit:20, fallback.none}");
    auto method =
        rich_wire(rich, "options.limit:20 options.fallback:none source.text:'hello' initial:10");
    same_wire(constructor, method);
    rp::Value decoded{};
    ASSERT_TRUE(rp::decode(rich, constructor.data, constructor.size, decoded, d));
    EXPECT_EQ(decoded.nodes[rp::child_key(decoded, 0, 1)].number, 10);
    EXPECT_EQ(decoded.nodes[rp::child_key(decoded, 0, 2)].number, 100);
}
TEST(RichParameters, SchemaRejectsInvalidReferencesBoundsCyclesAndText)
{
    auto reject = [](rp::Metadata m) {
        rp::Diagnostic d{};
        EXPECT_FALSE(rp::validate(m, d));
        EXPECT_NE(d.code, rp::Error::None);
    };
    auto m = rich;
    m.magic++;
    reject(m);
    m = rich;
    m.version++;
    reject(m);
    m = rich;
    m.size--;
    reject(m);
    m = rich;
    m.types[0].id = 0;
    reject(m);
    m = rich;
    m.types[1].id = m.types[0].id;
    reject(m);
    m = rich;
    m.types[1].text.name = m.types[0].text.name;
    reject(m);
    m = rich;
    m.entries[0].type = rp::type_limit;
    reject(m);
    m = rich;
    m.entries[0].type = 0;
    reject(m);
    m = rich;
    m.types[rp::fixture::OptionalMode].element = rp::fixture::OptionalMode;
    reject(m);
    m = rich;
    m.entries[1].id = m.entries[0].id;
    reject(m);
    m = rich;
    m.entries[1].text.name = m.entries[0].text.name;
    reject(m);
    m = rich;
    m.entries[7].number = m.entries[6].number;
    reject(m);
    m = rich;
    m.entries[7].number = uint64_t{1} << 32;
    reject(m);
    m = rich;
    m.types[rp::fixture::U32].maximum = uint64_t{1} << 32;
    reject(m);
    m = rich;
    m.types[rp::fixture::U32].minimum = 1001;
    reject(m);
    m = rich;
    m.types[rp::fixture::TextString].bound = 257;
    reject(m);
    m = rich;
    m.defaults[m.entries[2].default_offset] = 99;
    reject(m);
    m = rich;
    m.defaults[m.entries[3].default_offset] = 99;
    reject(m);
    m = rich;
    m.entries[0].default_size = 9;
    reject(m);
    m = rich;
    m.entries[0].text.summary.size = 161;
    reject(m);
    m = rich;
    m.entries[0].text.description.size = 2049;
    reject(m);
    m = rich;
    m.strings[m.entries[0].text.summary.offset] = char(0xc0);
    reject(m);
    m = rich;
    m.strings[m.entries[0].text.name.offset] = '1';
    reject(m);
    m = rich;
    m.strings[m.text_size] = 1;
    reject(m);
    m = rich;
    m.reserved = 1;
    reject(m);
    m = rich;
    m.types[rp::fixture::OptionalMode].reserved2 = 1;
    reject(m);
    m = rich;
    m.entries[0].default_offset = rp::default_limit;
    reject(m);
}
TEST(RichParameters, DotBraceQuotingAndDeterministicRoundtrip)
{
    const char* examples[] = {"",
                              "mode:fast",
                              "mode.fast",
                              "source.stdin",
                              "source.none",
                              "source.text:\"\"",
                              "source.file:{path:\"/tmp/a:b.txt\"}",
                              "source.file.path:'/tmp/a:b.txt'",
                              "label:null",
                              "label:\"null\"",
                              "label:\"A \\\"quoted\\\" title\\n\\r\\t\\\\\"",
                              "label:'a \\\"literal\\\" title'",
                              "options:{fallback.none,prefix:\"\",suffix:null}",
                              "label:\"O'Brien\"",
                              "label:\"café\""};
    for(auto input : examples)
    {
        rp::Value v{}, back{};
        rp::Diagnostic d{};
        ASSERT_TRUE(rich_parse(rich, input, v, d))
            << input << " " << d.path << " " << d.explanation;
        char output[2048]{};
        unsigned size = 0;
        ASSERT_TRUE(rp::print(rich, v, output, sizeof(output), size, d)) << d.explanation;
        ASSERT_TRUE(rp::parse(rich, output, size, back, d))
            << output << " " << d.path << " " << d.explanation;
        rp::Wire a{}, b{};
        ASSERT_TRUE(rp::encode(rich, v, a, d));
        ASSERT_TRUE(rp::encode(rich, back, b, d));
        same_wire(a, b);
        char output2[2048];
        unsigned size2 = 0;
        ASSERT_TRUE(rp::print(rich, back, output2, sizeof(output2), size2, d));
        EXPECT_STREQ(output, output2);
    }
    same_wire(rich_wire(rich, "source.file:{path:\"a\",offset:0} mode:fast"),
              rich_wire(rich, "mode.fast source.file.path:a source.file.offset:0"));
    same_wire(
        rich_wire(rich, ""),
        rich_wire(
            rich,
            "initial:0 ceiling:100 mode.balanced source.stdin "
            "options:{limit:10,enabled:true,prefix:null,fallback:null,suffix:\"!\"} label:null"));
}
TEST(RichParameters, RejectsConflictsInBothOrders)
{
    const char* pairs[][2] = {{"mode:fast", "mode.fast"},
                              {"mode.fast", "mode.thorough"},
                              {"source.text:a", "source.file.path:b"},
                              {"source.stdin", "source.stdin"},
                              {"options:{limit:10}", "options.limit:20"},
                              {"label:null", "label:\"x\""},
                              {"options.fallback:null", "options.fallback.none"},
                              {"source.file:{path:a}", "source.file.offset:2"}};
    for(auto& pair : pairs)
        for(unsigned order = 0; order < 2; ++order)
        {
            std::string text = std::string(pair[order]) + " " + pair[1 - order];
            rp::Value v{};
            rp::Diagnostic d{};
            EXPECT_FALSE(rich_parse(rich, text.c_str(), v, d)) << text;
            EXPECT_EQ(d.code, rp::Error::Conflict) << text << " " << d.explanation;
            EXPECT_NE(d.path[0], 0);
            EXPECT_GE(d.end, d.begin);
        }
}
TEST(RichParameters, RejectsMalformedCLIAndReportsCanonicalMissingPath)
{
    const char* bad[] = {"source",
                         "source.text",
                         "enabled",
                         "initial",
                         "mode",
                         "mode.unknown",
                         "mode:1",
                         "mode.fast:1",
                         "mode.fast.extra",
                         "source.stdin:x",
                         "source:\"x\"",
                         "source:text:x",
                         "source:{case:text,value:x}",
                         "source.file:{}",
                         "unknown:1",
                         "options.enabled:1",
                         "initial:-1",
                         "initial:18446744073709551616",
                         "initial:2x",
                         "options.limit:1001",
                         "label:'oops",
                         "label:\"bad\\q\"",
                         "label:nullx\"",
                         "label:\"a\"b",
                         "label:a:b",
                         "label:\"x\",mode.fast",
                         "options:{limit:2,}",
                         "options:{limit:2 enabled:true}",
                         "options:null",
                         "initial:null",
                         "source.file.path:null"};
    for(auto input : bad)
    {
        rp::Value v{};
        rp::Diagnostic d{};
        EXPECT_FALSE(rich_parse(rich, input, v, d)) << input;
        EXPECT_NE(d.code, rp::Error::None);
    }
    rp::Value v{};
    rp::Diagnostic d{};
    EXPECT_FALSE(rich_parse(rich, "source.file:{}", v, d));
    EXPECT_EQ(d.code, rp::Error::Missing);
    EXPECT_STREQ(d.path, "source.file.path");
    EXPECT_EQ(d.expected_type, rp::fixture::TextString);
    const char malformed[] = {
        'l', 'a', 'b', 'e', 'l', ':', '"', char(0xed), char(0xa0), char(0x80), '"'};
    EXPECT_FALSE(rp::parse(rich, malformed, sizeof(malformed), v, d));
    EXPECT_EQ(d.code, rp::Error::Utf8);
    std::string too_long = "label:\"" + std::string(65, 'a') + "\"";
    EXPECT_FALSE(rich_parse(rich, too_long.c_str(), v, d));
    EXPECT_EQ(d.code, rp::Error::Limit);
}
// Small independently authored codec schema, not generated from the implementation.
rp::Metadata golden_schema()
{
    rp::Metadata m{};
    m.magic = rp::metadata_magic;
    m.version = 1;
    m.size = sizeof(m);
    m.schema_id = 1;
    m.schema_version = 1;
    m.type_count = 5;
    m.entry_count = 4;
    m.types[0] = rp::fixture::compound(m, 1, rp::Kind::Record, 0, 2, "Input");
    m.types[1] = rp::fixture::scalar(m, 2, rp::Kind::String, 0, 0, 0, "Text");
    m.types[1].bound = 16;
    m.types[2] = rp::fixture::compound(m, 3, rp::Kind::Optional, 0, 0, "OptionalText");
    m.types[2].element = 1;
    m.types[3] = rp::fixture::compound(m, 4, rp::Kind::Variant, 2, 2, "Source");
    m.types[4] = rp::fixture::scalar(m, 5, rp::Kind::Unsigned, 2, 0, 65535, "Count");
    m.entries[0] = rp::fixture::entry(m, 2, 2, "label");
    m.entries[1] = rp::fixture::entry(m, 7, 3, "source");
    m.entries[2] = rp::fixture::entry(m, 3, rp::no_index, "none");
    m.entries[3] = rp::fixture::entry(m, 9, 4, "count");
    return m;
}
TEST(RichParameters, GoldenOptionalEmptyStringVariantAndScalarBytes)
{
    auto m = golden_schema();
    ASSERT_TRUE(rp::validate(m));
    const uint8_t none[] = {2, 0, 2, 0, 0, 0, 1, 0, 0, 7, 0, 0, 0, 6, 0, 3, 0, 0, 0, 0, 0};
    const uint8_t empty[] = {2, 0, 2, 0, 0, 0, 3, 0, 1, 0, 0,    7,   0,
                             0, 0, 8, 0, 9, 0, 0, 0, 2, 0, 0x34, 0x12};
    auto a = rich_wire(m, "source.none");
    ASSERT_EQ(a.size, sizeof(none));
    EXPECT_EQ(0, std::memcmp(a.data, none, sizeof(none)));
    auto b = rich_wire(m, "source.count:4660 label:\"\"");
    ASSERT_EQ(b.size, sizeof(empty));
    EXPECT_EQ(0, std::memcmp(b.data, empty, sizeof(empty)));
    rp::Value v{};
    rp::Diagnostic d{};
    ASSERT_TRUE(rp::decode(m, none, sizeof(none), v, d));
    EXPECT_EQ(v.nodes[rp::child_key(v, 0, 2)].number, 0);
    ASSERT_TRUE(rp::decode(m, empty, sizeof(empty), v, d));
    auto label = rp::child_key(v, 0, 2);
    EXPECT_EQ(v.nodes[label].number, 1);
    EXPECT_EQ(v.nodes[v.nodes[label].first].size, 0);
    for(unsigned size = 0; size < sizeof(empty); ++size)
        EXPECT_FALSE(rp::decode(m, empty, size, v, d)) << size;
    for(unsigned offset : {0u, 2u, 6u, 8u, 11u, 15u, 17u, 21u})
    {
        auto bad = b;
        bad.data[offset] = 255;
        EXPECT_FALSE(rp::decode(m, bad.data, bad.size, v, d)) << offset;
    }
    auto bad = a;
    bad.data[9] = 2;
    EXPECT_FALSE(rp::decode(m, bad.data, bad.size, v, d));  // duplicate ID
    bad = a;
    bad.data[8] = 2;
    EXPECT_FALSE(rp::decode(m, bad.data, bad.size, v, d));  // optional tag
    bad = a;
    bad.data[15] = 99;
    EXPECT_FALSE(rp::decode(m, bad.data, bad.size, v, d));  // case ID
    bad = a;
    bad.data[bad.size++] = 0;
    EXPECT_FALSE(rp::decode(m, bad.data, bad.size, v, d));
}
TEST(RichParameters, EnumGoldenMembershipCompatibilityAndCppCasts)
{
    auto m = golden_schema();
    m.types[4].kind = rp::Kind::Enum;
    m.types[4].first = 4;
    m.types[4].count = 2;
    m.entry_count = 6;
    m.entries[4] = rp::fixture::entry(m, 1, rp::no_index, "fast");
    m.entries[4].number = 0x1234;
    m.entries[5] = rp::fixture::entry(m, 2, rp::no_index, "none");
    m.entries[5].number = 9;
    auto w = rich_wire(m, "source.count.fast");
    EXPECT_EQ(w.data[w.size - 2], 0x34);
    EXPECT_EQ(w.data[w.size - 1], 0x12);
    same_wire(w, rich_wire(m, "source.count:fast"));
    rp::Value v{};
    rp::Diagnostic d{};
    ASSERT_TRUE(rp::decode(m, w.data, w.size, v, d));
    auto source = rp::child_key(v, 0, 7);
    v.nodes[v.nodes[source].first].number = 99;
    EXPECT_FALSE(rp::encode(m, v, w, d));
    EXPECT_EQ(d.code, rp::Error::Range);
    auto newer = m;
    newer.types[4].count = 3;
    newer.entry_count = 7;
    newer.entries[6] = rp::fixture::entry(newer, 3, rp::no_index, "newer");
    newer.entries[6].number = 77;
    w = rich_wire(newer, "source.count.newer");
    EXPECT_FALSE(rp::decode(m, w.data, w.size, v, d));
    EXPECT_EQ(d.code, rp::Error::Range);
    EXPECT_NE(rp::semantic_fingerprint(m), rp::semantic_fingerprint(newer));
}
TEST(RichParameters, TransactionalOutputsAndTransportBoundary)
{
    rp::Value v{};
    rp::Diagnostic d{};
    ASSERT_TRUE(rich_parse(rich, "", v, d));
    auto saved = v;
    EXPECT_FALSE(rich_parse(rich, "mode:unknown", v, d));
    EXPECT_EQ(0, std::memcmp(&saved, &v, sizeof(v)));
    uint8_t bad[] = {0};
    EXPECT_FALSE(rp::decode(rich, bad, sizeof(bad), v, d));
    EXPECT_EQ(0, std::memcmp(&saved, &v, sizeof(v)));
    rp::Wire w{};
    ASSERT_TRUE(rp::encode(rich, v, w, d));
    auto old = w;
    v.nodes[rp::child_key(v, 0, 3)].number = 99;
    EXPECT_FALSE(rp::encode(rich, v, w, d));
    EXPECT_EQ(0, std::memcmp(&old, &w, sizeof(w)));
    char buffer[] = "unchanged";
    unsigned size = 123;
    EXPECT_FALSE(rp::print(rich, saved, buffer, sizeof(buffer), size, d));
    EXPECT_STREQ(buffer, "unchanged");
    EXPECT_EQ(size, 123);
    auto m = golden_schema();
    m.types[1].bound = 256;
    // Fixed overhead 23 bytes plus label text: exact payload boundary.
    std::string input = "source.none label:\"" + std::string(233, 'x') + "\"";
    ASSERT_TRUE(rich_parse(m, input.c_str(), v, d)) << d.explanation;
    ASSERT_TRUE(rp::encode(m, v, w, d));
    EXPECT_EQ(w.size, 256);
    input = "source.none label:\"" + std::string(234, 'x') + "\"";
    EXPECT_FALSE(rich_parse(m, input.c_str(), v, d));
    EXPECT_EQ(d.code, rp::Error::Limit);
}
TEST(RichParameters, DefaultRulesNoneVersusNamedNoneAndNoRecordOverlay)
{
    auto m = rich;
    // Whole-record default limit=99; explicitly supplied options resolves limit=10.
    m.defaults[m.entries[4].default_offset + 8] = 99;
    auto omitted = rich_wire(m, "");
    auto supplied = rich_wire(m, "options:{}");
    EXPECT_NE(std::memcmp(omitted.data, supplied.data, omitted.size), 0);
    same_wire(supplied, rich_wire(m, "options.limit:10"));
    auto none = rich_wire(m, "options.fallback:null");
    auto named = rich_wire(m, "options.fallback.none");
    EXPECT_NE(none.size, named.size);
    auto defaulted = rich_wire(m, "options:{}");
    auto nulled = rich_wire(m, "options.suffix:null");
    EXPECT_NE(defaulted.size, nulled.size);
    // An optional record remains None despite all its children's defaults.
    m.types[11] = rp::fixture::compound(m, 12, rp::Kind::Optional, 0, 0, "OptionalOptions");
    m.types[11].element = rp::fixture::Options;
    m.type_count = 12;
    m.entries[4].type = 11;
    m.entries[4].default_size = m.entries[4].default_offset = 0;
    rp::Value v{};
    rp::Diagnostic d{};
    ASSERT_TRUE(rich_parse(m, "", v, d));
    EXPECT_EQ(v.nodes[rp::child_key(v, 0, 5)].number, 0);
    ASSERT_TRUE(rich_parse(m, "options:{}", v, d));
    EXPECT_EQ(v.nodes[rp::child_key(v, 0, 5)].number, 1);
}
TEST(RichParameters, DocumentationPreservedAndNotSemantic)
{
    auto m = rich;
    auto before = rp::semantic_fingerprint(m);
    ASSERT_NE(before, 0);
    m.strings[m.entries[0].text.summary.offset] = 'X';
    EXPECT_TRUE(rp::validate(m));
    EXPECT_EQ(before, rp::semantic_fingerprint(m));
    EXPECT_EQ(m.entries[0].text.summary.size, 22);
    auto with_long = rich;
    with_long.entries[0].text.description = {0, 2048};
    for(unsigned i = 0; i < 2048; ++i)
        with_long.strings[i] = 'a';
    // Direct bounded text validation separately pins maximum description size.
    with_long.text_size = 2048;
    EXPECT_TRUE(rp::text_valid(with_long, with_long.entries[0].text.description, 2048));
    with_long.entries[0].text.description.size = 2049;
    EXPECT_FALSE(rp::text_valid(with_long, with_long.entries[0].text.description, 2048));
}
TEST(RichParameters, SignedIntegersAndBoundedLists)
{
    auto m = golden_schema();
    m.types[4].kind = rp::Kind::Signed;
    m.types[4].minimum = uint64_t(-32768);
    m.types[4].maximum = 32767;
    auto w = rich_wire(m, "source.count:-32768");
    EXPECT_EQ(w.data[w.size - 2], 0);
    EXPECT_EQ(w.data[w.size - 1], 128);
    rp::Value v{};
    rp::Diagnostic d{};
    EXPECT_FALSE(rich_parse(m, "source.count:32768", v, d));
    EXPECT_FALSE(rich_parse(m, "source.count:-32769", v, d));
    ASSERT_TRUE(rp::decode(m, w.data, w.size, v, d));
    char printed[256];
    unsigned length;
    ASSERT_TRUE(rp::print(m, v, printed, sizeof(printed), length, d));
    EXPECT_STREQ(printed, "label:null source.count:-32768");
    m.types[5] = rp::fixture::compound(m, 6, rp::Kind::List, 0, 0, "Numbers");
    m.types[5].element = 4;
    m.types[5].bound = 3;
    m.type_count = 6;
    m.entries[3].type = 5;
    w = rich_wire(m, "source.count:[-1,0,32767]");
    ASSERT_TRUE(rp::decode(m, w.data, w.size, v, d));
    ASSERT_TRUE(rp::print(m, v, printed, sizeof(printed), length, d));
    EXPECT_STREQ(printed, "label:null source.count:[-1,0,32767]");
    EXPECT_FALSE(rich_parse(m, "source.count:[0,0,0,0]", v, d));
    EXPECT_FALSE(rich_parse(m, "source.count:[0,]", v, d));
    w = rich_wire(m, "source.count:[]");
    ASSERT_TRUE(rp::decode(m, w.data, w.size, v, d));
}
}  // namespace
namespace
{
TEST(RichParameters, AggregateNodeAdmissionDepthAndNestedOptionalRejection)
{
    auto m = golden_schema();
    m.types[5] = rp::fixture::compound(m, 6, rp::Kind::List, 0, 0, "Many");
    m.types[5].element = 4;
    m.types[5].bound = 64;
    m.type_count = 6;
    EXPECT_FALSE(rp::validate(m));
    m.types[5].bound = 63;
    EXPECT_TRUE(rp::validate(m));
    m.entries[1].type = 5;
    EXPECT_FALSE(rp::validate(m));  // root adds nodes beyond list's own budget
    m = golden_schema();
    m.types[5] = rp::fixture::compound(m, 6, rp::Kind::Optional, 0, 0, "NestedOptional");
    m.types[5].element = 2;
    m.type_count = 6;
    EXPECT_FALSE(rp::validate(m));
    m = golden_schema();
    // Chain nonrecursive single-field records; depth 8 accepted, depth 9 rejected.
    m.entries[0].type = 5;
    for(unsigned i = 5; i < 12; ++i)
    {
        char name[] = {'T', char('a' + i), 0};
        m.types[i] = rp::fixture::compound(m, i + 1, rp::Kind::Record, m.entry_count, 1, name);
        m.entries[m.entry_count++] = rp::fixture::entry(m, 1, i == 11 ? 4 : i + 1, "child");
    }
    m.type_count = 12;
    ASSERT_TRUE(rp::validate(m));
    rp::Value v{};
    rp::Diagnostic d{};
    ASSERT_TRUE(rich_parse(
        m, "label:{child:{child:{child:{child:{child:{child:{child:1}}}}}}} source.none", v, d))
        << d.explanation;
    m.entries[m.types[11].first].type = 12;
    m.types[12] = rp::fixture::compound(m, 13, rp::Kind::Record, m.entry_count, 1, "TooDeep");
    m.entries[m.entry_count++] = rp::fixture::entry(m, 1, 4, "child");
    m.type_count = 13;
    EXPECT_FALSE(rp::validate(m));
}
TEST(RichParameters, ListsOfOptionalVariantsRoundtrip)
{
    auto m = golden_schema();
    m.types[5] = rp::fixture::compound(m, 6, rp::Kind::Optional, 0, 0, "MaybeSource");
    m.types[5].element = 3;
    m.types[6] = rp::fixture::compound(m, 7, rp::Kind::List, 0, 0, "Sources");
    m.types[6].element = 5;
    m.types[6].bound = 4;
    m.type_count = 7;
    m.entries[1].type = 6;
    rp::Value v{}, back{};
    rp::Diagnostic d{};
    ASSERT_TRUE(rich_parse(m, "source:[null,.none,.count:12]", v, d)) << d.explanation;
    char output[256];
    unsigned length = 0;
    ASSERT_TRUE(rp::print(m, v, output, sizeof(output), length, d));
    EXPECT_STREQ(output, "label:null source:[null,.none,.count:12]");
    ASSERT_TRUE(rp::parse(m, output, length, back, d)) << d.explanation;
    rp::Wire a{}, b{};
    ASSERT_TRUE(rp::encode(m, v, a, d));
    ASSERT_TRUE(rp::encode(m, back, b, d));
    same_wire(a, b);
}
TEST(RichParameters, RejectsCorruptedCppTreesTransactionally)
{
    rp::Value v{}, out{};
    rp::Diagnostic d{};
    ASSERT_TRUE(rich_parse(rich, "", v, d));
    out = v;
    auto bad = v;
    bad.nodes[0].next = 0;
    EXPECT_FALSE(rp::normalize(rich, bad, out, d));
    bad = v;
    bad.nodes[0].state = 3;
    EXPECT_FALSE(rp::normalize(rich, bad, out, d));
    bad = v;
    bad.nodes[0].first = rp::node_limit;
    EXPECT_FALSE(rp::normalize(rich, bad, out, d));
    bad = v;
    auto options = rp::child_key(bad, 0, 5);
    bad.nodes[options].next = options;
    EXPECT_FALSE(rp::normalize(rich, bad, out, d));
    EXPECT_EQ(0, std::memcmp(&v, &out, sizeof(v)));
}
}  // namespace
namespace
{
TEST(RichParameters, EmbeddedNulAndUtf8BoundariesPreserveLengths)
{
    const char input[] = {'l', 'a', 'b', 'e', 'l', ':', '"', 'a', 0, 'b', '"'};
    rp::Value v{}, back{};
    rp::Diagnostic d{};
    ASSERT_TRUE(rp::parse(rich, input, sizeof(input), v, d));
    char printed[512];
    unsigned length = 0;
    ASSERT_TRUE(rp::print(rich, v, printed, sizeof(printed), length, d));
    ASSERT_TRUE(rp::parse(rich, printed, length, back, d));
    rp::Wire a{}, b{};
    ASSERT_TRUE(rp::encode(rich, v, a, d));
    ASSERT_TRUE(rp::encode(rich, back, b, d));
    same_wire(a, b);
    const unsigned char invalid[][4] = {{0xc0, 0x80, 0, 0},
                                        {0xf4, 0x90, 0x80, 0x80},
                                        {0xed, 0xa0, 0x80, 0},
                                        {0x80, 0, 0, 0},
                                        {0xe2, 0x82, 0, 0}};
    const unsigned sizes[] = {2, 4, 3, 1, 2};
    for(unsigned i = 0; i < 5; ++i)
        EXPECT_FALSE(rp::utf8(reinterpret_cast<const char*>(invalid[i]), sizes[i]));
    const unsigned char largest[] = {0xf4, 0x8f, 0xbf, 0xbf};
    EXPECT_TRUE(rp::utf8(reinterpret_cast<const char*>(largest), 4));
}
TEST(RichParameters, TypedDefaultAndCppNormalizationBoundaries)
{
    auto m = rich;
    m.defaults[m.entries[20].default_offset] = 2;
    EXPECT_FALSE(rp::validate(m));
    m = rich;
    m.defaults[m.entries[17].default_offset] = 2;
    EXPECT_FALSE(rp::validate(m));
    m = rich;
    m.defaults[m.entries[4].default_offset + 2] = 2;
    EXPECT_FALSE(rp::validate(m));
    rp::Value partial{}, out{};
    rp::Diagnostic d{};
    ASSERT_TRUE(rp::initialize(partial, rich.root));
    partial.nodes[0].state = 1;
    ASSERT_TRUE(rp::children(rich, partial, 0, d));
    auto initial = rp::child_key(partial, 0, 1);
    partial.nodes[initial].state = 2;
    partial.nodes[initial].number = 37;
    ASSERT_TRUE(rp::normalize(rich, partial, out, d));
    rp::Wire w{};
    ASSERT_TRUE(rp::encode(rich, out, w, d));
    same_wire(w, rich_wire(rich, "initial:37"));
    auto mode = rp::child_key(partial, 0, 3);
    partial.nodes[mode].state = 2;
    partial.nodes[mode].number = 99;
    EXPECT_FALSE(rp::normalize(rich, partial, out, d));
    EXPECT_EQ(d.code, rp::Error::Range);
}
}  // namespace
namespace
{
TEST(RichParameters, GoldenNestedRecordAndNormalizedDefaults)
{
    auto m = golden_schema();
    m.types[4] = rp::fixture::compound(m, 5, rp::Kind::Record, 4, 1, "Details");
    m.types[5] = rp::fixture::scalar(m, 6, rp::Kind::Unsigned, 2, 0, 65535, "Amount");
    m.type_count = 6;
    m.entry_count = 5;
    m.entries[4] = rp::fixture::entry(m, 5, 5, "amount");
    rp::fixture::scalar_default(m, m.entries[4], 0x1234, 2);
    const uint8_t expected[] = {2, 0, 2, 0,  0, 0, 1, 0, 0, 7, 0, 0, 0, 16,   0,   9,
                                0, 0, 0, 10, 0, 1, 0, 5, 0, 0, 0, 2, 0, 0x34, 0x12};
    auto w = rich_wire(m, "source.count:{}");
    ASSERT_EQ(w.size, sizeof(expected));
    EXPECT_EQ(0, std::memcmp(w.data, expected, sizeof(expected)));
    same_wire(w, rich_wire(m, "source.count.amount:4660"));
    m.entries[1].default_offset = m.default_size;
    m.entries[1].default_size = 16;
    for(unsigned i = 15; i < sizeof(expected); ++i)
        m.defaults[m.default_size++] = expected[i];
    same_wire(w, rich_wire(m, ""));
    rp::Value v{};
    rp::Diagnostic d{};
    ASSERT_TRUE(rp::decode(m, expected, sizeof(expected), v, d));
    auto bad = w;
    bad.data[21] = 0;
    EXPECT_FALSE(rp::decode(m, bad.data, bad.size, v, d));
    bad = w;
    bad.data[23] = 6;
    EXPECT_FALSE(rp::decode(m, bad.data, bad.size, v, d));
}
TEST(RichParameters, DistinctMetadataElfSectionAndTransactionalExtraction)
{
    auto bytes = image();
    Section section{};
    std::memcpy(&section, bytes.data() + 64 + 2 * sizeof(Section), sizeof(section));
    section.size = sizeof(rich);
    std::memcpy(bytes.data() + 64 + 2 * sizeof(Section), &section, sizeof(section));
    Section strings{};
    std::memcpy(&strings, bytes.data() + 64 + sizeof(Section), sizeof(strings));
    strings.size = 16;
    std::memcpy(bytes.data() + 64 + sizeof(Section), &strings, sizeof(strings));
    std::memcpy(bytes.data() + 512, "\0.os.parameters", 16);
    std::memcpy(bytes.data() + 1024, &rich, sizeof(rich));
    auto reader = [&](uint64_t offset, void* out, size_t size) {
        if(offset > bytes.size() || size > bytes.size() - offset)
            return false;
        std::memcpy(out, bytes.data() + offset, size);
        return true;
    };
    rp::Metadata out{}, scratch{};
    ASSERT_TRUE(rp::read_elf(reader, bytes.size(), out, scratch));
    EXPECT_EQ(0, std::memcmp(&out, &rich, sizeof(out)));
    auto saved = out;
    bytes[1024] ^= 1;
    EXPECT_FALSE(rp::read_elf(reader, bytes.size(), out, scratch));
    EXPECT_EQ(0, std::memcmp(&saved, &out, sizeof(out)));
    EXPECT_FALSE(rp::read_elf(reader, bytes.size(), out, out));
    bytes[1024] ^= 1;
    bytes[513] = 'X';
    EXPECT_FALSE(rp::read_elf(reader, bytes.size(), out, scratch));
    EXPECT_EQ(0, std::memcmp(&saved, &out, sizeof(out)));
}
}  // namespace

namespace
{
TEST(BufferMetadata, BoundedProfileAndLegacyLayout)
{
    namespace bm = os1::buffer_metadata;
    const auto& api = os1::textprocessor::metadata;
    bm::Metadata m{bm::magic,
                   1,
                   sizeof(m),
                   api.interface_id,
                   api.interface_version,
                   2,
                   {{os1::textprocessor::LowercaseBuffer, bm::exclusive, 1, bm::return_input},
                    {os1::textprocessor::UppercaseBuffer, bm::exclusive, 1, bm::distinct_output}}};
    ASSERT_TRUE(bm::validate(m, api));
    EXPECT_EQ(sizeof(Metadata), 1704u);
    EXPECT_EQ(api.members[0].id, 1u);
    EXPECT_EQ(api.members[0].input, 2u);
    EXPECT_EQ(api.members[0].output, 2u);
    auto bad = m;
    bad.count = 9;
    EXPECT_FALSE(bm::validate(bad, api));
    bad = m;
    bad.members[0].mode = 2;
    EXPECT_FALSE(bm::validate(bad, api));
    bad = m;
    bad.members[0].output = 3;
    EXPECT_FALSE(bm::validate(bad, api));
    bad = m;
    bad.members[0].input_count = 2;
    EXPECT_FALSE(bm::validate(bad, api));
    bad = m;
    bad.members[1].member = bad.members[0].member;
    EXPECT_FALSE(bm::validate(bad, api));
    bad = m;
    bad.members[0].member = os1::textprocessor::Lowercase;
    EXPECT_FALSE(bm::validate(bad, api));
    bad = m;
    bad.members[0].member = os1::textprocessor::Progress;
    EXPECT_FALSE(bm::validate(bad, api));
    bad = m;
    bad.members[2].member = 9;
    EXPECT_FALSE(bm::validate(bad, api));
    bad = m;
    bad.interface_id++;
    EXPECT_FALSE(bm::validate(bad, api));
}
TEST(BufferMetadata, OfflineSectionValidation)
{
    namespace bm = os1::buffer_metadata;
    const auto& api = os1::textprocessor::metadata;
    bm::Metadata profile{
        bm::magic,
        1,
        sizeof(profile),
        api.interface_id,
        api.interface_version,
        1,
        {{os1::textprocessor::LowercaseBuffer, bm::exclusive, 1, bm::return_input}}};
    auto bytes = image();
    Section section{};
    std::memcpy(&section, bytes.data() + 64 + 2 * sizeof(Section), sizeof(section));
    section.size = sizeof(profile);
    std::memcpy(bytes.data() + 64 + 2 * sizeof(Section), &section, sizeof(section));
    std::memset(bytes.data() + 512, 0, 15);
    std::memcpy(bytes.data() + 513, ".os.buffers", 12);
    std::memcpy(bytes.data() + 1024, &profile, sizeof(profile));
    auto parse_profile = [&]() {
        bm::Metadata output{};
        return read_section(
            [&](uint64_t offset, void* out, size_t size) {
                if(offset > bytes.size() || size > bytes.size() - offset)
                    return false;
                std::memcpy(out, bytes.data() + offset, size);
                return true;
            },
            bytes.size(),
            ".os.buffers",
            output,
            [&](const bm::Metadata& m) { return bm::validate(m, api); });
    };
    EXPECT_EQ(parse_profile(), 1);
    bytes[1024] ^= 1;
    EXPECT_EQ(parse_profile(), -1);
    bytes[1024] ^= 1;
    bytes[513] = 'X';
    EXPECT_EQ(parse_profile(), 0);
}
TEST(BufferMetadata, RangeBoundsWithoutOverflow)
{
    using namespace os1::textprocessor;
    EXPECT_TRUE(valid_range({0, 0, 0, 65536}, 1, 65536));
    EXPECT_TRUE(valid_range({0, 0, 65536, 0}, 1, 65536));
    EXPECT_FALSE(valid_range({0, 0, 65536, 1}, 1, 65536));
    EXPECT_FALSE(valid_range({0, 0, UINT64_MAX, 2}, 1, 65536));
    EXPECT_FALSE(valid_range({0, 0, 1, UINT64_MAX}, 1, 65536));
    EXPECT_FALSE(valid_range({1, 0, 0, 1}, 1, 65536));
    EXPECT_FALSE(valid_range({0, 1, 0, 1}, 1, 65536));
    EXPECT_FALSE(valid_range({0, 0, 0, 1}, 0, 65536));
}
}  // namespace

TEST(InterfaceMetadata, PublicMembersUseSnakeCaseButTypesAllowUpperCamelCase)
{
    for(auto kind : {Method, PropertyGet, PropertySet, Event})
    {
        auto m = os1::textprocessor::metadata;
        auto& member = m.members[0];
        member.kind = kind;
        member.input = kind == PropertyGet ? 0 : 2;
        member.output = kind == Event ? 0 : 2;
        for(const char* name : {"read", "try_read", "state_changed", "read_v2"})
        {
            std::memset(member.name, 0, sizeof(member.name));
            std::strcpy(member.name, name);
            EXPECT_TRUE(validate(m)) << name;
        }
        for(const char* name :
            {"TryRead", "tryRead", "Info", "state-Changed", "_read", "read_", "read__now", "2read"})
        {
            std::memset(member.name, 0, sizeof(member.name));
            std::strcpy(member.name, name);
            EXPECT_FALSE(validate(m)) << name;
        }
    }
    EXPECT_TRUE(validate(os1::textprocessor::metadata));
    EXPECT_TRUE(validate(os1::counter::metadata));
}
