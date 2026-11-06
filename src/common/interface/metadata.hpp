#pragma once
#include <stddef.h>
#include <stdint.h>

#include "elf/elf64.hpp"
namespace os1::interface
{
// Little-endian v1. No native pointers, strings, recursive types, or padding bytes.
constexpr uint64_t magic = 0x3146544e4931534full;  // OS1INTF1
constexpr uint32_t max_types = 8, max_fields = 16, max_members = 8;
enum Kind : uint32_t
{
    U8 = 1,
    U32 = 2,
    U64 = 3,
    Record = 4
};
enum MemberKind : uint32_t
{
    Method = 1,
    PropertyGet = 2,
    PropertySet = 3,
    Event = 4
};
constexpr uint32_t implemented = 1;
struct Type
{
    uint32_t kind, size, first_field, field_count;
    char name[32];
};
struct Field
{
    uint32_t kind, offset, count, reserved;
    char name[32];
};
struct Member
{
    uint32_t id, kind, flags, input, output, reserved;
    char name[32];
};
struct Metadata
{
    uint64_t magic;
    uint32_t version, size;
    uint64_t interface_id;
    uint32_t interface_version, type_count, field_count, member_count;
    char class_name[32], interface_name[32];
    Type types[max_types];
    Field fields[max_fields];
    Member members[max_members];
};
static_assert(sizeof(Metadata) == 1704);
inline bool zero(const void* data, size_t size)
{
    auto* b = static_cast<const uint8_t*>(data);
    for(size_t i = 0; i < size; ++i)
        if(b[i])
            return false;
    return true;
}
inline bool name_valid(const char (&name)[32])
{
    if(!name[0])
        return false;
    for(unsigned i = 0; i < 32; ++i)
    {
        if(!name[i])
            return zero(name + i, 32 - i);
        const char c = name[i];
        if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
             c == '_'))
            return false;
    }
    return false;
}
// Public method/property/event names use snake_case. Types and interfaces may
// use UpperCamelCase; their existing bounded name validator remains unchanged.
inline bool member_name_valid(const char (&name)[32])
{
    if(!name_valid(name) || name[0] < 'a' || name[0] > 'z')
        return false;
    for(unsigned i = 1; name[i]; ++i)
    {
        const char c = name[i];
        if(!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
        if(c == '_' && (name[i - 1] == '_' || !name[i + 1]))
            return false;
    }
    return true;
}
inline uint32_t scalar_size(uint32_t kind)
{
    return kind == U8 ? 1 : kind == U32 ? 4 : kind == U64 ? 8 : 0;
}
inline bool validate(const Metadata& m)
{
    if(m.magic != magic || m.version != 1 || m.size != sizeof(m) || !m.interface_id ||
       !m.interface_version || !name_valid(m.class_name) || !name_valid(m.interface_name) ||
       !m.type_count || m.type_count > max_types || m.field_count > max_fields || !m.member_count ||
       m.member_count > max_members)
        return false;
    uint32_t next_field = 0;
    for(unsigned i = 0; i < m.type_count; ++i)
    {
        const auto& t = m.types[i];
        if(!name_valid(t.name) || !t.size || t.size > 256)
            return false;
        if(t.kind != Record)
        {
            if(t.size != scalar_size(t.kind) || t.field_count || t.first_field)
                return false;
            continue;
        }
        if(!t.field_count || t.first_field != next_field ||
           t.field_count > m.field_count - next_field)
            return false;
        uint32_t offset = 0;
        for(unsigned j = 0; j < t.field_count; ++j)
        {
            const auto& f = m.fields[next_field++];
            const auto size = scalar_size(f.kind);
            if(!name_valid(f.name) || f.reserved || !size || !f.count || f.count > 256 ||
               f.offset != offset)
                return false;
            offset += size * f.count;
            if(offset > t.size)
                return false;
        }
        if(offset != t.size)
            return false;
    }
    if(next_field != m.field_count)
        return false;
    uint32_t last_id = 0;
    for(unsigned i = 0; i < m.member_count; ++i)
    {
        const auto& a = m.members[i];
        if(a.id <= last_id || !member_name_valid(a.name) || a.kind < Method || a.kind > Event ||
           a.flags > implemented || a.input > m.type_count || a.output > m.type_count || a.reserved)
            return false;
        if(a.kind == PropertyGet && a.input)
            return false;
        if(a.kind == Event && a.output)
            return false;
        last_id = a.id;
    }
    return zero(m.types + m.type_count, sizeof(Type) * (max_types - m.type_count)) &&
           zero(m.fields + m.field_count, sizeof(Field) * (max_fields - m.field_count)) &&
           zero(m.members + m.member_count, sizeof(Member) * (max_members - m.member_count));
}
struct Section
{
    uint32_t name, type;
    uint64_t flags, address, offset, size;
    uint32_t link, info;
    uint64_t alignment, entry_size;
};
static_assert(sizeof(Section) == 64);
// Reader must read exactly size bytes at the requested offset or return false.
// Inspection does not load/execute the image or trust its virtual addresses.
template<class Reader, class RecordType, class Validator>
int read_section(
    Reader read, uint64_t file_size, const char* wanted, RecordType& output, Validator valid)
{
    auto range = [&](uint64_t offset, uint64_t size) {
        return offset <= file_size && size <= file_size - offset;
    };
    elf::Elf64Header h{};
    if(!range(0, sizeof(h)) || !read(0, &h, sizeof(h)) || h.magic != elf::kMagic ||
       h.ident[0] != 2 || h.ident[1] != 1 || h.ident[2] != 1 || h.type != elf::kTypeExec ||
       h.machine != elf::kMachineX86_64 || h.version != 1 || h.ehsize != sizeof(h))
        return -1;
    // ELF section tables are optional for execution. A compact non-reflective
    // executable has no metadata; partially absent tables remain malformed.
    if(!h.shoff && !h.shnum && !h.shstrndx && !h.shentsize)
        return 0;
    if(h.shentsize != sizeof(Section) || !h.shnum || h.shnum > 128 || h.shstrndx >= h.shnum ||
       !range(h.shoff, uint64_t(h.shnum) * sizeof(Section)))
        return -1;
    Section strings{};
    if(!read(h.shoff + uint64_t(h.shstrndx) * sizeof(Section), &strings, sizeof(strings)) ||
       strings.type != 3 || !strings.size || strings.size > 4096 ||
       !range(strings.offset, strings.size))
        return -1;
    char names[4096]{};
    if(!read(strings.offset, names, strings.size))
        return -1;
    bool found = false;
    for(unsigned i = 0; i < h.shnum; ++i)
    {
        Section s{};
        if(!read(h.shoff + uint64_t(i) * sizeof(s), &s, sizeof(s)) || s.name >= strings.size)
            return -1;
        uint64_t length = 0;
        while(s.name + length < strings.size && names[s.name + length])
            ++length;
        if(s.name + length == strings.size)
            return -1;
        size_t wanted_length = 0;
        while(wanted[wanted_length])
            ++wanted_length;
        if(length != wanted_length)
            continue;
        bool match = true;
        for(unsigned j = 0; j < length; ++j)
            if(names[s.name + j] != wanted[j])
                match = false;
        if(!match)
            continue;
        if(found || s.type != 1 || s.flags != 2 || s.size != sizeof(output) ||
           !range(s.offset, s.size))
            return -1;
        if(!read(s.offset, &output, sizeof(output)) || !valid(output))
            return -1;
        found = true;
    }
    return found ? 1 : 0;
}
template<class Reader>
bool read_elf(Reader read, uint64_t size, Metadata& output)
{
    return read_section(read, size, ".os.interface", output, [](const Metadata& m) {
               return validate(m);
           }) == 1;
}
}  // namespace os1::interface
