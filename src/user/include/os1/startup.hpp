#pragma once
#include <os1/vfs.hpp>

#include "interface/constructor.hpp"
namespace os1::user
{
struct StartupContext
{
    const os1_startup_record& record;
    const char* environment(const char* name) const
    {
        if(!name || !*name)
            return nullptr;
        for(unsigned i = 0; i < record.envc; ++i)
        {
            const char* entry = record.strings + record.envp[i];
            unsigned j = 0;
            while(name[j] && entry[j] == name[j] && name[j] != '=')
                ++j;
            if(!name[j] && entry[j] == '=')
                return entry + j + 1;
        }
        return nullptr;
    }
};
inline bool constructor_record(const StartupContext& context,
                               const constructor::Metadata& schema,
                               os1_constructor_args& out)
{
    if(!context.record.constructor.schema_id)
        return constructor::defaults(schema, out);
    out = context.record.constructor;
    return constructor::matches(schema, out);
}
}  // namespace os1::user
