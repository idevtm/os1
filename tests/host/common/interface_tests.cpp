#include <gtest/gtest.h>

#include <cstring>
#include <vector>

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
    bad.members[7].id = 123;
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
