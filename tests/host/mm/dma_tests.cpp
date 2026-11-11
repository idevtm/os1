#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <thread>

#include "handoff/memory_layout.h"
#include "mm/dma.hpp"
#include "mm/kmem.hpp"
#include "platform/state.hpp"
#include "support/physical_memory.hpp"

namespace
{
constexpr uint64_t kArenaBytes = 16ull * 1024ull * 1024ull;
constexpr uint64_t kBitmapPhysical = 0x300000;

PageFrameContainer initialized_frames()
{
    std::array<BootMemoryRegion, 1> regions{{
        {
            .physical_start = 0,
            .length = kArenaBytes,
            .type = BootMemoryType::Usable,
            .attributes = 0,
        },
    }};

    PageFrameContainer frames;
    EXPECT_TRUE(frames.initialize(regions, kBitmapPhysical, kPageFrameBitmapQwordLimit));
    return frames;
}
}  // namespace

TEST(DmaBuffer, AllocateAndReleasePublishesOwnership)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);
    platform_reset_state();

    DmaBuffer buffer{};
    ASSERT_TRUE(dma_allocate_buffer(
        frames, DeviceId{DeviceBus::Pci, 5}, 6000, DmaDirection::Bidirectional, buffer));
    ASSERT_TRUE(buffer.active);
    EXPECT_EQ(2u, buffer.page_count);
    EXPECT_NE(nullptr, buffer.virtual_address);
    EXPECT_EQ(1u, dma_allocation_count());

    DmaAllocationRecord record{};
    EXPECT_TRUE(visit_dma_allocations(
        [](void* out, const DmaAllocationRecord& source) {
            *static_cast<DmaAllocationRecord*>(out) = source;
            return true;
        },
        &record));
    EXPECT_TRUE(record.active);

    dma_release_buffer(frames, buffer);
    EXPECT_FALSE(buffer.active);
    EXPECT_EQ(0u, dma_allocation_count());

    platform_reset_state();
}

TEST(DmaBuffer, AllocationsGrowPastLegacyLimit)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);
    platform_reset_state();

    constexpr size_t kAllocationTarget = kPlatformMaxDmaAllocations + 4u;
    std::array<DmaBuffer, kAllocationTarget> buffers{};
    for(size_t i = 0; i < buffers.size(); ++i)
    {
        ASSERT_TRUE(dma_allocate_buffer(frames,
                                        DeviceId{DeviceBus::Pci, static_cast<uint16_t>(i)},
                                        kPageSize,
                                        DmaDirection::Bidirectional,
                                        buffers[i]));
    }

    EXPECT_EQ(kAllocationTarget, dma_allocation_count());

    for(auto& buffer : buffers)
    {
        dma_release_buffer(frames, buffer);
    }
    EXPECT_EQ(0u, dma_allocation_count());

    platform_reset_state();
}

TEST(DmaBuffer, ConcurrentPublishersHaveNoSharedSnapshotCapacity)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);
    platform_reset_state();

    std::array<DmaBuffer, 64> buffers{};
    bool completed[2] = {};
    auto allocate = [&](unsigned group) {
        completed[group] = true;
        for(unsigned i = group; i < buffers.size(); i += 2)
            completed[group] &= dma_allocate_buffer(frames,
                                                    {DeviceBus::Pci, static_cast<uint16_t>(i)},
                                                    4096,
                                                    DmaDirection::Bidirectional,
                                                    buffers[i]);
    };
    std::thread first(allocate, 0), second(allocate, 1);
    first.join();
    second.join();
    EXPECT_TRUE(completed[0] && completed[1]);
    std::array<bool, 64> seen{};
    EXPECT_TRUE(visit_dma_allocations(
        [](void* out, const DmaAllocationRecord& record) {
            auto& seen = *static_cast<std::array<bool, 64>*>(out);
            if(!record.active || record.owner.index >= seen.size() || seen[record.owner.index])
                return false;
            seen[record.owner.index] = true;
            return true;
        },
        &seen));
    for(size_t i = 0; i < buffers.size(); ++i)
    {
        EXPECT_TRUE(seen[i]);
        dma_release_buffer(frames, buffers[i]);
    }
    EXPECT_EQ(0u, dma_allocation_count());
    platform_reset_state();
}
