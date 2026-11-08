#include "core/wall_clock.hpp"

namespace
{
volatile uint64_t clock_ns = 0;
bool leap(unsigned year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}
bool number(uint8_t raw, bool binary, unsigned& result)
{
    if(!binary && ((raw & 15) > 9 || (raw >> 4) > 9))
        return false;
    result = binary ? raw : (raw >> 4) * 10 + (raw & 15);
    return true;
}
}  // namespace
bool rtc_decode(const RtcCalendar& raw, bool has_century, uint64_t& unix_ns)
{
    unix_ns = 0;
    if(!(raw.status & 0x80) || (raw.mode & 0x80))
        return false;
    unsigned second, minute, hour, day, month, year, century = 20;
    const bool binary = raw.mode & 4, full_day = raw.mode & 2;
    if(!number(raw.second, binary, second) || !number(raw.minute, binary, minute) ||
       !number(full_day ? raw.hour : raw.hour & 0x7f, binary, hour) ||
       !number(raw.day, binary, day) || !number(raw.month, binary, month) ||
       !number(raw.year, binary, year) || (has_century && !number(raw.century, binary, century)))
        return false;
    if(second > 59 || minute > 59 || year > 99 || century < 19 || century > 21 || month < 1 ||
       month > 12 || (full_day ? hour > 23 : hour < 1 || hour > 12))
        return false;
    if(!full_day)
        hour = hour % 12 + ((raw.hour & 0x80) ? 12 : 0);
    year += century * 100;
    if(year < 1970 || year > 2106)
        return false;
    constexpr unsigned days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if(!day || day > days_in_month[month - 1] + (month == 2 && leap(year)))
        return false;
    uint64_t days = 0;
    for(unsigned y = 1970; y < year; ++y)
        days += 365 + leap(y);
    for(unsigned m = 1; m < month; ++m)
        days += days_in_month[m - 1] + (m == 2 && leap(year));
    days += day - 1;
    uint64_t seconds = ((days * 24 + hour) * 60 + minute) * 60 + second;
    if(!seconds || seconds > UINT32_MAX)
        return false;
    unix_ns = seconds * 1000000000ull;
    return true;
}
void wall_clock_initialize(uint64_t unix_ns)
{
    __atomic_store_n(&clock_ns, unix_ns, __ATOMIC_RELEASE);
}
void wall_clock_tick(uint64_t elapsed_ns)
{
    // Only the BSP advances this state; initialization precedes timer start.
    uint64_t now = __atomic_load_n(&clock_ns, __ATOMIC_RELAXED);
    if(now)
        __atomic_store_n(
            &clock_ns, elapsed_ns <= UINT64_MAX - now ? now + elapsed_ns : 0, __ATOMIC_RELEASE);
}
bool wall_clock_now(uint64_t& unix_ns)
{
    unix_ns = __atomic_load_n(&clock_ns, __ATOMIC_ACQUIRE);
    return unix_ns != 0;
}
