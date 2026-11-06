#include <gtest/gtest.h>

#include <array>

#include "arch/x86_64/apic/ipi.hpp"
#include "handoff/memory_layout.h"
#include "mm/virtual_memory.hpp"
#include "support/lapic_stub.hpp"
#include "support/physical_memory.hpp"

namespace
{
constexpr uint64_t kArenaBytes = 32ull * 1024ull * 1024ull;
constexpr uint64_t kBitmapPhysical = 0x300000;

PageFrameContainer make_frames(os1::host_test::PhysicalMemoryArena& arena)
{
    (void)arena;
    std::array<BootMemoryRegion, 1> regions{{
        {0, kArenaBytes, BootMemoryType::Usable, 0},
    }};
    PageFrameContainer frames;
    EXPECT_TRUE(frames.initialize(regions, kBitmapPhysical, kPageFrameBitmapQwordLimit));
    return frames;
}
}  // namespace

TEST(VirtualMemory, RejectsInvalidMapRequests)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames(arena);
    VirtualMemory vm(frames);

    EXPECT_FALSE(vm.map_physical(0x400000, 0x500000, 0, PageFlags::Present));
    EXPECT_FALSE(vm.map_physical(0x400001, 0x500000, 1, PageFlags::Present));
    EXPECT_FALSE(vm.map_physical(0x400000, 0x500001, 1, PageFlags::Present));
}

TEST(VirtualMemory, MapsProtectsAndTranslatesPages)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames(arena);
    VirtualMemory vm(frames);

    ASSERT_TRUE(vm.map_physical(0x0000008000400000ull,
                                0x00500000,
                                2,
                                PageFlags::Present | PageFlags::User | PageFlags::Write));

    uint64_t physical = 0;
    uint64_t flags = 0;
    ASSERT_TRUE(vm.translate(0x0000008000400123ull, physical, flags));
    EXPECT_EQ(0x00500123ull, physical);
    EXPECT_EQ(static_cast<uint64_t>(PageFlags::Present | PageFlags::User | PageFlags::Write),
              flags);

    ASSERT_TRUE(vm.protect(
        0x0000008000400000ull, 1, PageFlags::Present | PageFlags::User | PageFlags::NoExecute));
    ASSERT_TRUE(vm.translate(0x0000008000400000ull, physical, flags));
    EXPECT_EQ(0x00500000ull, physical);
    EXPECT_EQ(static_cast<uint64_t>(PageFlags::Present | PageFlags::User | PageFlags::NoExecute),
              flags);
}

TEST(VirtualMemory, ProtectingUserMappingsTriggersTlbShootdown)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames(arena);
    VirtualMemory vm(frames);

    ASSERT_TRUE(ipi_initialize());
    lapic_stub_reset();
    ASSERT_TRUE(vm.map_physical(
        kUserImageBase, 0x00500000, 1, PageFlags::Present | PageFlags::User | PageFlags::Write));

    ASSERT_TRUE(
        vm.protect(kUserImageBase, 1, PageFlags::Present | PageFlags::User | PageFlags::NoExecute));
    EXPECT_EQ(1u, lapic_stub_icr_send_count());
    EXPECT_EQ(ipi_tlb_shootdown_vector(), static_cast<uint8_t>(lapic_stub_last_icr_low()));
}

TEST(VirtualMemory, AllocatesAndDestroysUserSlot)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames(arena);
    VirtualMemory vm(frames);

    const uint64_t before = frames.free_page_count();
    uint64_t first_physical = 0;
    ASSERT_TRUE(vm.allocate_and_map(kUserImageBase,
                                    1,
                                    PageFlags::Present | PageFlags::User | PageFlags::Write,
                                    &first_physical));
    EXPECT_NE(0ull, first_physical);
    EXPECT_LT(frames.free_page_count(), before);

    uint64_t translated = 0;
    uint64_t flags = 0;
    ASSERT_TRUE(vm.translate(kUserImageBase, translated, flags));
    EXPECT_EQ(first_physical, translated);

    ASSERT_TRUE(vm.destroy_user_slot(kUserPml4Index));
    EXPECT_FALSE(vm.translate(kUserImageBase, translated, flags));
}

