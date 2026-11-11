#include "mm/page_frame.hpp"

#include "handoff/memory_layout.h"
#include "support/physical_memory.hpp"

#include <gtest/gtest.h>

#include <array>

namespace
{
constexpr uint64_t kArenaBytes = 16ull * 1024ull * 1024ull;
constexpr uint64_t kBitmapPhysical = 0x300000;

std::array<BootMemoryRegion, 1> usable_memory()
{
    return {{
        {
            .physical_start = 0,
            .length = kArenaBytes,
            .type = BootMemoryType::Usable,
            .attributes = 0,
        },
    }};
}

PageFrameContainer initialized_frames(os1::host_test::PhysicalMemoryArena& arena)
{
    (void)arena;
    PageFrameContainer frames;
    auto regions = usable_memory();
    EXPECT_TRUE(frames.initialize(regions, kBitmapPhysical, kPageFrameBitmapQwordLimit));
    return frames;
}
}  // namespace

TEST(PageFrameContainer, RejectsInvalidInitialization)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames;
    std::array<BootMemoryRegion, 1> reserved{{
        {0, kArenaBytes, BootMemoryType::Reserved, 0},
    }};
    EXPECT_FALSE(frames.initialize(reserved, kBitmapPhysical, kPageFrameBitmapQwordLimit));

    PageFrameContainer unaligned;
    std::array<BootMemoryRegion, 1> bad{{
        {1, kPageSize, BootMemoryType::Usable, 0},
    }};
    EXPECT_FALSE(unaligned.initialize(bad, kBitmapPhysical, kPageFrameBitmapQwordLimit));
}

TEST(PageFrameContainer, InitializesAndReservesBootRanges)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames(arena);

    const uint64_t reserved_pages = (kPageFrameBitmapSizeBytes / kPageSize) +
                                    (kEarlyReservedPhysicalEnd / kPageSize) +
                                    ((kKernelReservedPhysicalEnd - kKernelReservedPhysicalStart) / kPageSize);
    const uint64_t expected_free_pages = (kArenaBytes / kPageSize) - reserved_pages;

    EXPECT_EQ(kArenaBytes, frames.memory_size());
    EXPECT_EQ(kArenaBytes, frames.memory_end());
    EXPECT_EQ(kArenaBytes / kPageSize, frames.page_count());
    EXPECT_EQ(expected_free_pages, frames.free_page_count());

    uint64_t page = 0;
    ASSERT_TRUE(frames.allocate(page));
    EXPECT_EQ(kEarlyReservedPhysicalEnd, page);
    EXPECT_EQ(expected_free_pages - 1u, frames.free_page_count());
}

TEST(PageFrameContainer, AllocatesFreesAndReservesRanges)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames(arena);

    uint64_t first = 0;
    ASSERT_TRUE(frames.allocate(first));
    EXPECT_TRUE(frames.free(first));
    EXPECT_FALSE(frames.free(first + 1));

    uint64_t contiguous = 0;
    ASSERT_TRUE(frames.allocate(contiguous, 3));
    EXPECT_EQ(first, contiguous);

    const uint64_t before_reserve = frames.free_page_count();
    EXPECT_TRUE(frames.reserve_range(0x400000 + 1, kPageSize));
    EXPECT_EQ(before_reserve - 2, frames.free_page_count());
    EXPECT_FALSE(frames.reserve_range(kArenaBytes + kPageSize, kPageSize));
}

TEST(PageFrameContainer, ContiguousAllocationKeepsFirstFitAcrossBitmapWords)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames(arena);
    ASSERT_TRUE(frames.reserve_range(0, kArenaBytes));
    constexpr uint64_t base = 4ull * 1024 * 1024;
    ASSERT_TRUE(frames.free(base + 3 * kPageSize));
    for(unsigned page = 61; page < 201; ++page)
    {
        ASSERT_TRUE(frames.free(base + page * kPageSize));
    }
    const uint64_t free_before = frames.free_page_count();

    for(unsigned count : {4u, 63u, 64u, 65u, 128u})
    {
        uint64_t run = 0;
        ASSERT_TRUE(frames.allocate(run, count));
        EXPECT_EQ(base + 61 * kPageSize, run);
        EXPECT_EQ(free_before - count, frames.free_page_count());
        uint64_t left = 0, right = 0;
        ASSERT_TRUE(frames.allocate(left));
        ASSERT_TRUE(frames.allocate(right));
        EXPECT_EQ(base + 3 * kPageSize, left);
        EXPECT_EQ(run + count * kPageSize, right);
        for(unsigned page = 0; page < count; ++page)
        {
            EXPECT_TRUE(frames.free(run + page * kPageSize));
        }
        EXPECT_TRUE(frames.free(left));
        EXPECT_TRUE(frames.free(right));
        EXPECT_EQ(free_before, frames.free_page_count());
    }
}

TEST(PageFrameContainer, FindsAnInteriorRunOfNonPowerOfTwoLength)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames(arena);
    ASSERT_TRUE(frames.reserve_range(0, kArenaBytes));
    constexpr uint64_t base = 4ull * 1024 * 1024;
    for(unsigned page : {1u, 2u, 3u, 9u, 10u, 11u, 12u, 17u, 18u, 19u, 20u, 21u, 22u})
    {
        ASSERT_TRUE(frames.free(base + page * kPageSize));
    }

    uint64_t run = 0;
    ASSERT_TRUE(frames.allocate(run, 5));
    EXPECT_EQ(base + 17 * kPageSize, run);
    EXPECT_EQ(8u, frames.free_page_count());
}

TEST(PageFrameContainer, FailedRunSearchLeavesFragmentedBitmapAndTailUnchanged)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames;
    constexpr uint64_t memory_end = kArenaBytes - 3 * kPageSize;
    const std::array<BootMemoryRegion, 1> regions{{{0, memory_end, BootMemoryType::Usable, 0}}};
    ASSERT_TRUE(frames.initialize(regions, kBitmapPhysical, kPageFrameBitmapQwordLimit));
    ASSERT_TRUE(frames.reserve_range(0, memory_end));
    ASSERT_TRUE(frames.free(4ull * 1024 * 1024));
    const uint64_t tail = memory_end - 3 * kPageSize;
    for(unsigned page = 0; page < 3; ++page)
    {
        ASSERT_TRUE(frames.free(tail + page * kPageSize));
    }

    uint64_t run = ~uint64_t{0};
    for(unsigned count : {0u, 4u, 5u})
    {
        EXPECT_FALSE(frames.allocate(run, count));
        EXPECT_EQ(~uint64_t{0}, run);
        EXPECT_EQ(4u, frames.free_page_count());
    }
    ASSERT_TRUE(frames.allocate(run, 3));
    EXPECT_EQ(tail, run);
    EXPECT_EQ(1u, frames.free_page_count());
}
