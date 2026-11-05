#pragma once
#include "interface/parameters.hpp"
namespace os1::parameters
{
// v1 explicitly maps one rich schema to a constructor and named interface members.
// Method IDs refer to .os.interface; they are never inferred from argv or names.
struct Binding
{
    uint64_t magic;
    uint32_t version, size;
    uint64_t schema_id;
    uint32_t schema_version, constructor;
    uint64_t interface_id;
    uint32_t interface_version, member_count;
    uint32_t members[8];
};
constexpr uint64_t binding_magic = 0x31444e425031534full;
static_assert(sizeof(Binding) == 80);
inline bool validate_binding(const Binding& b, const Metadata& schema)
{
    if(b.magic != binding_magic || b.version != 1 || b.size != sizeof(b) ||
       b.schema_id != schema.schema_id || b.schema_version != schema.schema_version ||
       b.constructor > 1 || !b.interface_id || !b.interface_version || b.member_count > 8 ||
       (!b.constructor && !b.member_count))
        return false;
    for(unsigned i = 0; i < 8; ++i)
    {
        if(i >= b.member_count)
        {
            if(b.members[i])
                return false;
            continue;
        }
        if(!b.members[i])
            return false;
        for(unsigned j = 0; j < i; ++j)
            if(b.members[j] == b.members[i])
                return false;
    }
    return true;
}
inline bool binds(const Binding& b, const interface::Metadata& api, uint32_t member)
{
    if(b.interface_id != api.interface_id || b.interface_version != api.interface_version)
        return false;
    for(unsigned i = 0; i < b.member_count; ++i)
        if(b.members[i] == member)
        {
            for(unsigned j = 0; j < api.member_count; ++j)
                if(api.members[j].id == member && api.members[j].kind == interface::Method &&
                   api.members[j].flags)
                    return true;
        }
    return false;
}
}  // namespace os1::parameters
