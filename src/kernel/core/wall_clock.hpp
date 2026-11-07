#pragma once
#include <stdint.h>

// Raw CMOS calendar bytes; mode is register B, status is register D.
struct RtcCalendar
{
    uint8_t second, minute, hour, day, month, year, century, mode, status;
};
bool rtc_decode(const RtcCalendar&, bool has_century, uint64_t& unix_ns);
// Initialization is BSP-only, before the BSP scheduler timer starts.
void wall_clock_initialize(uint64_t unix_ns);
void wall_clock_tick(uint64_t elapsed_ns);
bool wall_clock_now(uint64_t& unix_ns);
