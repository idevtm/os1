#include <gtest/gtest.h>

#include <array>
#include <cstring>

#include "arch/x86_64/interrupt/interrupt.hpp"
#include "core/timer_source.hpp"
#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "platform/irq_registry.hpp"
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
    timer_source_set_scheduler(SchedulerTimerSource::Pit);
    timer_source_set_lapic_calibration(0, 0, 1000);
}

struct ScopedTimerSourceCleanup
{
    ~ScopedTimerSourceCleanup()
    {
        reset_state();
    }
};
}  // namespace

TEST(TimerSource, PitModeMatchesLegacyTimerOnly)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    ScopedTimerSourceCleanup cleanup;
    reset_state();

    EXPECT_TRUE(timer_vector_is_scheduler_tick(static_cast<uint8_t>(T_IRQ0 + IRQ_TIMER)));
    EXPECT_FALSE(timer_vector_is_scheduler_tick(T_LTIMER));
}

TEST(TimerSource, LapicModeMatchesOnlyTheCalibratedVector)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    ScopedTimerSourceCleanup cleanup;
    reset_state();

    uint8_t vector = 0;
    ASSERT_TRUE(
        platform_allocate_local_apic_irq_route(DeviceId{DeviceBus::Platform, 2}, T_LTIMER, vector));
    uint8_t other_timer = 0;
    ASSERT_TRUE(platform_allocate_local_apic_irq_route(
        DeviceId{DeviceBus::Platform, 3}, T_LTIMER, other_timer));
    timer_source_set_lapic_calibration(vector, 12345, 1000);
    timer_source_set_scheduler(SchedulerTimerSource::Lapic);
    EXPECT_TRUE(timer_vector_is_scheduler_tick(vector));
    EXPECT_FALSE(timer_vector_is_scheduler_tick(other_timer));
    EXPECT_FALSE(timer_vector_is_scheduler_tick(T_LTIMER));
    EXPECT_FALSE(timer_vector_is_scheduler_tick(static_cast<uint8_t>(T_IRQ0 + IRQ_TIMER)));
}

TEST(TimerSource, LapicModeRequiresValidCalibration)
{
    timer_source_set_scheduler(SchedulerTimerSource::Lapic);
    timer_source_set_lapic_calibration(0xA0, 0, 1000);
    EXPECT_FALSE(timer_vector_is_scheduler_tick(0xA0));
    EXPECT_FALSE(timer_vector_is_scheduler_tick(0));
    timer_source_set_scheduler(SchedulerTimerSource::Pit);
    EXPECT_TRUE(timer_vector_is_scheduler_tick(static_cast<uint8_t>(T_IRQ0 + IRQ_TIMER)));
    timer_source_set_lapic_calibration(0, 0, 1000);
}

TEST(TimerSource, LapicCalibrationCacheRoundTrips)
{
    timer_source_set_lapic_calibration(0, 0, 1000);
    LapicTimerCalibration empty = timer_source_lapic_calibration();
    EXPECT_FALSE(empty.valid);
    EXPECT_EQ(0u, empty.vector);
    EXPECT_EQ(0u, empty.initial_count);

    timer_source_set_lapic_calibration(0xA0, 12345, 1000);
    LapicTimerCalibration cal = timer_source_lapic_calibration();
    EXPECT_TRUE(cal.valid);
    EXPECT_EQ(0xA0u, cal.vector);
    EXPECT_EQ(12345u, cal.initial_count);
    EXPECT_EQ(1000u, cal.frequency_hz);

    // Reset for downstream tests.
    timer_source_set_lapic_calibration(0, 0, 1000);
}

TEST(TimerSource, ApTimerGateDefaultsEnabled)
{
    timer_source_set_ap_timer_enabled(true);
    EXPECT_TRUE(timer_source_ap_timer_enabled());

    timer_source_set_ap_timer_enabled(false);
    EXPECT_FALSE(timer_source_ap_timer_enabled());

    timer_source_set_ap_timer_enabled(true);
}

TEST(TimerSource, CpuStartLocalApicTimerRejectsWhenCalibrationMissing)
{
    timer_source_set_lapic_calibration(0, 0, 1000);
    EXPECT_FALSE(cpu_start_local_apic_timer());
}