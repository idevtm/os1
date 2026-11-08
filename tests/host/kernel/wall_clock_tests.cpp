#include <gtest/gtest.h>

#include "core/wall_clock.hpp"

TEST(WallClock, CalendarModesAndLeapDays)
{
    uint64_t a = 0, b = 0;
    RtcCalendar raw{0x56, 0x34, 0x12, 0x29, 0x02, 0x24, 0x20, 2, 0x80};
    ASSERT_TRUE(rtc_decode(raw, true, a));
    EXPECT_EQ(a, 1709210096000000000ull);
    raw = {56, 34, 12, 29, 2, 24, 20, 6, 0x80};
    ASSERT_TRUE(rtc_decode(raw, true, b));
    EXPECT_EQ(a, b);
    raw.mode = 4;
    raw.hour = 0x80 | 12;
    ASSERT_TRUE(rtc_decode(raw, true, b));
    EXPECT_EQ(a, b);
    raw.hour = 12;
    ASSERT_TRUE(rtc_decode(raw, true, b));
    EXPECT_EQ(a - b, 12ull * 3600 * 1000000000);
    raw.year = 23;
    EXPECT_FALSE(rtc_decode(raw, true, b));
    raw.year = 0;
    raw.century = 21;
    EXPECT_FALSE(rtc_decode(raw, true, b));
}
TEST(WallClock, RejectInvalidOrUnavailableCalendar)
{
    uint64_t now = 123;
    RtcCalendar raw{0, 0, 0, 1, 1, 26, 20, 6, 0x80};
    ASSERT_TRUE(rtc_decode(raw, false, now));
    EXPECT_EQ(now, 1767225600000000000ull);
    raw.status = 0;
    EXPECT_FALSE(rtc_decode(raw, true, now));
    EXPECT_EQ(now, 0u);
    raw.status = 0x80;
    raw.mode |= 0x80;
    EXPECT_FALSE(rtc_decode(raw, true, now));
    raw.mode = 2;
    raw.second = 0x6a;
    EXPECT_FALSE(rtc_decode(raw, true, now));
    raw.second = 0;
    raw.month = 0;
    EXPECT_FALSE(rtc_decode(raw, true, now));
    raw.month = 1;
    raw.day = 0;
    EXPECT_FALSE(rtc_decode(raw, true, now));
    raw.day = 1;
    raw.hour = 0x24;
    EXPECT_FALSE(rtc_decode(raw, true, now));
}
TEST(WallClock, IndependentTickStateAndOverflow)
{
    uint64_t now = 1;
    wall_clock_initialize(0);
    wall_clock_tick(1000000);
    EXPECT_FALSE(wall_clock_now(now));
    wall_clock_initialize(1700000000000000000ull);
    wall_clock_tick(1000000);
    ASSERT_TRUE(wall_clock_now(now));
    EXPECT_EQ(now, 1700000000001000000ull);
    wall_clock_initialize(UINT64_MAX - 2);
    wall_clock_tick(3);
    EXPECT_FALSE(wall_clock_now(now));
    wall_clock_initialize(0);
}