TEST(VirtualMemory, ClonesKernelAndDirectMapSlots)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames(arena);
    VirtualMemory source(frames);
    VirtualMemory target(frames);

    ASSERT_TRUE(source.map_physical(kKernelVirtualBase, 0x100000, 1, PageFlags::Present));
    ASSERT_TRUE(source.map_physical(
        phys_to_virt(0x700000), 0x700000, 1, PageFlags::Present | PageFlags::Write));
    ASSERT_TRUE(target.clone_kernel_mappings(source.root()));

    uint64_t physical = 0;
    uint64_t flags = 0;
    EXPECT_TRUE(target.translate(kKernelVirtualBase, physical, flags));
    EXPECT_EQ(0x100000ull, physical);
    EXPECT_TRUE(target.translate(phys_to_virt(0x700000), physical, flags));
    EXPECT_EQ(0x700000ull, physical);
}

TEST(VirtualMemory, FailedMappingReturnsUnlinkedDataFrame)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames(arena);
    // Leave exactly one frame: data allocation succeeds, page-table allocation fails.
    std::array<uint64_t, kArenaBytes / kPageSize> held{};
    size_t count = 0;
    while(frames.free_page_count() > 1)
        ASSERT_TRUE(frames.allocate(held[count++]));
    VirtualMemory vm(frames);
    ASSERT_EQ(1u, frames.free_page_count());
    EXPECT_FALSE(vm.allocate_and_map(kUserImageBase, 1, PageFlags::User | PageFlags::Write));
    EXPECT_EQ(1u, frames.free_page_count());
    while(count)
        EXPECT_TRUE(frames.free(held[--count]));
}

TEST(VirtualMemory, TeardownNeverInterpretsUserBytesAsPageTableEntries)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames(arena);
    const uint64_t before = frames.free_page_count();
    VirtualMemory vm(frames);
    uint64_t data = 0;
    ASSERT_TRUE(vm.allocate_and_map(kUserImageBase, 1, PageFlags::User | PageFlags::Write, &data));
    // Ordinary user bytes happen to look like a PTE naming a reserved boot page.
    *kernel_physical_pointer<uint64_t>(data) = 0x1003;
    ASSERT_TRUE(vm.destroy_user_slot(kUserPml4Index));
    ASSERT_TRUE(frames.free(vm.root()));
    EXPECT_EQ(before, frames.free_page_count());
    uint64_t next = 0;
    ASSERT_TRUE(frames.allocate(next));
    EXPECT_GE(next, kEarlyReservedPhysicalEnd);
}

TEST(VirtualMemory, SharedBackingSurvivesAddressSpaceDestruction)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames(arena);
    uint64_t backing = 0;
    ASSERT_TRUE(frames.allocate(backing));
    const auto baseline = frames.free_page_count();
    VirtualMemory vm(frames);
    ASSERT_TRUE(
        vm.map_physical(kUserImageBase, backing, 1, PageFlags::User | PageFlags::SharedBacking));
    const auto root = vm.root();
    ASSERT_TRUE(vm.destroy_user_slot(kUserPml4Index));
    ASSERT_TRUE(frames.free(root));
    EXPECT_EQ(baseline, frames.free_page_count());
    ASSERT_TRUE(frames.free(backing));
    EXPECT_EQ(baseline + 1, frames.free_page_count());
}

TEST(VirtualMemory, InheritsHighPciMappingsWithoutSharingUserSlot)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    auto frames = make_frames(arena);
    VirtualMemory source(frames), target(frames);
    constexpr uint64_t physical_bar = 0xc000000000ull;
    constexpr uint64_t virtual_bar = 0xffff80c000000000ull;
    ASSERT_TRUE(source.map_physical(virtual_bar, physical_bar, 1, PageFlags::Write));
    ASSERT_TRUE(source.map_physical(kUserImageBase, 0x700000, 1, PageFlags::User));
    ASSERT_TRUE(target.clone_kernel_mappings(source.root()));
    uint64_t physical = 0, flags = 0;
    ASSERT_TRUE(target.translate(virtual_bar + 0x16, physical, flags));
    EXPECT_EQ(physical, physical_bar + 0x16);
    auto* top = kernel_physical_pointer<const uint64_t>(target.root());
    EXPECT_EQ(top[(virtual_bar >> 39) & 511] & uint64_t(PageFlags::User), 0u);
    EXPECT_FALSE(target.translate(kUserImageBase, physical, flags));
}
