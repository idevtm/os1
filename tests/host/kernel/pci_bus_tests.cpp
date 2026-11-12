#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

#include "arch/x86_64/interrupt/vector_allocator.hpp"
#include "drivers/bus/device.hpp"
#include "drivers/bus/driver_registry.hpp"
#include "drivers/bus/pci_bus.hpp"
#include "drivers/bus/resource.hpp"
#include "handoff/memory_layout.h"
#include "hardware/tree.hpp"
#include "mm/dma.hpp"
#include "mm/kmem.hpp"
#include "mm/virtual_memory.hpp"
#include "platform/hardware.hpp"
#include "platform/irq_registry.hpp"
#include "platform/pci.hpp"
#include "platform/state.hpp"
#include "support/physical_memory.hpp"

namespace
{
constexpr uint64_t kArenaBytes = 16ull * 1024ull * 1024ull;
constexpr uint64_t kBitmapPhysical = 0x300000;

struct ProbeHarness
{
    size_t first_probe_calls = 0;
    size_t second_probe_calls = 0;
    size_t failing_probe_calls = 0;
    DeviceId last_probe_id{DeviceBus::Platform, 0};
};

struct RemoveHarness
{
    bool remove_called = false;
    bool binding_visible_during_remove = false;
    PageFrameContainer* frames = nullptr;
    DmaBuffer dma_buffer{};
};

ProbeHarness g_probe_harness;
RemoveHarness g_remove_harness;

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
    driver_registry_reset();
    g_probe_harness = {};
    g_remove_harness = {};
}

bool first_probe(VirtualMemory&, PageFrameContainer&, const PciDevice&, size_t, DeviceId)
{
    ++g_probe_harness.first_probe_calls;
    return true;
}

bool second_probe(
    VirtualMemory&, PageFrameContainer&, const PciDevice&, size_t device_index, DeviceId id)
{
    ++g_probe_harness.second_probe_calls;
    g_probe_harness.last_probe_id = id;
    return device_binding_publish(
        id, static_cast<uint16_t>(device_index), "second-driver", nullptr);
}

bool failing_probe(VirtualMemory&, PageFrameContainer&, const PciDevice&, size_t, DeviceId)
{
    ++g_probe_harness.failing_probe_calls;
    return false;
}

bool stub_probe(VirtualMemory&, PageFrameContainer&, const PciDevice&, size_t, DeviceId)
{
    return true;
}

void remove_driver(DeviceId id)
{
    g_remove_harness.remove_called = true;
    if(const DeviceBinding* binding = device_binding_find(id))
    {
        g_remove_harness.binding_visible_during_remove = binding->active;
    }

    release_pci_bars_for_owner(id);
    platform_release_irq_routes_for_owner(id);
    if((nullptr != g_remove_harness.frames) && g_remove_harness.dma_buffer.active)
    {
        dma_release_buffer(*g_remove_harness.frames, g_remove_harness.dma_buffer);
    }
}
}  // namespace

TEST(PciDriverRegistry, MatchesClassSubclassAndProgIf)
{
    const PciMatch match{
        .class_code = 0x0c,
        .subclass = 0x03,
        .prog_if = 0x30,
        .match_flags =
            static_cast<uint8_t>(kPciMatchClassCode | kPciMatchSubclass | kPciMatchProgIf),
    };
    const PciDriver driver{
        .name = "xhci",
        .matches = &match,
        .match_count = 1,
        .probe = stub_probe,
    };

    PciDevice device{};
    device.class_code = 0x0c;
    device.subclass = 0x03;
    device.prog_if = 0x30;
    EXPECT_TRUE(pci_driver_matches_device(driver, device));

    device.prog_if = 0x20;
    EXPECT_FALSE(pci_driver_matches_device(driver, device));
}

TEST(PciBus, OnlyProbesMatchingDriver)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    (void)arena;
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    VirtualMemory kernel_vm(frames);
    reset_state();

    g_platform.device_count = 1;
    g_platform.devices[0].vendor_id = 0x1AF4;
    g_platform.devices[0].device_id = 0x1042;

    const PciMatch first_match{
        .vendor_id = 0x8086,
        .device_id = 0x100e,
        .match_flags = static_cast<uint8_t>(kPciMatchVendorId | kPciMatchDeviceId),
    };
    const PciMatch second_match{
        .vendor_id = 0x1AF4,
        .device_id = 0x1042,
        .match_flags = static_cast<uint8_t>(kPciMatchVendorId | kPciMatchDeviceId),
    };
    const PciDriver first_driver{
        .name = "first-driver",
        .matches = &first_match,
        .match_count = 1,
        .probe = first_probe,
    };
    const PciDriver second_driver{
        .name = "second-driver",
        .matches = &second_match,
        .match_count = 1,
        .probe = second_probe,
    };

    ASSERT_TRUE(driver_registry_add_pci_driver(first_driver));
    ASSERT_TRUE(driver_registry_add_pci_driver(second_driver));
    ASSERT_TRUE(pci_bus_probe_all(kernel_vm, frames));
    EXPECT_EQ(0u, g_probe_harness.first_probe_calls);
    EXPECT_EQ(1u, g_probe_harness.second_probe_calls);
    EXPECT_EQ(DeviceBus::Pci, g_probe_harness.last_probe_id.bus);
    EXPECT_EQ(0u, g_probe_harness.last_probe_id.index);

    const DeviceBinding* binding = device_binding_find(DeviceId{DeviceBus::Pci, 0});
    ASSERT_NE(nullptr, binding);
    EXPECT_EQ(DeviceState::Started, binding->state);
    EXPECT_STREQ("second-driver", binding->driver_name);

    platform_reset_state();
}

