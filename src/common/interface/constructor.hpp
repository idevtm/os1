#pragma once
#include <os1/startup.h>

#include "interface/metadata.hpp"
namespace os1::constructor
{
constexpr uint64_t magic = 0x3152544331534full;  // OS1CTR1, padded with zero
constexpr unsigned field_limit = 8;
struct Field
{
    uint32_t offset, kind;
    uint64_t default_value;
    char name[32], option[32];
};
struct Metadata
{
    uint64_t magic;
    uint32_t version, size;
    uint64_t schema_id;
    uint32_t schema_version, payload_bytes, field_count, reserved;
    Field fields[field_limit];
};
static_assert(sizeof(Metadata) == 680);
inline bool equal(const char* a, const char* b)
{
    while(*a && *a == *b)
    {
        ++a;
        ++b;
    }
    return *a == *b;
}
inline uint64_t maximum(unsigned size)
{
    return size == 8 ? UINT64_MAX : (uint64_t{1} << (size * 8)) - 1;
}
inline bool validate(const Metadata& m)
{
    if(m.magic != magic || m.version != 1 || m.size != sizeof(m) || !m.schema_id ||
       !m.schema_version || !m.field_count || m.field_count > field_limit ||
       m.payload_bytes > OS1_CONSTRUCTOR_BYTES || m.reserved)
        return false;
    unsigned offset = 0;
    for(unsigned i = 0; i < m.field_count; ++i)
    {
        const auto& f = m.fields[i];
        auto size = interface::scalar_size(f.kind);
        if(!size || f.offset != offset || !interface::name_valid(f.name) ||
           !interface::name_valid(f.option) || f.default_value > maximum(size))
            return false;
        for(unsigned j = 0; j < i; ++j)
            if(equal(f.name, m.fields[j].name) || equal(f.option, m.fields[j].option))
                return false;
        offset += size;
    }
    return offset == m.payload_bytes &&
           interface::zero(m.fields + m.field_count, sizeof(Field) * (field_limit - m.field_count));
}
inline void encode(uint8_t* out, unsigned size, uint64_t value)
{
    for(unsigned i = 0; i < size; ++i)
        out[i] = static_cast<uint8_t>(value >> (8 * i));
}
inline uint64_t decode(const uint8_t* in, unsigned size)
{
    uint64_t value = 0;
    for(unsigned i = 0; i < size; ++i)
        value |= uint64_t{in[i]} << (8 * i);
    return value;
}
inline bool defaults(const Metadata& m, os1_constructor_args& args)
{
    if(!validate(m))
        return false;
    args = {};
    args.schema_id = m.schema_id;
    args.version = m.schema_version;
    args.size = m.payload_bytes;
    for(unsigned i = 0; i < m.field_count; ++i)
    {
        const auto& f = m.fields[i];
        encode(args.payload + f.offset, interface::scalar_size(f.kind), f.default_value);
    }
    return true;
}
// Options are --name value, decimal unsigned integers, no duplicate/unknown options.
inline bool parse(const Metadata& m,
                  unsigned argc,
                  const char* const* argv,
                  os1_constructor_args& args)
{
    os1_constructor_args prepared{};
    if(!defaults(m, prepared) || argc % 2 || (argc && !argv))
        return false;
    uint32_t seen = 0;
    for(unsigned i = 0; i < argc; i += 2)
    {
        const char* option = argv[i];
        const char* value = argv[i + 1];
        if(!option || option[0] != '-' || option[1] != '-' || !value || !*value)
            return false;
        unsigned field = 0;
        while(field < m.field_count && !equal(option + 2, m.fields[field].option))
            ++field;
        if(field == m.field_count || (seen & (1u << field)))
            return false;
        seen |= 1u << field;
        const auto& f = m.fields[field];
        auto size = interface::scalar_size(f.kind);
        uint64_t number = 0;
        for(unsigned j = 0; value[j]; ++j)
        {
            if(value[j] < '0' || value[j] > '9')
                return false;
            unsigned digit = value[j] - '0';
            if(number > (maximum(size) - digit) / 10)
                return false;
            number = number * 10 + digit;
        }
        encode(prepared.payload + f.offset, size, number);
    }
    args = prepared;
    return true;
}
inline bool matches(const Metadata& m, const os1_constructor_args& args)
{
    return validate(m) && args.schema_id == m.schema_id && args.version == m.schema_version &&
           args.size == m.payload_bytes &&
           interface::zero(args.payload + args.size, OS1_CONSTRUCTOR_BYTES - args.size);
}
}  // namespace os1::constructor
