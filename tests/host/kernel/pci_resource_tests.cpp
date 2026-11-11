#include <gtest/gtest.h>

#include <array>
#include <cstring>

#include "drivers/bus/resource.hpp"
#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "platform/state.hpp"
#include "support/physical_memory.hpp"

namespace
{
constexpr uint64_t kArenaBytes = 4ull * 1024ull * 1024ull;
constexpr uint64_t kBitmapPhysical = 0x100000;

PageFrameContainer make_frames()
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

void reset_platform_state()
{
    platform_reset_state();
    g_platform.device_count = 1;
    g_platform.devices[0].bars[0].base = 0x100000;
    g_platform.devices[0].bars[0].size = 0x2000;
    g_platform.devices[0].bars[0].type = PciBarType::Mmio32;
}
}  // namespace

TEST(PciResource, ClaimIsIdempotentForSameOwner)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    reset_platform_state();

    PciBarResource resource{};
    ASSERT_TRUE(claim_pci_bar(DeviceId{DeviceBus::Pci, 0}, 0, 0, resource));
    ASSERT_TRUE(claim_pci_bar(DeviceId{DeviceBus::Pci, 0}, 0, 0, resource));
    EXPECT_EQ(1u, pci_bar_claim_count());
    EXPECT_EQ(0x100000u, resource.base);

    platform_reset_state();
}

TEST(PciResource, ClaimRejectsDifferentOwner)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    reset_platform_state();

    PciBarResource resource{};
    ASSERT_TRUE(claim_pci_bar(DeviceId{DeviceBus::Pci, 0}, 0, 0, resource));
    EXPECT_FALSE(claim_pci_bar(DeviceId{DeviceBus::Pci, 1}, 0, 0, resource));

    platform_reset_state();
}

TEST(PciResource, ReleaseMakesClaimInactive)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    reset_platform_state();

    PciBarResource resource{};
    ASSERT_TRUE(claim_pci_bar(DeviceId{DeviceBus::Pci, 0}, 0, 0, resource));
    release_pci_bar(DeviceId{DeviceBus::Pci, 0}, 0, 0);

    EXPECT_TRUE(visit_pci_bar_claims([](void*, const PciBarClaim&) { return false; }, nullptr));
    EXPECT_EQ(0u, pci_bar_claim_count());

    platform_reset_state();
}

TEST(PciResource, ClaimsGrowPastLegacyLimit)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    platform_reset_state();

    constexpr size_t kClaimTarget = kPlatformMaxPciBarClaims + 4u;
    g_platform.device_count = kClaimTarget;
    for(size_t i = 0; i < kClaimTarget; ++i)
    {
        g_platform.devices[i].bars[0].base = 0x100000u + static_cast<uint64_t>(i) * 0x1000u;
        g_platform.devices[i].bars[0].size = 0x1000u;
        g_platform.devices[i].bars[0].type = PciBarType::Mmio32;

        PciBarResource resource{};
        ASSERT_TRUE(claim_pci_bar(DeviceId{DeviceBus::Pci, static_cast<uint16_t>(i)},
                                  static_cast<uint16_t>(i),
                                  0,
                                  resource));
    }

    EXPECT_EQ(kClaimTarget, pci_bar_claim_count());

    platform_reset_state();
}
