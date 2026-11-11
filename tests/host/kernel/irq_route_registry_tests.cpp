#include <gtest/gtest.h>

#include <array>
#include <cstring>

#include "arch/x86_64/interrupt/interrupt.hpp"
#include "arch/x86_64/interrupt/vector_allocator.hpp"
#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "platform/irq_registry.hpp"
#include "platform/platform.hpp"
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

void reset_state()
{
    platform_reset_state();
    irq_vector_allocator_reset();
}

struct ScopedIrqRegistryCleanup
{
    ~ScopedIrqRegistryCleanup()
    {
        platform_reset_state();
        irq_vector_allocator_reset();
    }
};
}  // namespace

TEST(IrqRouteRegistry, RegistersLegacyRoutesByVector)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    ScopedIrqRegistryCleanup cleanup;
    reset_state();

    ASSERT_TRUE(platform_register_isa_irq_route(DeviceId{DeviceBus::Platform, 0},
                                                IRQ_TIMER,
                                                IRQ_TIMER,
                                                0,
                                                static_cast<uint8_t>(T_IRQ0 + IRQ_TIMER)));
    ASSERT_EQ(1u, platform_irq_route_count());

    IrqRoute route{};
    ASSERT_TRUE(platform_find_irq_route(static_cast<uint8_t>(T_IRQ0 + IRQ_TIMER), route));
    EXPECT_EQ(IrqRouteKind::LegacyIsa, route.kind);
    EXPECT_EQ(DeviceBus::Platform, route.owner.bus);
    EXPECT_EQ(IRQ_TIMER, route.source_irq);
    EXPECT_EQ(IRQ_TIMER, route.gsi);
}

TEST(IrqRouteRegistry, ReleasingDynamicRouteReturnsVectorToAllocator)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    ScopedIrqRegistryCleanup cleanup;
    reset_state();

    uint8_t vector = 0;
    ASSERT_TRUE(irq_allocate_vector(vector));
    ASSERT_TRUE(platform_register_msix_irq_route(DeviceId{DeviceBus::Pci, 7}, 0, vector));
    EXPECT_TRUE(irq_vector_is_allocated(vector));

    ASSERT_TRUE(platform_release_irq_route(vector));
    EXPECT_FALSE(irq_vector_is_allocated(vector));
    {
        IrqRoute absent{};
        EXPECT_FALSE(platform_find_irq_route(vector, absent));
    }
}

TEST(IrqRouteRegistry, AllocatesAndReleasesLocalApicRoutes)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    ScopedIrqRegistryCleanup cleanup;
    reset_state();

    uint8_t vector = 0;
    ASSERT_TRUE(
        platform_allocate_local_apic_irq_route(DeviceId{DeviceBus::Platform, 2}, T_LTIMER, vector));
    EXPECT_TRUE(irq_vector_is_allocated(vector));

    IrqRoute route{};
    ASSERT_TRUE(platform_find_irq_route(vector, route));
    EXPECT_EQ(IrqRouteKind::LocalApic, route.kind);
    EXPECT_EQ(DeviceBus::Platform, route.owner.bus);
    EXPECT_EQ(static_cast<uint16_t>(T_LTIMER), route.source_id);

    ASSERT_TRUE(platform_release_irq_route(vector));
    EXPECT_FALSE(irq_vector_is_allocated(vector));
    {
        IrqRoute absent{};
        EXPECT_FALSE(platform_find_irq_route(vector, absent));
    }
}

TEST(IrqRouteRegistry, DuplicateVectorsAreRejected)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    ScopedIrqRegistryCleanup cleanup;
    reset_state();

    const uint8_t vector = static_cast<uint8_t>(T_IRQ0 + IRQ_KBD);
    ASSERT_TRUE(platform_register_isa_irq_route(
        DeviceId{DeviceBus::Platform, 1}, IRQ_KBD, IRQ_KBD, 0, vector));
    EXPECT_FALSE(platform_register_msi_irq_route(DeviceId{DeviceBus::Pci, 1}, 0, vector));
}

TEST(IrqRouteRegistry, RegistersPastLegacyLimit)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    ScopedIrqRegistryCleanup cleanup;
    reset_state();

    constexpr size_t kRouteTarget = kPlatformMaxIrqRoutes + 4u;
    for(size_t i = 0; i < kRouteTarget; ++i)
    {
        const size_t raw_vector = static_cast<size_t>(kDynamicIrqVectorBase) + i;
        const uint8_t vector =
            static_cast<uint8_t>(raw_vector + ((raw_vector >= T_SYSCALL) ? 1u : 0u));
        ASSERT_TRUE(platform_register_isa_irq_route(DeviceId{DeviceBus::Platform, 1},
                                                    static_cast<uint8_t>(i),
                                                    static_cast<uint32_t>(i),
                                                    0,
                                                    vector));
    }

    EXPECT_EQ(kRouteTarget, platform_irq_route_count());
}
