#pragma once
#include "interface/metadata.hpp"
namespace os1::buffer_metadata
{
// A bounded attachment profile. This does not extend the generic type grammar.
constexpr uint64_t magic = 0x3146554231534full;  // OS1BUF1
constexpr uint32_t exclusive = 1, return_input = 1, distinct_output = 2;
struct Member
{
    uint32_t member, mode, input_count, output;
};
struct Metadata
{
    uint64_t magic;
    uint32_t version, size;
    uint64_t interface_id;
    uint32_t interface_version, count;
    Member members[8];
};
static_assert(sizeof(Metadata) == 160);
inline bool reference(const interface::Metadata& api, uint32_t type)
{
    if(!type || type > api.type_count)
        return false;
    const auto& t = api.types[type - 1];
    if(t.kind != interface::Record || t.size != 24 || t.field_count != 4)
        return false;
    constexpr unsigned kinds[] = {interface::U32, interface::U32, interface::U64, interface::U64};
    constexpr unsigned offsets[] = {0, 4, 8, 16};
    for(unsigned i = 0; i < 4; ++i)
    {
        const auto& f = api.fields[t.first_field + i];
        if(f.kind != kinds[i] || f.offset != offsets[i] || f.count != 1)
            return false;
    }
    return true;
}
inline bool validate(const Metadata& m, const interface::Metadata& api)
{
    if(!interface::validate(api) || m.magic != magic || m.version != 1 || m.size != sizeof(m) ||
       m.interface_id != api.interface_id || m.interface_version != api.interface_version ||
       !m.count || m.count > 8)
        return false;
    uint32_t last = 0;
    for(unsigned i = 0; i < m.count; ++i)
    {
        const auto& a = m.members[i];
        if(a.member <= last || a.mode != exclusive || a.input_count != 1 ||
           (a.output != return_input && a.output != distinct_output))
            return false;
        bool found = false;
        for(unsigned j = 0; j < api.member_count; ++j)
        {
            const auto& method = api.members[j];
            if(method.id == a.member && method.kind == interface::Method && method.flags &&
               reference(api, method.input) && reference(api, method.output))
                found = true;
        }
        if(!found)
            return false;
        last = a.member;
    }
    return interface::zero(m.members + m.count, sizeof(Member) * (8 - m.count));
}
}  // namespace os1::buffer_metadata
