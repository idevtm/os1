#pragma once
#include <os1/observe.h>
#include <os1/resource.h>

#include <os1/syscall.hpp>

#include "shell/utc.hpp"

namespace os1::user::clock
{
inline long now(uint64_t& unix_ns)
{
    Os1ObserveWallClock result{};
    const long status = observe(OS1_OBSERVE_WALL_CLOCK, &result, sizeof(result));
    if(status != static_cast<long>(sizeof(result)))
        return status < 0 ? status : OS1_ERR_IO;
    unix_ns = result.unix_ns;
    return 0;
}

inline void format(char (&text)[24], uint64_t unix_ns, bool iso)
{
    shell::format_utc(text, shell::utc_time(unix_ns), iso);
    text[19] = iso ? 'Z' : ' ';
    text[20] = iso ? 0 : 'U';
    text[21] = 'T';
    text[22] = 'C';
    text[23] = 0;
}
}  // namespace os1::user::clock
