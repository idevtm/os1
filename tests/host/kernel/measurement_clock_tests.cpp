#include <gtest/gtest.h>

#include "core/measurement_clock.hpp"

TEST(MeasurementClock, RequiresWideProgressingCounterAndValidPeriod)
{
    MeasurementClock clock;
    uint64_t ns = 1, resolution = 1;
    EXPECT_FALSE(clock.sample(1, ns, resolution));
    EXPECT_EQ(0u, ns);
    EXPECT_EQ(0u, resolution);
    EXPECT_FALSE(clock.initialize(false, 10000000, 0, 1));
    EXPECT_FALSE(clock.initialize(true, 0, 0, 1));
    EXPECT_FALSE(clock.initialize(true, 100000001, 0, 1));
    EXPECT_FALSE(clock.initialize(true, 10000000, 10, 10));
    EXPECT_FALSE(clock.initialize(true, 10000000, 10, 9));
}
TEST(MeasurementClock, ConvertsFractionalPeriodsWithoutEarlyOverflow)
{
    MeasurementClock clock;
    uint64_t ns = 0, resolution = 0;
    ASSERT_TRUE(clock.initialize(true, 1500000, 0, 1));
    ASSERT_TRUE(clock.sample(4, ns, resolution));
    EXPECT_EQ(4u, ns);
    EXPECT_EQ(2u, resolution);
    ASSERT_TRUE(clock.sample(1000000000001ull, ns, resolution));
    EXPECT_EQ(1500000000000ull, ns);
}
TEST(MeasurementClock, CounterRollbackAndConversionOverflowInvalidateClock)
{
    MeasurementClock clock;
    uint64_t ns, resolution;
    ASSERT_TRUE(clock.initialize(true, 100000000, 0, 1));
    ASSERT_TRUE(clock.sample(10, ns, resolution));
    ASSERT_TRUE(clock.sample(10, ns, resolution));
    EXPECT_FALSE(clock.sample(9, ns, resolution));
    EXPECT_FALSE(clock.sample(11, ns, resolution));
    EXPECT_EQ(0u, ns);
    ASSERT_TRUE(clock.initialize(true, 100000000, 0, 1));
    EXPECT_FALSE(clock.sample(UINT64_MAX, ns, resolution));
    EXPECT_FALSE(clock.sample(2, ns, resolution));
}
