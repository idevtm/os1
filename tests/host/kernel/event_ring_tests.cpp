#include "debug/event_ring.hpp"

#include <gtest/gtest.h>

#include <array>

TEST(EventRing, StartsEmpty)
{
    kernel_event::KernelEventRing ring;
    std::array<Os1ObserveEventRecord, 4> records{};

    EXPECT_EQ(0u, ring.count());
    EXPECT_EQ(1u, ring.next_sequence());
    EXPECT_EQ(0u, ring.snapshot(records.data(), static_cast<uint32_t>(records.size())));
}

TEST(EventRing, AppendsRecordsWithContextAndSequence)
{
    kernel_event::KernelEventRing ring;
    std::array<Os1ObserveEventRecord, 2> records{};

    ring.append(10,
                OS1_KERNEL_EVENT_IRQ,
                OS1_KERNEL_EVENT_FLAG_SUCCESS,
                3,
                11,
                12,
                33,
                34,
                35,
                36);

    ASSERT_EQ(1u, ring.snapshot(records.data(), static_cast<uint32_t>(records.size())));
    EXPECT_EQ(1u, records[0].sequence);
    EXPECT_EQ(10u, records[0].tick_count);
    EXPECT_EQ(OS1_KERNEL_EVENT_IRQ, records[0].type);
    EXPECT_EQ(OS1_KERNEL_EVENT_FLAG_SUCCESS, records[0].flags);
    EXPECT_EQ(3u, records[0].cpu);
    EXPECT_EQ(11u, records[0].pid);
    EXPECT_EQ(12u, records[0].tid);
    EXPECT_EQ(33u, records[0].arg0);
    EXPECT_EQ(34u, records[0].arg1);
    EXPECT_EQ(35u, records[0].arg2);
    EXPECT_EQ(36u, records[0].arg3);
    EXPECT_EQ(2u, ring.next_sequence());
}

TEST(EventRing, SnapshotIsOldestToNewest)
{
    kernel_event::KernelEventRing ring;
    std::array<Os1ObserveEventRecord, 3> records{};

    ring.append(1, OS1_KERNEL_EVENT_TRAP, 0, 0, 1, 1, 10, 0, 0, 0);
    ring.append(2, OS1_KERNEL_EVENT_IRQ, 0, 0, 1, 1, 20, 0, 0, 0);
    ring.append(3, OS1_KERNEL_EVENT_BLOCK_IO, 0, 0, 1, 1, 30, 0, 0, 0);

    ASSERT_EQ(3u, ring.snapshot(records.data(), static_cast<uint32_t>(records.size())));
    EXPECT_EQ(1u, records[0].sequence);
    EXPECT_EQ(2u, records[1].sequence);
    EXPECT_EQ(3u, records[2].sequence);
    EXPECT_EQ(10u, records[0].arg0);
    EXPECT_EQ(20u, records[1].arg0);
    EXPECT_EQ(30u, records[2].arg0);
}

TEST(EventRing, WrapOverwritesOldestRecords)
{
    kernel_event::KernelEventRing ring;
    std::array<Os1ObserveEventRecord, OS1_OBSERVE_EVENT_RING_CAPACITY> records{};

    for(uint32_t i = 0; i < OS1_OBSERVE_EVENT_RING_CAPACITY + 5u; ++i)
    {
        ring.append(i, OS1_KERNEL_EVENT_SMOKE_MARKER, 0, 0, 0, 0, i, 0, 0, 0);
    }

    ASSERT_EQ(OS1_OBSERVE_EVENT_RING_CAPACITY,
              ring.snapshot(records.data(), static_cast<uint32_t>(records.size())));
    EXPECT_EQ(6u, records[0].sequence);
    EXPECT_EQ(5u, records[0].arg0);
    EXPECT_EQ(OS1_OBSERVE_EVENT_RING_CAPACITY + 5u, records.back().sequence);
    EXPECT_EQ(OS1_OBSERVE_EVENT_RING_CAPACITY + 4u, records.back().arg0);
}

TEST(EventRing, SmallSnapshotReturnsNewestRecords)
{
    kernel_event::KernelEventRing ring;
    std::array<Os1ObserveEventRecord, 2> records{};

    for(uint32_t i = 0; i < 5u; ++i)
    {
        ring.append(i, OS1_KERNEL_EVENT_SMOKE_MARKER, 0, 0, 0, 0, i, 0, 0, 0);
    }

    ASSERT_EQ(2u, ring.snapshot(records.data(), static_cast<uint32_t>(records.size())));
    EXPECT_EQ(4u, records[0].sequence);
    EXPECT_EQ(5u, records[1].sequence);
}

TEST(EventRing, ResetClearsRecordsAndSequence)
{
    kernel_event::KernelEventRing ring;
    std::array<Os1ObserveEventRecord, 1> records{};

    ring.append(1, OS1_KERNEL_EVENT_TRAP, 0, 0, 0, 0, 0, 0, 0, 0);
    ring.reset();

    EXPECT_EQ(0u, ring.count());
    EXPECT_EQ(1u, ring.next_sequence());
    EXPECT_EQ(0u, ring.snapshot(records.data(), static_cast<uint32_t>(records.size())));
}

TEST(EventRing, RingWithoutStorageCountsSequencesButKeepsNoRecords)
{
    kernel_event::KernelEventRing ring;
    ring.attach(nullptr);
    std::array<Os1ObserveEventRecord, 2> records{};

    const auto record = ring.append(5, OS1_KERNEL_EVENT_IRQ, 0, 0, 0, 0, 1, 2, 3, 4);
    EXPECT_EQ(1u, record.sequence);
    EXPECT_EQ(2u, ring.next_sequence());
    EXPECT_EQ(0u, ring.count());
    EXPECT_EQ(0u, ring.snapshot(records.data(), static_cast<uint32_t>(records.size())));
    ring.reset();
    EXPECT_EQ(0u, ring.count());
}

TEST(EventRing, AttachedStorageStartsEmptyAndKeepsTheSequence)
{
    kernel_event::KernelEventRing ring;
    ring.attach(nullptr);
    ring.append(1, OS1_KERNEL_EVENT_IRQ, 0, 0, 0, 0, 0, 0, 0, 0);
    std::array<Os1ObserveEventRecord, kernel_event::kEventRingCapacity> storage{};
    storage[0].sequence = 99;
    ring.attach(storage.data());
    EXPECT_EQ(0u, ring.count());
    EXPECT_EQ(0u, storage[0].sequence);

    ring.append(2, OS1_KERNEL_EVENT_IRQ, 0, 0, 0, 0, 7, 0, 0, 0);
    std::array<Os1ObserveEventRecord, 1> records{};
    ASSERT_EQ(1u, ring.snapshot(records.data(), 1));
    EXPECT_EQ(2u, records[0].sequence);
    EXPECT_EQ(7u, records[0].arg0);
    EXPECT_EQ(2u, storage[0].sequence);
}