TEST(PciBus, MatchingProbeFailureDoesNotStopLaterDevices)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    (void)arena;
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    VirtualMemory kernel_vm(frames);
    reset_state();

    g_platform.device_count = 2;
    g_platform.devices[0].vendor_id = 0x1AF4;
    g_platform.devices[0].device_id = 0x1042;
    g_platform.devices[1].vendor_id = 0x8086;
    g_platform.devices[1].device_id = 0x100e;

    const PciMatch failing_match{
        .vendor_id = 0x1AF4,
        .device_id = 0x1042,
        .match_flags = static_cast<uint8_t>(kPciMatchVendorId | kPciMatchDeviceId),
    };
    const PciMatch second_match{
        .vendor_id = 0x8086,
        .device_id = 0x100e,
        .match_flags = static_cast<uint8_t>(kPciMatchVendorId | kPciMatchDeviceId),
    };
    const PciDriver failing_driver{
        .name = "virtio-blk",
        .matches = &failing_match,
        .match_count = 1,
        .probe = failing_probe,
    };
    const PciDriver second_driver{
        .name = "second-driver",
        .matches = &second_match,
        .match_count = 1,
        .probe = second_probe,
    };

    ASSERT_TRUE(driver_registry_add_pci_driver(failing_driver));
    ASSERT_TRUE(driver_registry_add_pci_driver(second_driver));
    EXPECT_TRUE(pci_bus_probe_all(kernel_vm, frames));
    EXPECT_EQ(1u, g_probe_harness.failing_probe_calls);
    ASSERT_NE(nullptr, device_binding_find(DeviceId{DeviceBus::Pci, 0}));
    EXPECT_EQ(DeviceState::Failed, device_binding_find(DeviceId{DeviceBus::Pci, 0})->state);
    hardware_freeze_bindings();
    Os1HardwareQuery description{};
    description.version = 1;
    description.operation = OS1_HARDWARE_DESCRIBE;
    description.id = os1::hardware::object_id(os1::hardware::Pci, 0) + 3;
    ASSERT_EQ(0, hardware_query(description));
    EXPECT_STREQ("failed", description.nodes[0].text);
    description = {};
    description.version = 1;
    description.operation = OS1_HARDWARE_DESCRIBE;
    description.id = os1::hardware::object_id(os1::hardware::Pci, 0) + 4;
    ASSERT_EQ(0, hardware_query(description));
    EXPECT_STREQ("virtio-blk", description.nodes[0].text);
    EXPECT_EQ(1u, g_probe_harness.second_probe_calls);
    EXPECT_EQ(1u, g_probe_harness.last_probe_id.index);
    const DeviceBinding* binding = device_binding_find(DeviceId{DeviceBus::Pci, 1});
    ASSERT_NE(nullptr, binding);
    EXPECT_EQ(DeviceState::Started, binding->state);

    platform_reset_state();
}

