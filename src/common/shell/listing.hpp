#pragma once
#include <stdint.h>

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

        constexpr unsigned seconds_per_day = 86400;
        const uint64_t seconds = modified_ns / 1000000000ull;
        const unsigned day_seconds = seconds % seconds_per_day;
        unsigned days = seconds / seconds_per_day;
        unsigned year = 1970;
        // A uint64_t nanosecond timestamp spans fewer than 585 calendar years.
        for(; days >= 365 + leap(year); ++year)
            days -= 365 + leap(year);
        constexpr unsigned month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        unsigned month = 0;
        for(; month < 11; ++month)
        {
            unsigned count = month_days[month] + (month == 1 && leap(year));
            if(days < count)
                break;
            days -= count;
        }

        digits(time, year, 4);
        time[4] = time[7] = '-';
        digits(time + 5, month + 1, 2);
        digits(time + 8, days + 1, 2);
        digits(time + 11, day_seconds / 3600, 2);
        time[13] = time[16] = ':';
        digits(time + 14, day_seconds / 60 % 60, 2);
        digits(time + 17, day_seconds % 60, 2);
    }

private:
    static unsigned leap(unsigned year)
    {
        return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    }

    static void digits(char* output, unsigned value, unsigned width)
    {
        while(width)
        {
            output[--width] = char('0' + value % 10);
            value /= 10;
        }
    }
};
}  // namespace os1::shell
