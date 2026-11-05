#pragma once
#include "interface/parameters.hpp"

namespace os1::parameters::fixture
{
constexpr uint64_t schema_id = 0x315247464331534full;
enum TypeIndex : uint16_t
{
    Input,
    U64,
    U32,
    Bool,
    TextString,
    Mode,
    OptionalMode,
    File,
    Source,
    Options,
    OptionalString
};
enum class ProcessingMode : uint32_t
{
    fast = 1,
    balanced = 2,
    thorough = 3,
    none = 4
};
constexpr Text text(Metadata& m, const char* s)
{
    if(!*s)
        return {};
    Text result{m.text_size, 0};
    while(*s)
    {
        m.strings[m.text_size++] = *s++;
        ++result.size;
    }
    return result;
}
constexpr Documentation docs(Metadata& m,
                             const char* name,
                             const char* summary = "",
                             const char* description = "")
{
    return {text(m, name), text(m, summary), text(m, description), {}, {}};
}
constexpr Type scalar(Metadata& m,
                      uint32_t id,
                      Kind kind,
                      unsigned width,
                      uint64_t min,
                      uint64_t max,
                      const char* name)
{
    Type t{};
    t.id = id;
    t.kind = kind;
    t.width = width;
    t.minimum = min;
    t.maximum = max;
    t.text = docs(m, name);
    return t;
}
constexpr Type compound(
    Metadata& m, uint32_t id, Kind kind, unsigned first, unsigned count, const char* name)
{
    Type t{};
    t.id = id;
    t.kind = kind;
    t.first = first;
    t.count = count;
    t.text = docs(m, name);
    return t;
}
constexpr Entry entry(Metadata& m,
                      uint32_t id,
                      uint16_t type,
                      const char* name,
                      const char* summary = "",
                      const char* description = "")
{
    Entry e{};
    e.id = id;
    e.type = type;
    e.text = docs(m, name, summary, description);
    return e;
}
constexpr void scalar_default(Metadata& m, Entry& e, uint64_t value, unsigned width)
{
    e.default_offset = m.default_size;
    e.default_size = width;
    for(unsigned i = 0; i < width; ++i)
        m.defaults[m.default_size++] = static_cast<uint8_t>(value >> (8 * i));
}
// Return by value for constexpr authoring. Providers should embed exactly one
// instance in .os.parameters and reference that instance for all runtime calls.
constexpr Metadata make()
{
    Metadata m{};
    m.magic = metadata_magic;
    m.version = 1;
    m.size = sizeof(m);
    m.schema_id = schema_id;
    m.schema_version = 1;
    m.root = Input;
    m.type_count = 11;
    m.entry_count = 21;
    m.types[Input] = compound(m, 1, Kind::Record, 0, 6, "Parameters");
    m.types[Input].text.summary = text(m, "Shared constructor and method input.");
    m.types[U64] = scalar(m, 2, Kind::Unsigned, 8, 0, UINT64_MAX, "Count");
    m.types[U32] = scalar(m, 3, Kind::Unsigned, 4, 0, 1000, "Limit");
    m.types[Bool] = scalar(m, 4, Kind::Boolean, 1, 0, 1, "Enabled");
    m.types[TextString] = scalar(m, 5, Kind::String, 0, 0, 0, "Text");
    m.types[TextString].bound = 64;
    m.types[Mode] = compound(m, 6, Kind::Enum, 6, 4, "ProcessingMode");
    m.types[Mode].width = 4;
    m.types[Mode].maximum = UINT32_MAX;
    m.types[OptionalMode] = compound(m, 7, Kind::Optional, 0, 0, "OptionalMode");
    m.types[OptionalMode].element = Mode;
    m.types[File] = compound(m, 8, Kind::Record, 10, 2, "FileSource");
    m.types[Source] = compound(m, 9, Kind::Variant, 12, 4, "Source");
    m.types[Options] = compound(m, 10, Kind::Record, 16, 5, "Options");
    m.types[OptionalString] = compound(m, 11, Kind::Optional, 0, 0, "OptionalText");
    m.types[OptionalString].element = TextString;
    m.entries[0] = entry(m, 1, U64, "initial", "Initial counter value.");
    scalar_default(m, m.entries[0], 0, 8);
    m.entries[1] = entry(m, 2, U64, "ceiling", "Maximum counter value.");
    scalar_default(m, m.entries[1], 100, 8);
    m.entries[2] = entry(m, 3, Mode, "mode", "Processing tradeoff.");
    scalar_default(m, m.entries[2], 2, 4);
    m.entries[3] = entry(m,
                         4,
                         Source,
                         "source",
                         "Input source.",
                         "A file path is data; parsing it does not open a file.");
    m.entries[3].default_offset = m.default_size;
    m.entries[3].default_size = 6;
    m.defaults[m.default_size++] = 3;
    m.default_size += 5;
    m.entries[4] = entry(m, 5, Options, "options", "Processing options.");
    // Complete canonical record default for options, authored after field defaults.
    m.entries[5] = entry(m, 6, OptionalString, "label", "Optional label.");
    const char* names[] = {"fast", "balanced", "thorough", "none"};
    const char* summaries[] = {"Favor speed.",
                               "Balance speed and precision.",
                               "Favor precision.",
                               "A named present value, distinct from absence."};
    for(unsigned i = 0; i < 4; ++i)
    {
        m.entries[6 + i] = entry(m, i + 1, no_index, names[i], summaries[i]);
        m.entries[6 + i].number = i + 1;
    }
    m.entries[10] = entry(m, 1, TextString, "path", "Path text.");
    m.entries[11] = entry(m, 2, U64, "offset", "Byte offset.");
    scalar_default(m, m.entries[11], 0, 8);
    m.entries[12] = entry(m, 1, TextString, "text", "Literal text source.");
    m.entries[13] = entry(m, 2, File, "file", "File source description.");
    m.entries[14] = entry(m, 3, no_index, "stdin", "Use standard input.");
    m.entries[15] = entry(m, 4, no_index, "none", "A present source with no payload.");
    m.entries[16] = entry(m, 1, U32, "limit", "Maximum entries.");
    scalar_default(m, m.entries[16], 10, 4);
    m.entries[16].text.unit = text(m, "items");
    m.entries[17] = entry(m, 2, Bool, "enabled", "Enable processing.");
    scalar_default(m, m.entries[17], 1, 1);
    m.entries[18] = entry(m, 3, OptionalString, "prefix", "Optional prefix.");
    m.entries[19] = entry(m, 4, OptionalMode, "fallback", "Optional fallback mode.");
    m.entries[20] = entry(m, 5, OptionalString, "suffix", "Default suffix.");
    // Optional Some("!") default. Explicit suffix:null suppresses this value.
    m.entries[20].default_offset = m.default_size;
    m.entries[20].default_size = 4;
    m.defaults[m.default_size++] = 1;
    m.defaults[m.default_size++] = 1;
    m.defaults[m.default_size++] = 0;
    m.defaults[m.default_size++] = '!';
    // options default: record field count=5, stable IDs 1..5 and typed payloads.
    auto& options = m.entries[4];
    options.default_offset = m.default_size;
    options.default_size = 43;
    m.defaults[m.default_size++] = 5;
    m.defaults[m.default_size++] = 0;
    for(unsigned i = 0; i < 5; ++i)
    {
        m.defaults[m.default_size++] = i + 1;
        m.default_size += 3;
        unsigned len = i == 0 ? 4 : i == 4 ? 4 : 1;
        m.defaults[m.default_size++] = len;
        m.defaults[m.default_size++] = 0;
        if(i == 0)
        {
            m.defaults[m.default_size++] = 10;
            m.default_size += 3;
        }
        else if(i == 1)
            m.defaults[m.default_size++] = 1;
        else if(i == 4)
        {
            m.defaults[m.default_size++] = 1;
            m.defaults[m.default_size++] = 1;
            m.defaults[m.default_size++] = 0;
            m.defaults[m.default_size++] = '!';
        }
        else
            m.defaults[m.default_size++] = 0;
    }
    return m;
}
}  // namespace os1::parameters::fixture