TEST(PciBus, RemoveMatchesDriverNameByContentAndReleasesResources)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    reset_state();

    const DeviceId owner{DeviceBus::Pci, 0};
    g_platform.device_count = 1;
    g_platform.devices[0].bars[0].base = 0x100000;
    g_platform.devices[0].bars[0].size = 0x2000;
    g_platform.devices[0].bars[0].type = PciBarType::Mmio32;

    PciBarResource resource{};
    ASSERT_TRUE(claim_pci_bar(owner, 0, 0, resource));

    uint8_t vector = 0;
    ASSERT_TRUE(irq_allocate_vector(vector));
    ASSERT_TRUE(platform_register_msix_irq_route(owner, 0, vector));

    ASSERT_TRUE(dma_allocate_buffer(
        frames, owner, 4096, DmaDirection::Bidirectional, g_remove_harness.dma_buffer));
    g_remove_harness.frames = &frames;

    char binding_name[] = "remove-driver";
    const PciDriver driver{
        .name = "remove-driver",
        .probe = stub_probe,
        .remove = remove_driver,
        .quiesce = [](DeviceId) { return true; },
    };
    ASSERT_NE(binding_name, driver.name);
    ASSERT_TRUE(driver_registry_add_pci_driver(driver));
    ASSERT_TRUE(device_binding_publish(owner, 0, binding_name, nullptr));

    ASSERT_TRUE(pci_bus_remove_device(owner));
    EXPECT_TRUE(g_remove_harness.remove_called);
    EXPECT_TRUE(g_remove_harness.binding_visible_during_remove);
    EXPECT_EQ(nullptr, device_binding_find(owner));
    EXPECT_EQ(0u, device_binding_count());

    EXPECT_TRUE(visit_pci_bar_claims([](void*, const PciBarClaim&) { return false; }, nullptr));
    EXPECT_EQ(0u, pci_bar_claim_count());
    {
        IrqRoute absent{};
        EXPECT_FALSE(platform_find_irq_route(vector, absent));
    }
    EXPECT_FALSE(irq_vector_is_allocated(vector));
    EXPECT_FALSE(g_remove_harness.dma_buffer.active);
    EXPECT_EQ(0u, dma_allocation_count());

    platform_reset_state();
}

TEST(PciBus, DeviceBindingsGrowPastLegacyLimit)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    reset_state();

    constexpr size_t kBindingTarget = kPlatformMaxDeviceBindings + 8u;
    for(size_t i = 0; i < kBindingTarget; ++i)
    {
        ASSERT_TRUE(device_binding_publish(DeviceId{DeviceBus::Pci, static_cast<uint16_t>(i)},
                                           static_cast<uint16_t>(i),
                                           "growth-driver",
                                           nullptr));
    }

    EXPECT_EQ(kBindingTarget, device_binding_count());
    ASSERT_NE(
        nullptr,
        device_binding_find(DeviceId{DeviceBus::Pci, static_cast<uint16_t>(kBindingTarget - 1u)}));

    platform_reset_state();
}

TEST(PciBus, EarlyDriverRunsBeforeLowerBdfOrdinaryDeviceAndDoesNotRetryFailure)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    VirtualMemory kernel_vm(frames);
    for(bool enable_early : {true, false})
    {
        reset_state();
        g_platform.device_count = 2;
        g_platform.devices[0].device_id = 1;
        g_platform.devices[1].device_id = 2;
        const PciMatch ordinary_match{.device_id = 1, .match_flags = kPciMatchDeviceId};
        const PciMatch early_match{.device_id = 2, .match_flags = kPciMatchDeviceId};
        const PciDriver ordinary{
            .name = "ordinary",
            .matches = &ordinary_match,
            .match_count = 1,
            .probe = [](VirtualMemory&, PageFrameContainer&, const PciDevice&, size_t, DeviceId) {
                EXPECT_EQ(g_probe_harness.second_probe_calls, 0u);
                ++g_probe_harness.first_probe_calls;
                return true;
            }};
        const PciDriver early{
            .name = "early",
            .matches = &early_match,
            .match_count = 1,
            .probe =
                [](VirtualMemory&, PageFrameContainer&, const PciDevice&, size_t, DeviceId) {
                    EXPECT_EQ(g_probe_harness.first_probe_calls, 0u);
                    ++g_probe_harness.failing_probe_calls;
                    return false;
                },
            .probe_early = true};
        const PciDriver fallback{
            .name = "fallback", .matches = &early_match, .match_count = 1, .probe = second_probe};
        ASSERT_TRUE(driver_registry_add_pci_driver(ordinary));
        if(enable_early)
            ASSERT_TRUE(driver_registry_add_pci_driver(early));
        ASSERT_TRUE(driver_registry_add_pci_driver(fallback));
        ASSERT_TRUE(pci_bus_probe_all(kernel_vm, frames));
        EXPECT_EQ(g_probe_harness.first_probe_calls, 1u);
        EXPECT_EQ(g_probe_harness.failing_probe_calls, enable_early ? 1u : 0u);
        EXPECT_EQ(g_probe_harness.second_probe_calls, enable_early ? 0u : 1u);
    }
    platform_reset_state();
}

