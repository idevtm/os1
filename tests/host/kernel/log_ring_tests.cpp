#include <gtest/gtest.h>

#include <array>

#include "log/ring.hpp"
TEST(LogRing, OrdinaryTrafficCannotOverwriteReservedErrors)
{
    std::array<os1_log_record, 16> storage{};
    os1::log::Ring ring;
    ring.initialize(storage.data(), storage.size());
    os1_log_record error{};
    error.level = OS1_LOG_ERROR;
    ASSERT_TRUE(ring.append(error));
    os1_log_record ordinary{};
    ordinary.level = OS1_LOG_INFO_LEVEL;
    for(unsigned i = 0; i < 100; ++i)
        ASSERT_TRUE(ring.append(ordinary));
    os1_log_record read{};
    ASSERT_TRUE(ring.next(0, ring.latest(), read));
    EXPECT_EQ(1u, read.sequence);
    EXPECT_EQ(OS1_LOG_ERROR, read.level);
    EXPECT_EQ(86u, ring.overwritten());
    ASSERT_TRUE(ring.next(read.sequence, ring.latest(), read));
    EXPECT_EQ(88u, read.sequence);
}
TEST(LogRing, MergesPartitionsAndHonorsSnapshotBoundary)
{
    std::array<os1_log_record, 16> storage{};
    os1::log::Ring ring;
    ring.initialize(storage.data(), storage.size());
    for(unsigned i = 1; i <= 8; ++i)
    {
        os1_log_record r{};
        r.level = i == 3 || i == 7 ? OS1_LOG_ERROR : OS1_LOG_INFO_LEVEL;
        ring.append(r);
    }
    os1_log_record out{};
    for(unsigned i = 1; i <= 6; ++i)
    {
        ASSERT_TRUE(ring.next(i - 1, 6, out));
        EXPECT_EQ(i, out.sequence);
    }
    EXPECT_FALSE(ring.next(6, 6, out));
}
TEST(LogRing, BoundedQueryValidationAndFilters)
{
    os1_log_query q{};
    q.version = 1;
    q.size = sizeof(q);
    q.pid = 7;
    q.command_id = 42;
    q.min_level = OS1_LOG_WARNING;
    q.kind = OS1_LOG_IPC_END;
    os1_log_record r{};
    r.pid = 7;
    r.command_id = 42;
    r.level = OS1_LOG_WARNING;
    r.kind = OS1_LOG_IPC_END;
    EXPECT_TRUE(os1::log::valid(q));
    EXPECT_TRUE(os1::log::matches(r, q));
    r.pid = 8;
    EXPECT_FALSE(os1::log::matches(r, q));
    q.through = 2;
    q.after = 3;
    EXPECT_FALSE(os1::log::valid(q));
    q.after = 0;
    q.kind = 999;
    EXPECT_FALSE(os1::log::valid(q));
}
TEST(LogRing, EmptyOrInvalidStorageFailsWithoutDereference)
{
    os1::log::Ring ring;
    os1_log_record r{};
    EXPECT_FALSE(ring.append(r));
    EXPECT_FALSE(ring.next(0, 99, r));
    ring.initialize(nullptr, 0);
    EXPECT_FALSE(ring.append(r));
}
