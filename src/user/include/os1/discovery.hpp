#pragma once
#include <os1/vfs.hpp>

#include "discovery/view.hpp"
namespace os1::user::discovery
{
inline bool refresh(os1::discovery::View& view, bool shell = false)
{
    // QUERY and FILE_INFO require INSPECT. Do not infer hidden grant authority.
    os1::discovery::Grants grants{};
    if(query(root(), &grants.root) < 0)
        grants.root = {};
    if(query(cwd(), &grants.cwd) < 0)
        grants.cwd = {};
    for(unsigned i = 0; i < 3; ++i)
        if(query(i, &grants.streams[i]) < 0)
            grants.streams[i] = {};
    os1_file_info info{};
    if(file_info(root(), &info) >= 0)
        grants.ceiling = info.descendant_rights;
    grants.shell = shell;
    // Refresh invalidates prior cursors, even if grants and data happen to match.
    uint64_t next = view.generation + 1;
    return next && view.build(grants, environment_data, next);
}
}  // namespace os1::user::discovery