TEST(PciBus, LateDriverUsesSameAdmissionAndRejectsDuplicateProbe)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    auto frames = make_frames();
    kmem_init(frames);
    reset_state();
    VirtualMemory vm(frames);
    g_platform.device_count = 1;
    const PciDriver driver{.name = "late", .probe = first_probe};
    ASSERT_TRUE(driver_registry_add_pci_driver(driver));
    EXPECT_TRUE(pci_bus_probe_driver(vm, frames, driver));
    EXPECT_FALSE(pci_bus_probe_driver(vm, frames, driver));
    EXPECT_EQ(g_probe_harness.first_probe_calls, 1u);
    ASSERT_NE(device_binding_find({DeviceBus::Pci, 0}), nullptr);
    EXPECT_EQ(device_binding_find({DeviceBus::Pci, 0})->state, DeviceState::Started);
    platform_reset_state();
}
TEST(PciBus, UnprovenQuiescenceKeepsBindingAndDoesNotRelease)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    auto frames = make_frames();
    kmem_init(frames);
    reset_state();
    VirtualMemory vm(frames);
    g_platform.device_count = 1;
    const PciDriver driver{.name = "uncertain",
                           .probe = first_probe,
                           .remove = remove_driver,
                           .quiesce = [](DeviceId) { return false; }};
    ASSERT_TRUE(driver_registry_add_pci_driver(driver));
    ASSERT_TRUE(pci_bus_probe_driver(vm, frames, driver));
    EXPECT_FALSE(pci_bus_remove_device({DeviceBus::Pci, 0}));
    EXPECT_FALSE(g_remove_harness.remove_called);
    ASSERT_NE(device_binding_find({DeviceBus::Pci, 0}), nullptr);
    EXPECT_EQ(device_binding_find({DeviceBus::Pci, 0})->state, DeviceState::Failed);
    EXPECT_FALSE(pci_bus_probe_driver(vm, frames, driver));
    platform_reset_state();
}
TEST(PciBus, MissingQuiesceCallbackCannotReleaseResources)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    auto frames = make_frames();
    kmem_init(frames);
    reset_state();
    VirtualMemory vm(frames);
    g_platform.device_count = 1;
    const PciDriver driver{.name = "legacy", .probe = first_probe, .remove = remove_driver};
    ASSERT_TRUE(driver_registry_add_pci_driver(driver));
    ASSERT_TRUE(pci_bus_probe_driver(vm, frames, driver));
    EXPECT_FALSE(pci_bus_remove_device({DeviceBus::Pci, 0}));
    EXPECT_FALSE(g_remove_harness.remove_called);
    EXPECT_EQ(device_binding_find({DeviceBus::Pci, 0})->state, DeviceState::Started);
    platform_reset_state();
}

TEST(PciBus, AdmissionAllocationFailureRecordsFailedHardwareWithoutCallingDriver)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    auto frames = make_frames();
    kmem_init(frames);
    reset_state();
    VirtualMemory vm(frames);
    g_platform.device_count = 1;
    const PciDriver driver{.name = "no-memory", .probe = first_probe};
    ASSERT_TRUE(driver_registry_add_pci_driver(driver));
    std::vector<uint64_t> held;
    uint64_t page;
    while(frames.allocate(page))
        held.push_back(page);
    EXPECT_FALSE(pci_bus_probe_driver(vm, frames, driver));
    EXPECT_EQ(0u, g_probe_harness.first_probe_calls);
    EXPECT_EQ(nullptr, device_binding_find({DeviceBus::Pci, 0}));
    hardware_freeze_bindings();
    Os1HardwareQuery description{};
    description.version = 1;
    description.operation = OS1_HARDWARE_DESCRIBE;
    description.id = os1::hardware::object_id(os1::hardware::Pci, 0) + 3;
    ASSERT_EQ(0, hardware_query(description));
    EXPECT_STREQ("failed", description.nodes[0].text);
    for(auto physical : held)
        EXPECT_TRUE(frames.free(physical));
    platform_reset_state();
}

TEST(PciEnumeration, CapsDiscoveredFunctionsAndKeepsInventory)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    auto frames = make_frames();
    constexpr uint64_t ecam = 0x400000;
    constexpr uint64_t ecam_bytes = 9u << 20;
    ASSERT_TRUE(frames.reserve_range(ecam, ecam_bytes));
    std::memset(arena.data() + ecam, 0xff, ecam_bytes);
    for(size_t index = 0; index <= kPlatformMaxPciDevices; ++index)
    {
        auto* config = arena.data() + ecam + (index << 15);
        config[0] = 0x34;
        config[1] = 0x12;
        config[0x0e] = 2;  // Single function and no BAR sizing.
    }

    VirtualMemory vm(frames);
    const PciEcamRegion region{ecam, 0, 0, 8};
    std::array<PciDevice, kPlatformMaxPciDevices> devices{};
    size_t count = 0;
    ASSERT_TRUE(enumerate_pci(vm, &region, 1, devices.data(), count));
    ASSERT_EQ(kPlatformMaxPciDevices, count);
    EXPECT_EQ(0x1234u, devices.back().vendor_id);
    EXPECT_EQ(7u, devices.back().bus);
    EXPECT_EQ(31u, devices.back().slot);
}
