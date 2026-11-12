#pragma once
#include <stdint.h>

namespace os1::shell
{
inline constexpr char utc_fields[][11] = {
    "year", "month", "day", "hour", "minute", "second", "nanosecond", "unix_ns"};

struct UtcTime
{
    unsigned year, month, day, hour, minute, second, nanosecond;
};

inline unsigned utc_leap(unsigned year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

inline void utc_digits(char* output, unsigned value, unsigned width)
{
    while(width)
    {
        output[--width] = char('0' + value % 10);
        value /= 10;
    }
}

inline UtcTime utc_time(uint64_t unix_ns)
{
    constexpr unsigned seconds_per_day = 86400;
    const uint64_t seconds = unix_ns / 1000000000ull;
    const unsigned day_seconds = seconds % seconds_per_day;
    unsigned days = seconds / seconds_per_day;
    unsigned year = 1970;
    // A uint64_t nanosecond timestamp spans fewer than 585 calendar years.
    for(; days >= 365 + utc_leap(year); ++year)
        days -= 365 + utc_leap(year);
    constexpr unsigned month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned month = 0;
    for(; month < 11; ++month)
    {
        unsigned count = month_days[month] + (month == 1 && utc_leap(year));
        if(days < count)
            break;
        days -= count;
    }

    return {year,
            month + 1,
            days + 1,
            day_seconds / 3600,
            day_seconds / 60 % 60,
            day_seconds % 60,
            static_cast<unsigned>(unix_ns % 1000000000ull)};
}

inline void format_utc(char* time, const UtcTime& utc, bool iso = false)
{
    utc_digits(time, utc.year, 4);
    time[4] = time[7] = '-';
    utc_digits(time + 5, utc.month, 2);
    utc_digits(time + 8, utc.day, 2);
    utc_digits(time + 11, utc.hour, 2);
    time[13] = time[16] = ':';
    utc_digits(time + 14, utc.minute, 2);
    utc_digits(time + 17, utc.second, 2);
    time[10] = iso ? 'T' : ' ';
}
}  // namespace os1::shell
