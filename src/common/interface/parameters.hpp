#pragma once
#include <stddef.h>
#include <stdint.h>

#include "interface/metadata.hpp"

// Bounded rich parameters, profile 1. Metadata is a distinct little-endian ELF
// section (.os.parameters), never an interpretation of the older v1 sections.
namespace os1::parameters
{
constexpr uint64_t metadata_magic = 0x3152415031534full;  // OS1PAR1
constexpr uint16_t no_index = 0xffff;
constexpr unsigned type_limit = 16, entry_limit = 32, text_limit = 2304;
constexpr unsigned default_limit = 256, node_limit = 64, byte_limit = 256, depth_limit = 8;
constexpr unsigned name_limit = 31, summary_limit = 160, description_limit = 2048;
constexpr const char* section_name = ".os.parameters";
enum class Kind : uint8_t
{
    Unsigned = 1,
    Signed,
    Boolean,
    Enum,
    String,
    Record,
    Optional,
    Variant,
    List
};
struct Text
{
    uint16_t offset, size;
};
struct Documentation
{
    Text name, summary, description, unit, format;
};
struct Type
{
    uint32_t id;
    Kind kind;
    uint8_t width;  // integers/enums: 1, 2, 4, 8; Boolean: 1
    uint16_t first, count, element, bound;
    uint16_t reserved;
    uint64_t minimum, maximum;  // Signed: two's-complement int64 bounds
    Documentation text;
    uint32_t reserved2;
};
struct Entry
{
    uint32_t id;    // record field or variant case; enum tooling identity
    uint16_t type;  // no_index for a unit case or enumerator
    uint16_t default_offset, default_size, reserved;
    uint32_t reserved2;
    uint64_t number;  // enum wire identity; zero for other entries
    Documentation text;
    uint32_t reserved3;
};
// Fixed ELF container, all unused bytes/reserved fields zero. Text is length
// delimited UTF-8, not native pointers; defaults are canonical typed wire bytes.
struct Metadata
{
    uint64_t magic;
    uint32_t version, size;
    uint64_t schema_id;
    uint32_t schema_version;
    uint16_t root, type_count, entry_count, text_size, default_size, reserved;
    Type types[type_limit];
    Entry entries[entry_limit];
    char strings[text_limit];
    uint8_t defaults[default_limit];
};
static_assert(sizeof(Type) == 56 && sizeof(Entry) == 48);
static_assert(sizeof(Metadata) == 5032);
enum class Error : uint8_t
{
    None,
    Schema,
    Limit,
    Syntax,
    UnknownName,
    Conflict,
    Missing,
    Type,
    Range,
    Utf8,
    Noncanonical,
    Truncated
};
struct Diagnostic
{
    Error code{};
    uint16_t expected_type = no_index;
    uint32_t begin{}, end{};
    char path[256]{};
    const char* explanation = "";  // static library text, never metadata instructions
};
inline bool fail(Diagnostic& d, Error e, const char* why, uint16_t type = no_index)
{
    d.code = e;
    d.explanation = why;
    d.expected_type = type;
    return false;
}
inline bool zeros(const void* data, unsigned size)
{
    auto p = static_cast<const uint8_t*>(data);
    for(unsigned i = 0; i < size; ++i)
        if(p[i])
            return false;
    return true;
}
inline bool utf8(const char* p, unsigned n)
{
    for(unsigned i = 0; i < n;)
    {
        uint8_t c = static_cast<uint8_t>(p[i++]);
        if(c < 0x80)
            continue;
        unsigned tail;
        uint32_t cp, minimum;
        if(c >= 0xc2 && c <= 0xdf)
        {
            tail = 1;
            cp = c & 31;
            minimum = 0x80;
        }
        else if(c >= 0xe0 && c <= 0xef)
        {
            tail = 2;
            cp = c & 15;
            minimum = 0x800;
        }
        else if(c >= 0xf0 && c <= 0xf4)
        {
            tail = 3;
            cp = c & 7;
            minimum = 0x10000;
        }
        else
            return false;
        if(tail > n - i)
            return false;
        while(tail--)
        {
            c = static_cast<uint8_t>(p[i++]);
            if((c & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (c & 63);
        }
        if(cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
    }
    return true;
}
inline bool letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool ident(char c)
{
    return letter(c) || (c >= '0' && c <= '9') || c == '-';
}
inline bool equal(const Metadata& m, Text a, Text b)
{
    if(a.size != b.size)
        return false;
    for(unsigned i = 0; i < a.size; ++i)
        if(m.strings[a.offset + i] != m.strings[b.offset + i])
            return false;
    return true;
}
inline bool text_equal(const Metadata& m, Text a, const char* p, unsigned n)
{
    if(a.size != n)
        return false;
    for(unsigned i = 0; i < n; ++i)
        if(m.strings[a.offset + i] != p[i])
            return false;
    return true;
}
inline bool text_valid(const Metadata& m, Text t, unsigned limit, bool name = false)
{
    if(t.size > limit || t.offset > m.text_size || t.size > m.text_size - t.offset ||
       (!t.size && t.offset))
        return false;
    if(!utf8(m.strings + t.offset, t.size))
        return false;
    if(name)
    {
        if(!t.size || !letter(m.strings[t.offset]))
            return false;
        for(unsigned i = 1; i < t.size; ++i)
            if(!ident(m.strings[t.offset + i]))
                return false;
    }
    return true;
}
inline bool docs_valid(const Metadata& m, const Documentation& d)
{
    return text_valid(m, d.name, name_limit, true) && text_valid(m, d.summary, summary_limit) &&
           text_valid(m, d.description, description_limit) && text_valid(m, d.unit, 64) &&
           text_valid(m, d.format, 64);
}
inline uint64_t width_max(unsigned w)
{
    return w == 8 ? UINT64_MAX : ((uint64_t{1} << (w * 8)) - 1);
}
inline bool width_valid(unsigned w)
{
    return w == 1 || w == 2 || w == 4 || w == 8;
}
inline int64_t signed_value(uint64_t bits)
{
    return bits <= INT64_MAX ? static_cast<int64_t>(bits)
                             : -1 - static_cast<int64_t>(UINT64_MAX - bits);
}
inline bool scalar_valid(const Type& t, uint64_t v)
{
    if(t.kind == Kind::Signed)
        return signed_value(v) >= signed_value(t.minimum) &&
               signed_value(v) <= signed_value(t.maximum);
    return v >= t.minimum && v <= t.maximum;
}
inline uint16_t entry_number(const Metadata& m, const Type& t, uint64_t n)
{
    for(unsigned i = 0; i < t.count; ++i)
        if(m.entries[t.first + i].number == n)
            return t.first + i;
    return no_index;
}
inline uint16_t entry_id(const Metadata& m, const Type& t, uint32_t id)
{
    for(unsigned i = 0; i < t.count; ++i)
        if(m.entries[t.first + i].id == id)
            return t.first + i;
    return no_index;
}
inline uint16_t entry_name(const Metadata& m, const Type& t, const char* p, unsigned n)
{
    for(unsigned i = 0; i < t.count; ++i)
        if(text_equal(m, m.entries[t.first + i].text.name, p, n))
            return t.first + i;
    return no_index;
}
struct Node
{
    uint64_t number;
    uint32_t key;
    uint16_t type, first, next, begin, size;
    uint8_t state, reserved;  // 0 omitted, 1 partial, 2 explicitly assigned/normalized
};
struct Value
{
    uint16_t count{}, bytes{};
    Node nodes[node_limit]{};
    char strings[byte_limit]{};
};
static_assert(sizeof(Value) <= 2304);
struct Wire
{
    uint16_t size{};
    uint8_t data[byte_limit]{};
};
inline uint16_t add(Value& v, uint16_t type, uint32_t key = 0)
{
    if(v.count == node_limit)
        return no_index;
    uint16_t n = v.count++;
    v.nodes[n] = {};
    v.nodes[n].type = type;
    v.nodes[n].key = key;
    v.nodes[n].first = v.nodes[n].next = no_index;
    return n;
}
inline bool initialize(Value& v, uint16_t type)
{
    v = {};
    return add(v, type) != no_index;
}
inline bool children(const Metadata& m, Value& v, uint16_t n, Diagnostic& d)
{
    auto& node = v.nodes[n];
    const auto& t = m.types[node.type];
    if(node.first != no_index)
        return true;
    uint16_t last = no_index;
    for(unsigned i = 0; i < t.count; ++i)
    {
        const auto& e = m.entries[t.first + i];
        uint16_t child = add(v, e.type, e.id);
        if(child == no_index)
            return fail(d, Error::Limit, "decoded node limit", node.type);
        if(last == no_index)
            node.first = child;
        else
            v.nodes[last].next = child;
        last = child;
    }
    return true;
}
inline uint16_t child_key(const Value& v, uint16_t n, uint32_t key)
{
    for(uint16_t c = v.nodes[n].first; c != no_index; c = v.nodes[c].next)
        if(v.nodes[c].key == key)
            return c;
    return no_index;
}
namespace detail
{
inline bool decode_node(
    const Metadata&, uint16_t, const uint8_t*, unsigned, Value&, uint16_t, unsigned, Diagnostic&);
inline bool encode_node(const Metadata&, const Value&, uint16_t, Wire&, unsigned, Diagnostic&);
inline unsigned node_budget(const Metadata& m, uint16_t type)
{
    const auto& t = m.types[type];
    unsigned total = 1;
    if(t.kind == Kind::Optional)
        total += node_budget(m, t.element);
    else if(t.kind == Kind::List)
        total += t.bound * node_budget(m, t.element);
    else if(t.kind == Kind::Record)
        for(unsigned i = 0; i < t.count; ++i)
            total += node_budget(m, m.entries[t.first + i].type);
    else if(t.kind == Kind::Variant)
    {
        unsigned largest = 0;
        for(unsigned i = 0; i < t.count; ++i)
        {
            auto child = m.entries[t.first + i].type;
            unsigned budget = child == no_index ? 0 : node_budget(m, child);
            if(budget > largest)
                largest = budget;
        }
        total += largest;
    }
    return total > node_limit ? node_limit + 1 : total;
}
inline bool schema_walk(
    const Metadata& m, uint16_t type, uint32_t active, unsigned depth, Diagnostic& d)
{
    if(depth > depth_limit || (active & (uint32_t{1} << type)))
        return fail(d, Error::Schema, "recursive or excessive type depth", type);
    active |= uint32_t{1} << type;
    const auto& t = m.types[type];
    if(t.kind == Kind::Optional || t.kind == Kind::List)
        return schema_walk(m, t.element, active, depth + 1, d);
    if(t.kind == Kind::Record || t.kind == Kind::Variant)
        for(unsigned j = 0; j < t.count; ++j)
        {
            auto e = m.entries[t.first + j].type;
            if(e != no_index && !schema_walk(m, e, active, depth + 1, d))
                return false;
        }
    return true;
}
}  // namespace detail
inline bool validate(const Metadata& m, Diagnostic& d)
{
    d = {};
    if(m.magic != metadata_magic || m.version != 1 || m.size != sizeof(Metadata) || !m.schema_id ||
       !m.schema_version || m.reserved || !m.type_count || m.type_count > type_limit ||
       m.entry_count > entry_limit || m.root >= m.type_count || m.text_size > text_limit ||
       m.default_size > default_limit)
        return fail(d, Error::Schema, "invalid metadata header");
    if(!utf8(m.strings, m.text_size))
        return fail(d, Error::Schema, "invalid metadata UTF-8 table");
    if(m.types[m.root].kind != Kind::Record)
        return fail(d, Error::Schema, "input root must be a record");
    bool used[entry_limit]{};
    for(unsigned i = 0; i < m.type_count; ++i)
    {
        const auto& t = m.types[i];
        if(!t.id || !docs_valid(m, t.text) || t.reserved || t.reserved2 ||
           static_cast<unsigned>(t.kind) < 1 || static_cast<unsigned>(t.kind) > 9)
            return fail(d, Error::Schema, "invalid type or text", i);
        for(unsigned j = 0; j < i; ++j)
            if(t.id == m.types[j].id || equal(m, t.text.name, m.types[j].text.name))
                return fail(d, Error::Schema, "duplicate type identity", i);
        bool table = t.kind == Kind::Record || t.kind == Kind::Variant || t.kind == Kind::Enum;
        if(table)
        {
            if(t.first > m.entry_count || t.count > m.entry_count - t.first || t.element ||
               t.bound || ((t.kind != Kind::Record) && !t.count))
                return fail(d, Error::Schema, "invalid member table", i);
            uint32_t last = 0;
            for(unsigned j = 0; j < t.count; ++j)
            {
                unsigned k = t.first + j;
                const auto& e = m.entries[k];
                if(used[k] || !e.id || e.id <= last || !docs_valid(m, e.text) || e.reserved ||
                   e.reserved2 || e.reserved3)
                    return fail(d, Error::Schema, "invalid entry identity or text", i);
                used[k] = true;
                last = e.id;
                for(unsigned z = 0; z < j; ++z)
                    if(equal(m, e.text.name, m.entries[t.first + z].text.name) ||
                       (t.kind == Kind::Enum && e.number == m.entries[t.first + z].number))
                        return fail(d, Error::Schema, "duplicate entry name or enum value", i);
                if(t.kind == Kind::Enum)
                {
                    if(e.type != no_index || e.default_size || e.default_offset ||
                       !width_valid(t.width) || e.number > width_max(t.width))
                        return fail(d, Error::Schema, "invalid enumerator", i);
                }
                else if((e.type == no_index && t.kind != Kind::Variant) ||
                        (e.type != no_index && e.type >= m.type_count) || e.number)
                    return fail(d, Error::Schema, "invalid field/case reference", i);
                if(e.default_offset > m.default_size ||
                   e.default_size > m.default_size - e.default_offset ||
                   (!e.default_size && e.default_offset) ||
                   (t.kind != Kind::Record && e.default_size))
                    return fail(d, Error::Schema, "invalid default bounds", i);
            }
        }
        else if(t.first || t.count)
            return fail(d, Error::Schema, "unexpected member table", i);
        if(t.kind == Kind::Unsigned || t.kind == Kind::Enum || t.kind == Kind::Boolean)
        {
            if(!width_valid(t.width) || t.minimum > t.maximum || t.maximum > width_max(t.width) ||
               (t.kind == Kind::Boolean && (t.width != 1 || t.minimum || t.maximum != 1)) ||
               (t.kind == Kind::Enum && (t.minimum || t.maximum != width_max(t.width))))
                return fail(d, Error::Schema, "invalid integer bounds", i);
        }
        else if(t.kind == Kind::Signed)
        {
            if(!width_valid(t.width) || signed_value(t.minimum) > signed_value(t.maximum))
                return fail(d, Error::Schema, "invalid signed bounds", i);
            if(t.width < 8)
            {
                int64_t max = (int64_t{1} << (8 * t.width - 1)) - 1;
                if(signed_value(t.minimum) < -max - 1 || signed_value(t.maximum) > max)
                    return fail(d, Error::Schema, "signed bounds exceed width", i);
            }
        }
        else if(t.width || t.minimum || t.maximum)
            return fail(d, Error::Schema, "unexpected scalar constraints", i);
        if(t.kind == Kind::Optional || t.kind == Kind::List)
        {
            if(t.element >= m.type_count || (t.kind == Kind::Optional && t.bound) ||
               (t.kind == Kind::List && (!t.bound || t.bound > node_limit)))
                return fail(d, Error::Schema, "invalid element reference/bound", i);
        }
        else if(t.element)
            return fail(d, Error::Schema, "unexpected element reference", i);
        if(t.kind == Kind::Optional && m.types[t.element].kind == Kind::Optional)
            return fail(d, Error::Schema, "directly nested optionals have no CLI spelling", i);
        if(t.kind == Kind::String)
        {
            if(t.bound > byte_limit)
                return fail(d, Error::Schema, "string bound exceeds profile", i);
        }
        else if(t.kind != Kind::List && t.bound)
            return fail(d, Error::Schema, "unexpected length constraint", i);
    }
    for(unsigned i = 0; i < m.entry_count; ++i)
        if(!used[i])
            return fail(d, Error::Schema, "unowned entry");
    if(!zeros(m.types + m.type_count, sizeof(Type) * (type_limit - m.type_count)) ||
       !zeros(m.entries + m.entry_count, sizeof(Entry) * (entry_limit - m.entry_count)) ||
       !zeros(m.strings + m.text_size, text_limit - m.text_size) ||
       !zeros(m.defaults + m.default_size, default_limit - m.default_size))
        return fail(d, Error::Schema, "nonzero unused metadata");
    for(unsigned i = 0; i < m.type_count; ++i)
        if(!detail::schema_walk(m, i, 0, 0, d))
            return false;
    for(unsigned i = 0; i < m.type_count; ++i)
        if(detail::node_budget(m, i) > node_limit)
            return fail(d, Error::Schema, "type exceeds decoded node budget", i);
    // Validate typed constants without recursively revalidating the metadata.
    for(unsigned i = 0; i < m.entry_count; ++i)
        if(m.entries[i].default_size)
        {
            const auto& e = m.entries[i];
            Value v{};
            initialize(v, e.type);
            if(!detail::decode_node(
                   m, e.type, m.defaults + e.default_offset, e.default_size, v, 0, 0, d))
                return fail(d, Error::Schema, "invalid typed default", e.type);
        }
    return true;
}
inline bool validate(const Metadata& m)
{
    Diagnostic d{};
    return validate(m, d);
}
// The caller supplies the large scratch buffer so ELF extraction is transactional
// without adding a 5 KB automatic allocation to small freestanding stacks.
template<class Reader>
bool read_elf(Reader read, uint64_t file_size, Metadata& output, Metadata& scratch)
{
    if(&output == &scratch)
        return false;
    if(interface::read_section(read, file_size, section_name, scratch, [](const Metadata& m) {
           return validate(m);
       }) != 1)
        return false;
    output = scratch;
    return true;
}
namespace detail
{
inline uint64_t read_le(const uint8_t* p, unsigned n)
{
    uint64_t v = 0;
    for(unsigned i = 0; i < n; ++i)
        v |= uint64_t{p[i]} << (i * 8);
    return v;
}
inline bool put(Wire& w, uint64_t n, unsigned bytes, Diagnostic& d)
{
    if(bytes > byte_limit - w.size)
        return fail(d, Error::Limit, "256-byte transport limit");
    for(unsigned i = 0; i < bytes; ++i)
        w.data[w.size++] = static_cast<uint8_t>(n >> (8 * i));
    return true;
}
inline bool decode_node(const Metadata& m,
                        uint16_t type,
                        const uint8_t* p,
                        unsigned size,
                        Value& v,
                        uint16_t n,
                        unsigned depth,
                        Diagnostic& d)
{
    if(depth > depth_limit || type >= m.type_count || n >= v.count)
        return fail(d, Error::Limit, "decoded depth limit", type);
    auto& a = v.nodes[n];
    const auto& t = m.types[type];
    a.state = 2;
    if(t.kind == Kind::Unsigned || t.kind == Kind::Signed || t.kind == Kind::Boolean ||
       t.kind == Kind::Enum)
    {
        if(size != t.width)
            return fail(d, Error::Noncanonical, "wrong scalar width", type);
        a.number = read_le(p, size);
        if(t.kind == Kind::Signed && size < 8 && (a.number & (uint64_t{1} << (size * 8 - 1))))
            a.number |= ~width_max(size);
        if(!scalar_valid(t, a.number) ||
           (t.kind == Kind::Enum && entry_number(m, t, a.number) == no_index))
            return fail(d, Error::Range, "unknown enum or scalar outside range", type);
        return true;
    }
    if(t.kind == Kind::String)
    {
        if(size < 2)
            return fail(d, Error::Truncated, "missing string length", type);
        unsigned len = read_le(p, 2);
        if(len != size - 2)
            return fail(d, Error::Noncanonical, "string length mismatch", type);
        if(len > t.bound || len > byte_limit - v.bytes)
            return fail(d, Error::Limit, "string byte limit", type);
        if(!utf8(reinterpret_cast<const char*>(p + 2), len))
            return fail(d, Error::Utf8, "invalid UTF-8", type);
        a.begin = v.bytes;
        a.size = len;
        for(unsigned i = 0; i < len; ++i)
            v.strings[v.bytes++] = p[2 + i];
        return true;
    }
    if(t.kind == Kind::Optional)
    {
        if(!size)
            return fail(d, Error::Truncated, "missing optional tag", type);
        if(p[0] > 1 || (!p[0] && size != 1))
            return fail(d, Error::Noncanonical, "invalid optional tag/payload", type);
        a.number = p[0];
        if(!a.number)
            return true;
        a.first = add(v, t.element);
        if(a.first == no_index)
            return fail(d, Error::Limit, "decoded node limit", type);
        return decode_node(m, t.element, p + 1, size - 1, v, a.first, depth + 1, d);
    }
    if(t.kind == Kind::Variant)
    {
        if(size < 6)
            return fail(d, Error::Truncated, "missing variant header", type);
        a.number = read_le(p, 4);
        unsigned len = read_le(p + 4, 2);
        auto ei = entry_id(m, t, a.number);
        if(ei == no_index)
            return fail(d, Error::Range, "unknown variant case", type);
        if(len != size - 6)
            return fail(d, Error::Noncanonical, "variant length mismatch", type);
        auto payload = m.entries[ei].type;
        if(payload == no_index)
            return !len || fail(d, Error::Noncanonical, "unit case has payload", type);
        a.first = add(v, payload);
        if(a.first == no_index)
            return fail(d, Error::Limit, "decoded node limit", type);
        return decode_node(m, payload, p + 6, len, v, a.first, depth + 1, d);
    }
    if(size < 2)
        return fail(d, Error::Truncated, "missing collection count", type);
    unsigned count = read_le(p, 2), pos = 2;
    if((t.kind == Kind::Record && count != t.count) || (t.kind == Kind::List && count > t.bound))
        return fail(d, Error::Noncanonical, "noncanonical collection count", type);
    uint16_t last = no_index;
    for(unsigned i = 0; i < count; ++i)
    {
        unsigned header = t.kind == Kind::Record ? 6 : 2;
        if(header > size - pos)
            return fail(d, Error::Truncated, "missing field/element header", type);
        uint32_t key = t.kind == Kind::Record ? read_le(p + pos, 4) : i;
        uint16_t childtype = t.kind == Kind::Record ? m.entries[t.first + i].type : t.element;
        if(t.kind == Kind::Record && key != m.entries[t.first + i].id)
            return fail(d, Error::Noncanonical, "unknown, duplicate or unsorted field", type);
        unsigned len = read_le(p + pos + header - 2, 2);
        pos += header;
        if(len > size - pos)
            return fail(d, Error::Truncated, "truncated field/element", childtype);
        auto child = add(v, childtype, key);
        if(child == no_index)
            return fail(d, Error::Limit, "decoded node limit", childtype);
        if(last == no_index)
            a.first = child;
        else
            v.nodes[last].next = child;
        last = child;
        if(!decode_node(m, childtype, p + pos, len, v, child, depth + 1, d))
            return false;
        pos += len;
    }
    return pos == size || fail(d, Error::Noncanonical, "trailing collection bytes", type);
}
inline bool encode_node(
    const Metadata& m, const Value& v, uint16_t n, Wire& w, unsigned depth, Diagnostic& d)
{
    if(n >= v.count || depth > depth_limit)
        return fail(d, Error::Limit, "invalid node reference/depth");
    const auto& a = v.nodes[n];
    if(a.type >= m.type_count || a.state != 2 || a.reserved)
        return fail(d, Error::Type, "unresolved or invalid value", a.type);
    const auto& t = m.types[a.type];
    if(t.kind == Kind::Unsigned || t.kind == Kind::Signed || t.kind == Kind::Boolean ||
       t.kind == Kind::Enum)
    {
        if(a.first != no_index || !scalar_valid(t, a.number) ||
           (t.kind == Kind::Enum && entry_number(m, t, a.number) == no_index))
            return fail(d, Error::Range, "invalid scalar or enum value", a.type);
        return put(w, a.number, t.width, d);
    }
    if(t.kind == Kind::String)
    {
        if(a.first != no_index || a.size > t.bound || a.begin > v.bytes ||
           a.size > v.bytes - a.begin)
            return fail(d, Error::Limit, "invalid string extent", a.type);
        if(!utf8(v.strings + a.begin, a.size))
            return fail(d, Error::Utf8, "invalid UTF-8", a.type);
        if(!put(w, a.size, 2, d))
            return false;
        for(unsigned i = 0; i < a.size; ++i)
            if(!put(w, static_cast<uint8_t>(v.strings[a.begin + i]), 1, d))
                return false;
        return true;
    }
    if(t.kind == Kind::Optional)
    {
        if(a.number > 1 || (!a.number && a.first != no_index))
            return fail(d, Error::Type, "invalid optional state", a.type);
        if(!put(w, a.number, 1, d))
            return false;
        if(!a.number)
            return true;
        if(a.first >= v.count || v.nodes[a.first].type != t.element ||
           v.nodes[a.first].next != no_index)
            return fail(d, Error::Type, "invalid optional payload", a.type);
        return encode_node(m, v, a.first, w, depth + 1, d);
    }
    if(t.kind == Kind::Variant)
    {
        auto ei = entry_id(m, t, a.number);
        if(a.number > UINT32_MAX || ei == no_index)
            return fail(d, Error::Range, "unknown variant case", a.type);
        auto childtype = m.entries[ei].type;
        if(!put(w, a.number, 4, d))
            return false;
        unsigned length_at = w.size;
        if(!put(w, 0, 2, d))
            return false;
        unsigned start = w.size;
        if(childtype == no_index)
        {
            if(a.first != no_index)
                return fail(d, Error::Type, "unit case has payload", a.type);
        }
        else
        {
            if(a.first >= v.count || v.nodes[a.first].type != childtype ||
               v.nodes[a.first].next != no_index)
                return fail(d, Error::Type, "invalid variant payload", a.type);
            if(!encode_node(m, v, a.first, w, depth + 1, d))
                return false;
        }
        unsigned len = w.size - start;
        w.data[length_at] = len & 255;
        w.data[length_at + 1] = len >> 8;
        return true;
    }
    unsigned count = 0;
    uint16_t c = a.first;
    while(c != no_index)
    {
        if(c >= v.count || ++count > node_limit)
            return fail(d, Error::Type, "invalid collection chain", a.type);
        c = v.nodes[c].next;
    }
    if((t.kind == Kind::Record && count != t.count) || (t.kind == Kind::List && count > t.bound))
        return fail(d, Error::Type, "invalid collection count", a.type);
    if(!put(w, count, 2, d))
        return false;
    c = a.first;
    for(unsigned i = 0; i < count; ++i)
    {
        auto childtype = t.kind == Kind::Record ? m.entries[t.first + i].type : t.element;
        auto key = t.kind == Kind::Record ? m.entries[t.first + i].id : i;
        if(v.nodes[c].type != childtype || v.nodes[c].key != key)
            return fail(d, Error::Type, "invalid field/element identity", a.type);
        if(t.kind == Kind::Record && !put(w, key, 4, d))
            return false;
        unsigned at = w.size;
        if(!put(w, 0, 2, d))
            return false;
        unsigned start = w.size;
        if(!encode_node(m, v, c, w, depth + 1, d))
            return false;
        unsigned len = w.size - start;
        w.data[at] = len & 255;
        w.data[at + 1] = len >> 8;
        c = v.nodes[c].next;
    }
    return true;
}
inline unsigned push_path(Diagnostic& d, const Metadata& m, Text text)
{
    unsigned n = 0;
    while(d.path[n] && n < 255)
        ++n;
    unsigned old = n;
    if(n && n < 255)
        d.path[n++] = '.';
    for(unsigned i = 0; i < text.size && n < 255; ++i)
        d.path[n++] = m.strings[text.offset + i];
    d.path[n] = 0;
    return old;
}
inline bool normalize_node(const Metadata& m, Value& v, uint16_t n, unsigned depth, Diagnostic& d)
{
    if(n >= v.count || depth > depth_limit || v.nodes[n].type >= m.type_count)
        return fail(d, Error::Type, "invalid partial tree");
    auto& a = v.nodes[n];
    const auto& t = m.types[a.type];
    if(a.state > 2 || a.reserved)
        return fail(d, Error::Type, "invalid partial node state", a.type);
    if(!a.state)
    {
        if(t.kind == Kind::Optional)
        {
            a.number = 0;
            a.state = 2;
            return true;
        }
        return fail(d, Error::Missing, "required value missing", a.type);
    }
    if(t.kind == Kind::Record)
    {
        if(!children(m, v, n, d))
            return false;
        uint16_t c = a.first;
        for(unsigned i = 0; i < t.count; ++i)
        {
            const auto& e = m.entries[t.first + i];
            if(c >= v.count || v.nodes[c].key != e.id || v.nodes[c].type != e.type)
                return fail(d, Error::Type, "invalid record children", a.type);
            unsigned old = push_path(d, m, e.text.name);
            if(!v.nodes[c].state && e.default_size)
            {
                if(!decode_node(m,
                                e.type,
                                m.defaults + e.default_offset,
                                e.default_size,
                                v,
                                c,
                                depth + 1,
                                d))
                    return false;
            }
            else if(!normalize_node(m, v, c, depth + 1, d))
                return false;
            d.path[old] = 0;
            c = v.nodes[c].next;
        }
        if(c != no_index)
            return fail(d, Error::Type, "excess record children", a.type);
    }
    else if(t.kind == Kind::Optional)
    {
        if(a.number == 1)
        {
            if(!normalize_node(m, v, a.first, depth + 1, d))
                return false;
        }
        else if(a.number)
            return fail(d, Error::Type, "invalid optional tag", a.type);
    }
    else if(t.kind == Kind::Variant)
    {
        if(a.number > UINT32_MAX)
            return fail(d, Error::Range, "unknown variant case", a.type);
        auto ei = entry_id(m, t, a.number);
        if(ei == no_index)
            return fail(d, Error::Range, "unknown variant case", a.type);
        unsigned old = push_path(d, m, m.entries[ei].text.name);
        if(m.entries[ei].type != no_index && !normalize_node(m, v, a.first, depth + 1, d))
            return false;
        d.path[old] = 0;
    }
    else if(t.kind == Kind::List)
    {
        unsigned count = 0;
        for(auto c = a.first; c != no_index;)
        {
            if(c >= v.count || ++count > t.bound)
                return fail(d, Error::Limit, "invalid list chain", a.type);
            if(!normalize_node(m, v, c, depth + 1, d))
                return false;
            c = v.nodes[c].next;
        }
    }
    a.state = 2;
    return true;
}
}  // namespace detail
// All public conversion functions are transactional: output changes only on success.
inline bool encode(const Metadata& m, const Value& value, Wire& out, Diagnostic& d)
{
    if(!validate(m, d))
        return false;
    if(!value.count || value.count > node_limit || value.bytes > byte_limit ||
       value.nodes[0].type != m.root || value.nodes[0].next != no_index)
        return fail(d, Error::Type, "invalid value root");
    Wire w{};
    if(!detail::encode_node(m, value, 0, w, 0, d))
        return false;
    out = w;
    return true;
}
inline bool decode(
    const Metadata& m, const uint8_t* bytes, unsigned size, Value& out, Diagnostic& d)
{
    if(!validate(m, d))
        return false;
    if(size > byte_limit || (!bytes && size))
        return fail(d, Error::Limit, "invalid wire buffer");
    Value v{};
    initialize(v, m.root);
    if(!detail::decode_node(m, m.root, bytes, size, v, 0, 0, d))
        return false;
    out = v;
    return true;
}
inline bool normalize(const Metadata& m, const Value& partial, Value& out, Diagnostic& d)
{
    if(!validate(m, d))
        return false;
    if(!partial.count || partial.count > node_limit || partial.bytes > byte_limit ||
       partial.nodes[0].type != m.root || partial.nodes[0].next != no_index)
        return fail(d, Error::Type, "invalid partial root");
    Value v = partial;
    if(!detail::normalize_node(m, v, 0, 0, d))
        return false;
    Wire check{};
    if(!detail::encode_node(m, v, 0, check, 0, d))
        return false;
    out = v;
    return true;
}
namespace detail
{
struct Parser
{
    const Metadata& m;
    const char* input;
    unsigned length, pos{};
    Value& v;
    Diagnostic& d;
    bool space(char c) const
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }
    void skip()
    {
        while(pos < length && space(input[pos]))
            ++pos;
    }
    bool boundary(char c) const
    {
        return space(c) || c == ',' || c == '}' || c == ']';
    }
    bool error(Error e, const char* why, uint16_t type = no_index)
    {
        d.end = pos;
        return fail(d, e, why, type);
    }
    bool token_is(const char* text, unsigned count) const
    {
        if(count > length - pos)
            return false;
        for(unsigned i = 0; i < count; ++i)
            if(input[pos + i] != text[i])
                return false;
        return pos + count == length || boundary(input[pos + count]);
    }
    bool present(uint16_t& n)
    {
        if(v.nodes[n].state == 2)
            return error(
                Error::Conflict, "ancestor/descendant assignment overlap", v.nodes[n].type);
        while(m.types[v.nodes[n].type].kind == Kind::Optional)
        {
            auto& a = v.nodes[n];
            a.state = 1;
            a.number = 1;
            if(a.first == no_index)
            {
                a.first = add(v, m.types[a.type].element);
                if(a.first == no_index)
                    return error(Error::Limit, "decoded node limit", a.type);
            }
            n = a.first;
            if(v.nodes[n].state == 2)
                return error(
                    Error::Conflict, "ancestor/descendant assignment overlap", v.nodes[n].type);
        }
        return true;
    }
    bool select(uint16_t n, uint16_t ei, uint16_t& payload, bool& terminal)
    {
        auto& a = v.nodes[n];
        const auto& t = m.types[a.type];
        const auto& e = m.entries[ei];
        if(t.kind == Kind::Enum)
        {
            if(a.state)
                return error(Error::Conflict, "multiple enum selections", a.type);
            a.number = e.number;
            a.state = 2;
            terminal = true;
            payload = n;
            return true;
        }
        if(a.state && a.number != e.id)
            return error(Error::Conflict, "multiple variant cases selected", a.type);
        if(a.state == 2)
            return error(Error::Conflict, "duplicate variant assignment", a.type);
        a.number = e.id;
        a.state = 1;
        if(e.type == no_index)
        {
            a.state = 2;
            terminal = true;
            payload = n;
            return true;
        }
        if(a.first == no_index)
        {
            a.first = add(v, e.type);
            if(a.first == no_index)
                return error(Error::Limit, "decoded node limit", e.type);
        }
        payload = a.first;
        terminal = false;
        return true;
    }
    bool assignment(uint16_t base, unsigned depth)
    {
        if(depth > depth_limit * 2 + 2)
            return error(Error::Limit, "CLI nesting limit");
        uint16_t n = base;
        bool terminal = false;
        d.begin = pos;
        for(;;)
        {
            if(!present(n))
                return false;
            auto& a = v.nodes[n];
            const auto& t = m.types[a.type];
            unsigned start = pos;
            if(pos == length || !letter(input[pos]))
                return error(Error::Syntax, "expected canonical path name", a.type);
            ++pos;
            while(pos < length && ident(input[pos]))
                ++pos;
            if(t.kind != Kind::Record && t.kind != Kind::Variant && t.kind != Kind::Enum)
                return error(Error::Type, "path descends through terminal value", a.type);
            auto ei = entry_name(m, t, input + start, pos - start);
            if(ei == no_index)
                return error(Error::UnknownName, "unknown field, enumerator or case", a.type);
            push_path(d, m, m.entries[ei].text.name);
            if(t.kind == Kind::Record)
            {
                a.state = 1;
                if(!children(m, v, n, d))
                    return false;
                n = child_key(v, n, m.entries[ei].id);
            }
            else if(!select(n, ei, n, terminal))
                return false;
            if(pos < length && input[pos] == '.')
            {
                if(terminal)
                    return error(Error::Type, "selected enum/unit is terminal", v.nodes[n].type);
                ++pos;
                continue;
            }
            break;
        }
        if(terminal)
        {
            if(pos < length && input[pos] == ':')
                return error(Error::Type, "enum/unit takes no payload", v.nodes[n].type);
            return true;
        }
        if(pos == length || input[pos] != ':')
            return error(Error::Syntax, "value requires colon", v.nodes[n].type);
        ++pos;
        skip();
        return value(n, depth + 1);
    }
    bool string(uint16_t n)
    {
        auto& a = v.nodes[n];
        const auto& t = m.types[a.type];
        unsigned begin = v.bytes;
        char quote = pos < length && (input[pos] == '\'' || input[pos] == '"') ? input[pos++] : 0;
        bool closed = !quote;
        while(pos < length)
        {
            char c = input[pos];
            if(quote && c == quote)
            {
                ++pos;
                closed = true;
                break;
            }
            if(!quote && (boundary(c) || c == ':' || c == '{' || c == '[' || c == '\'' ||
                          c == '"' || c == '\\'))
                break;
            ++pos;
            if(quote == '"' && c == '\\')
            {
                if(pos == length)
                    return error(Error::Syntax, "unterminated escape", a.type);
                c = input[pos++];
                if(c == 'n')
                    c = '\n';
                else if(c == 'r')
                    c = '\r';
                else if(c == 't')
                    c = '\t';
                else if(c != '"' && c != '\\')
                    return error(Error::Syntax, "unknown string escape", a.type);
            }
            if(v.bytes - begin >= t.bound || v.bytes == byte_limit)
                return error(Error::Limit, "string byte bound", a.type);
            v.strings[v.bytes++] = c;
        }
        if(!closed || (!quote && begin == v.bytes))
            return error(Error::Syntax, "missing or unterminated string", a.type);
        if(!utf8(v.strings + begin, v.bytes - begin))
            return error(Error::Utf8, "invalid UTF-8 string", a.type);
        a.begin = begin;
        a.size = v.bytes - begin;
        a.state = 2;
        return true;
    }
    bool value(uint16_t n, unsigned depth)
    {
        if(depth > depth_limit * 2 + 2)
            return error(Error::Limit, "CLI nesting limit");
        auto& a = v.nodes[n];
        const auto& t = m.types[a.type];
        if(a.state)
            return error(Error::Conflict, "duplicate or overlapping assignment", a.type);
        if(token_is("null", 4))
        {
            if(t.kind != Kind::Optional)
                return error(Error::Type, "null requires optional type", a.type);
            pos += 4;
            a.number = 0;
            a.state = 2;
            return true;
        }
        if(t.kind == Kind::Optional)
        {
            a.number = 1;
            a.state = 2;
            a.first = add(v, t.element);
            if(a.first == no_index)
                return error(Error::Limit, "decoded node limit", a.type);
            return value(a.first, depth + 1);
        }
        if(t.kind == Kind::String)
            return string(n);
        if(t.kind == Kind::Record)
        {
            if(pos == length || input[pos++] != '{')
                return error(Error::Type, "record requires braces", a.type);
            a.state = 1;
            skip();
            if(pos < length && input[pos] == '}')
            {
                ++pos;
                a.state = 2;
                return true;
            }
            for(;;)
            {
                unsigned old = 0;
                while(d.path[old])
                    ++old;
                if(!assignment(n, depth + 1))
                    return false;
                d.path[old] = 0;
                skip();
                if(pos == length)
                    return error(Error::Syntax, "unterminated record", a.type);
                char c = input[pos++];
                if(c == '}')
                    break;
                if(c != ',')
                    return error(Error::Syntax, "expected comma", a.type);
                skip();
            }
            a.state = 2;
            return true;
        }
        if(t.kind == Kind::List)
        {
            if(pos == length || input[pos++] != '[')
                return error(Error::Type, "list requires brackets", a.type);
            a.state = 2;
            skip();
            uint16_t last = no_index;
            unsigned count = 0;
            if(pos < length && input[pos] == ']')
            {
                ++pos;
                return true;
            }
            for(;;)
            {
                if(count == t.bound)
                    return error(Error::Limit, "list element bound", a.type);
                auto c = add(v, t.element, count++);
                if(c == no_index)
                    return error(Error::Limit, "decoded node limit", a.type);
                if(last == no_index)
                    a.first = c;
                else
                    v.nodes[last].next = c;
                last = c;
                unsigned old = 0;
                while(d.path[old])
                    ++old;
                if(!list_value(c, depth + 1))
                    return false;
                d.path[old] = 0;
                skip();
                if(pos == length)
                    return error(Error::Syntax, "unterminated list", a.type);
                char end = input[pos++];
                if(end == ']')
                    return true;
                if(end != ',')
                    return error(Error::Syntax, "expected comma", a.type);
                skip();
            }
        }
        if(t.kind == Kind::Variant)
            return error(Error::Type, "variant requires dot case selection", a.type);
        unsigned start = pos;
        while(pos < length && !boundary(input[pos]))
            ++pos;
        unsigned len = pos - start;
        if(!len)
            return error(Error::Syntax, "missing scalar value", a.type);
        if(t.kind == Kind::Enum)
        {
            auto ei = entry_name(m, t, input + start, len);
            if(ei == no_index)
                return error(Error::UnknownName, "unknown enum name", a.type);
            a.number = m.entries[ei].number;
        }
        else if(t.kind == Kind::Boolean)
        {
            if(len == 4 && input[start] == 't' && input[start + 1] == 'r' &&
               input[start + 2] == 'u' && input[start + 3] == 'e')
                a.number = 1;
            else if(len == 5 && input[start] == 'f' && input[start + 1] == 'a' &&
                    input[start + 2] == 'l' && input[start + 3] == 's' && input[start + 4] == 'e')
                a.number = 0;
            else
                return error(Error::Type, "expected true or false", a.type);
        }
        else
        {
            bool neg = input[start] == '-';
            unsigned i = start + (neg ? 1 : 0);
            if(i == pos || (neg && t.kind != Kind::Signed))
                return error(Error::Type, "invalid integer sign", a.type);
            uint64_t number = 0, max = t.kind == Kind::Signed
                                           ? (neg ? uint64_t{1} << 63 : INT64_MAX)
                                           : UINT64_MAX;
            for(; i < pos; ++i)
            {
                char c = input[i];
                if(c < '0' || c > '9')
                    return error(Error::Type, "invalid integer digit", a.type);
                unsigned digit = c - '0';
                if(number > (max - digit) / 10)
                    return error(Error::Range, "integer overflow", a.type);
                number = number * 10 + digit;
            }
            a.number = neg ? uint64_t{0} - number : number;
            if(!scalar_valid(t, a.number))
                return error(Error::Range, "integer outside declared range", a.type);
        }
        a.state = 2;
        return true;
    }
    bool list_value(uint16_t n, unsigned depth)
    {
        if(depth > depth_limit * 2 + 2)
            return error(Error::Limit, "CLI nesting limit");
        auto& a = v.nodes[n];
        const auto& t = m.types[a.type];
        if(t.kind == Kind::Optional && !token_is("null", 4))
        {
            a.number = 1;
            a.state = 2;
            a.first = add(v, t.element);
            if(a.first == no_index)
                return error(Error::Limit, "decoded node limit", a.type);
            return list_value(a.first, depth + 1);
        }
        if(t.kind == Kind::Variant)
        {
            if(pos == length || input[pos++] != '.')
                return error(Error::Type, "list variant requires dot case", a.type);
            return assignment(n, depth + 1);
        }
        return value(n, depth);
    }
    bool run()
    {
        v.nodes[0].state = 1;
        skip();
        while(pos < length)
        {
            d.path[0] = 0;
            if(!assignment(0, 0))
                return false;
            if(pos < length && !space(input[pos]))
                return error(Error::Syntax, "assignments require whitespace");
            skip();
        }
        d.path[0] = 0;
        d.begin = d.end = length;
        return normalize_node(m, v, 0, 0, d);
    }
};
struct Printer
{
    const Metadata& m;
    const Value& v;
    char* output;
    unsigned size{};
    bool declaration = false;
    void ch(char c)
    {
        if(output)
            output[size] = c;
        ++size;
    }
    void text(Text t)
    {
        for(unsigned i = 0; i < t.size; ++i)
            ch(m.strings[t.offset + i]);
    }
    void literal(const char* s)
    {
        while(*s)
            ch(*s++);
    }
    void number(uint64_t n)
    {
        char b[20];
        unsigned len = 0;
        do
        {
            b[len++] = char('0' + n % 10);
            n /= 10;
        } while(n);
        while(len)
            ch(b[--len]);
    }
    void named(uint16_t n)
    {
        const auto& a = v.nodes[n];
        const auto& t = m.types[a.type];
        if(t.kind == Kind::Optional && a.number)
        {
            named(a.first);
            return;
        }
        if(t.kind == Kind::Enum)
        {
            ch('.');
            text(m.entries[entry_number(m, t, a.number)].text.name);
            return;
        }
        if(t.kind == Kind::Variant)
        {
            ch('.');
            const auto& e = m.entries[entry_id(m, t, a.number)];
            text(e.text.name);
            if(e.type != no_index)
                named(a.first);
            return;
        }
        ch(':');
        value(n);
    }
    void value(uint16_t n)
    {
        const auto& a = v.nodes[n];
        const auto& t = m.types[a.type];
        switch(t.kind)
        {
            case Kind::Unsigned:
                number(a.number);
                break;
            case Kind::Signed:
                if(signed_value(a.number) < 0)
                {
                    ch('-');
                    number(uint64_t{0} - a.number);
                }
                else
                    number(a.number);
                break;
            case Kind::Boolean:
                literal(a.number ? "true" : "false");
                break;
            case Kind::Enum:
                if(declaration)
                {
                    text(t.text.name);
                    ch('.');
                }
                text(m.entries[entry_number(m, t, a.number)].text.name);
                break;
            case Kind::Optional:
                if(a.number)
                    value(a.first);
                else
                    literal("null");
                break;
            case Kind::String:
                ch('"');
                for(unsigned i = 0; i < a.size; ++i)
                {
                    char c = v.strings[a.begin + i];
                    if(c == '"' || c == '\\')
                    {
                        ch('\\');
                        ch(c);
                    }
                    else if(c == '\n')
                        literal("\\n");
                    else if(c == '\r')
                        literal("\\r");
                    else if(c == '\t')
                        literal("\\t");
                    else if(declaration && (static_cast<unsigned char>(c) < 32 || c == 127))
                    {
                        constexpr char hex[] = "0123456789abcdef";
                        literal("\\x");
                        ch(hex[static_cast<unsigned char>(c) >> 4]);
                        ch(hex[c & 15]);
                    }
                    else
                        ch(c);
                }
                ch('"');
                break;
            case Kind::Record:
                ch('{');
                record(n, ',');
                ch('}');
                break;
            case Kind::Variant:
                if(declaration)
                {
                    text(t.text.name);
                    ch('.');
                    const auto& e = m.entries[entry_id(m, t, a.number)];
                    text(e.text.name);
                    if(e.type != no_index)
                    {
                        ch('(');
                        value(a.first);
                        ch(')');
                    }
                }
                else
                    named(n);
                break;
            case Kind::List:
                ch('[');
                for(auto c = a.first; c != no_index; c = v.nodes[c].next)
                {
                    if(c != a.first)
                        ch(',');
                    value(c);
                }
                ch(']');
                break;
        }
    }
    void record(uint16_t n, char separator)
    {
        const auto& a = v.nodes[n];
        const auto& t = m.types[a.type];
        auto c = a.first;
        for(unsigned i = 0; i < t.count; ++i)
        {
            if(i)
                ch(separator);
            text(m.entries[t.first + i].text.name);
            if(declaration)
            {
                ch(':');
                value(c);
            }
            else
                named(c);
            c = v.nodes[c].next;
        }
    }
};
}  // namespace detail
inline bool parse(const Metadata& m, const char* input, unsigned length, Value& out, Diagnostic& d)
{
    if(!validate(m, d))
        return false;
    if(length > 4096 || (!input && length))
        return fail(d, Error::Limit, "CLI input limit");
    Value v{};
    initialize(v, m.root);
    detail::Parser p{m, input, length, 0, v, d};
    if(!p.run())
        return false;
    Wire wire{};
    if(!detail::encode_node(m, v, 0, wire, 0, d))
        return false;
    out = v;
    d = {};
    return true;
}
inline bool print(const Metadata& m,
                  const Value& value,
                  char* output,
                  unsigned capacity,
                  unsigned& written,
                  Diagnostic& d)
{
    Wire check{};
    if(!encode(m, value, check, d))
        return false;
    detail::Printer count{m, value, nullptr};
    count.record(0, ' ');
    if(!output || count.size >= capacity)
        return fail(d, Error::Limit, "printer output capacity");
    detail::Printer printer{m, value, output};
    printer.record(0, ' ');
    output[printer.size] = 0;
    written = printer.size;
    return true;
}
// Documentation is not part of wire compatibility. This semantic digest covers
// type/field names and normalized wire defaults, but deliberately excludes prose,
// unit and format hints. It is a compatibility aid, not authentication.
inline uint64_t semantic_fingerprint(const Metadata& m)
{
    if(!validate(m))
        return 0;
    uint64_t hash = 14695981039346656037ull;
    auto number = [&](uint64_t n) {
        for(unsigned i = 0; i < 8; ++i)
        {
            hash ^= static_cast<uint8_t>(n >> (8 * i));
            hash *= 1099511628211ull;
        }
    };
    auto name = [&](Text s) {
        number(s.size);
        for(unsigned i = 0; i < s.size; ++i)
        {
            hash ^= static_cast<uint8_t>(m.strings[s.offset + i]);
            hash *= 1099511628211ull;
        }
    };
    number(m.schema_id);
    number(m.schema_version);
    number(m.root);
    number(m.type_count);
    number(m.entry_count);
    for(unsigned i = 0; i < m.type_count; ++i)
    {
        const auto& t = m.types[i];
        number(t.id);
        number(static_cast<unsigned>(t.kind));
        number(t.width);
        number(t.first);
        number(t.count);
        number(t.element);
        number(t.bound);
        number(t.minimum);
        number(t.maximum);
        name(t.text.name);
    }
    for(unsigned i = 0; i < m.entry_count; ++i)
    {
        const auto& e = m.entries[i];
        number(e.id);
        number(e.type);
        number(e.number);
        name(e.text.name);
        number(e.default_size);
        for(unsigned j = 0; j < e.default_size; ++j)
            number(m.defaults[e.default_offset + j]);
    }
    return hash;
}
}  // namespace os1::parameters
