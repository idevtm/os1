#pragma once
#include "shell/utc.hpp"

namespace os1::shell
{
inline constexpr char long_listing_header[] =
    "TYPE                 BYTES  MODIFIED (UTC)       NAME\n";

struct LongListingRow
{
    static constexpr unsigned bytes_end = 26, time_begin = 28, time_width = 19;
    char prefix[time_begin + time_width + 2];

    LongListingRow(bool directory, uint64_t bytes, bool has_modified, uint64_t modified_ns)
    {
        for(auto& c : prefix)
            c = ' ';
        prefix[0] = directory ? 'd' : '-';
        unsigned end = bytes_end;
        do
        {
            prefix[--end] = char('0' + bytes % 10);
            bytes /= 10;
        } while(bytes);
        char* time = prefix + time_begin;
        if(!has_modified)
        {
            time[0] = '-';
            return;
        }

        format_utc(time, utc_time(modified_ns));
    }
};
}  // namespace os1::shell
