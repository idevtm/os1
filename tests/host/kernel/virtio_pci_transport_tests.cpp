#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <cstring>

#include "core/kernel_state.hpp"
#include "drivers/module/virtio_host.hpp"
#include "drivers/timer/tsc_clock.hpp"
#include "drivers/virtio/pci_transport.hpp"
#include "handoff/memory_layout.h"
#include "mm/dma.hpp"
#include "mm/kmem.hpp"
#include "mm/virtual_memory.hpp"
#include "platform/pci_config.hpp"
#include "platform/state.hpp"
#include "support/physical_memory.hpp"

uint64_t g_kernel_root_cr3 = ~0ull;

uint64_t tsc_clock_now_100ns()
{
    return 0;
}

namespace
{
constexpr uint64_t kArenaBytes = 16ull * 1024 * 1024;
constexpr uint64_t kConfig = 0x600000;
constexpr uint64_t kBar = 0x800000;
constexpr uint32_t kBarBytes = 4096;
constexpr uint8_t kCommon = 0x40;
constexpr uint8_t kNotify = 0x54;
constexpr uint8_t kDevice = 0x68;
constexpr uint8_t kIsr = 0x7c;
constexpr DeviceId kOwner{DeviceBus::Pci, 0};
constexpr std::array<BootMemoryRegion, 1> kRegions{{
    {0, kArenaBytes, BootMemoryType::Usable, 0},
}};
constexpr uint64_t kBitmapPhysical = 0x300000;

class VirtioPciTransportTest : public testing::Test
{
protected:
    os1::host_test::PhysicalMemoryArena arena{kArenaBytes};
    PageFrameContainer frames;
    VirtioPciTransport transport{};
    PciDevice& device = g_platform.devices[0];

    void SetUp() override
    {
        ASSERT_TRUE(frames.initialize(kRegions, kBitmapPhysical, kPageFrameBitmapQwordLimit));
        kmem_init(frames);
        platform_reset_state();
        g_platform.device_count = 1;
        device.config_physical = kConfig;
        device.capability_pointer = kCommon;
        device.bars[0].base = kBar;
        device.bars[0].size = kBarBytes;
        device.bars[0].type = PciBarType::Mmio32;
        set_capability(kCommon, kNotify, kVirtioPciCapCommonCfg, 0, sizeof(VirtioPciCommonCfg));
        set_capability(kNotify, kDevice, kVirtioPciCapNotifyCfg, 0x100, 8);
        pci_config_write32(device, kNotify + sizeof(VirtioPciCapability), 4);
        set_capability(kDevice, kIsr, kVirtioPciCapDeviceCfg, 0x200, 8);
        set_capability(kIsr, 0, kVirtioPciCapIsrCfg, kBarBytes - 1, 1);
    }

    void TearDown() override
    {
        platform_reset_state();
    }

    void set_capability(
        uint8_t at, uint8_t next, uint8_t type, uint32_t offset, uint32_t bytes, uint8_t bar = 0)
    {
        VirtioPciCapability capability{};
        capability.cap_vndr = 0x09;
        capability.cap_next = next;
        capability.cap_len =
            sizeof(capability) + (type == kVirtioPciCapNotifyCfg ? sizeof(uint32_t) : 0);
        capability.cfg_type = type;
        capability.bar = bar;
        capability.offset = offset;
        capability.length = bytes;
        std::memcpy(kernel_physical_pointer<void>(kConfig + at), &capability, sizeof(capability));
    }
};

TEST_F(VirtioPciTransportTest, RejectsWrappedNotificationRegion)
{
    VirtualMemory vm(frames);
    set_capability(kNotify, kDevice, kVirtioPciCapNotifyCfg, UINT32_MAX - 7u, 16);
    EXPECT_FALSE(virtio_pci_bind_transport(vm, kOwner, device, 8, transport));
    EXPECT_EQ(pci_read_command(device), 0u);
}

TEST_F(VirtioPciTransportTest, RejectsZeroLengthAndOutOfBarIsr)
{
    VirtualMemory vm(frames);
    set_capability(kIsr, 0, kVirtioPciCapIsrCfg, kBarBytes, 0);
    EXPECT_FALSE(virtio_pci_bind_transport(vm, kOwner, device, 8, transport));
    EXPECT_EQ(pci_read_command(device), 0u);
    set_capability(kIsr, 0, kVirtioPciCapIsrCfg, UINT32_MAX, 1);
    EXPECT_FALSE(virtio_pci_bind_transport(vm, kOwner, device, 8, transport));
    EXPECT_EQ(pci_read_command(device), 0u);
}

TEST_F(VirtioPciTransportTest, RejectsQueueRegisterOutsideNotificationRegionBeforeEnabling)
{
    VirtualMemory vm(frames);
    struct Sample
    {
        uint32_t multiplier;
        uint32_t length;
        uint16_t queue_offset;
    };
    constexpr Sample samples[]{{4, 8, 2}, {0x80000000u, 8, 2}, {2, 7, 3}};
    for(const auto& sample : samples)
    {
        set_capability(kNotify, kDevice, kVirtioPciCapNotifyCfg, 0x100, sample.length);
        pci_config_write32(device, kNotify + sizeof(VirtioPciCapability), sample.multiplier);
        ASSERT_TRUE(virtio_pci_bind_transport(vm, kOwner, device, 8, transport));
        transport.common_cfg->queue_size = 16;
        transport.common_cfg->queue_notify_off = sample.queue_offset;
        EXPECT_FALSE(virtio_pci_setup_queue(transport, 0, 8, 0x1000, 0x2000, 0x3000));
        EXPECT_EQ(transport.common_cfg->queue_size, 16u);
        EXPECT_EQ(transport.common_cfg->queue_enable, 0u);
        EXPECT_EQ(transport.common_cfg->queue_desc, 0u);
    }
}

TEST_F(VirtioPciTransportTest, IgnoresReservedBarCapabilityWithUsableAlternative)
{
    VirtualMemory vm(frames);
    constexpr uint8_t reserved = 0x90;
    set_capability(kCommon, reserved, kVirtioPciCapCommonCfg, 0, sizeof(VirtioPciCommonCfg));
    set_capability(reserved, kNotify, kVirtioPciCapNotifyCfg, 0, 0, 6);
    ASSERT_TRUE(virtio_pci_bind_transport(vm, kOwner, device, 8, transport));
    EXPECT_EQ(transport.notify_base, kBar + 0x100);

    set_capability(kCommon, kNotify, kVirtioPciCapCommonCfg, 0, sizeof(VirtioPciCommonCfg));
    set_capability(kNotify, reserved, kVirtioPciCapNotifyCfg, 0x100, 8);
    set_capability(reserved, kDevice, kVirtioPciCapNotifyCfg, 0, 0, 7);
    ASSERT_TRUE(virtio_pci_bind_transport(vm, kOwner, device, 8, transport));
    EXPECT_EQ(transport.notify_base, kBar + 0x100);
}

TEST_F(VirtioPciTransportTest, RejectsNotificationCapabilityWithoutMultiplierField)
{
    VirtualMemory vm(frames);
    *kernel_physical_pointer<uint8_t>(kConfig + kNotify + 2u) = sizeof(VirtioPciCapability);
    EXPECT_FALSE(virtio_pci_bind_transport(vm, kOwner, device, 8, transport));
    EXPECT_EQ(pci_read_command(device), 0u);
}

TEST_F(VirtioPciTransportTest, CachesDistinctQueueRegistersWithoutReReadingDeviceOffset)
{
    VirtualMemory vm(frames);
    pci_config_write32(device, kNotify + sizeof(VirtioPciCapability), 2);
    ASSERT_TRUE(virtio_pci_bind_transport(vm, kOwner, device, 8, transport));
    EXPECT_EQ(transport.isr_status,
              kernel_physical_pointer<volatile uint8_t>(kBar + kBarBytes - 1));
    transport.common_cfg->queue_size = 16;
    auto* first = virtio_pci_setup_queue(transport, 0, 8, 0x1000, 0x2000, 0x3000);
    ASSERT_EQ(first, kernel_physical_pointer<volatile uint16_t>(kBar + 0x100));
    transport.common_cfg->queue_notify_off = 3;
    auto* second = virtio_pci_setup_queue(transport, 1, 8, 0x4000, 0x5000, 0x6000);
    ASSERT_EQ(second, kernel_physical_pointer<volatile uint16_t>(kBar + 0x106));

    *first = 0xffff;
    transport.common_cfg->queue_notify_off = UINT16_MAX;
    virtio_pci_notify_queue(first, 0);
    virtio_pci_notify_queue(second, 1);
    EXPECT_EQ(*first, 0u);
    EXPECT_EQ(*second, 1u);
    EXPECT_EQ(transport.common_cfg->queue_select, 1u);
}

TEST_F(VirtioPciTransportTest, ZeroMultiplierSharesNotificationRegister)
{
    VirtualMemory vm(frames);
    pci_config_write32(device, kNotify + sizeof(VirtioPciCapability), 0);
    ASSERT_TRUE(virtio_pci_bind_transport(vm, kOwner, device, 8, transport));
    transport.common_cfg->queue_size = 16;
    transport.common_cfg->queue_notify_off = UINT16_MAX;
    auto* notify = virtio_pci_setup_queue(transport, 1, 8, 0x1000, 0x2000, 0x3000);
    ASSERT_EQ(notify, kernel_physical_pointer<volatile uint16_t>(kBar + 0x100));
    virtio_pci_notify_queue(notify, 1);
    EXPECT_EQ(*notify, 1u);
}

void verify_rejected_module_queue_cleanup()
{
    ASSERT_TRUE(page_frames.initialize(kRegions, kBitmapPhysical, kPageFrameBitmapQwordLimit));
    kmem_init(page_frames);
    auto* common = kernel_physical_pointer<VirtioPciCommonCfg>(kBar);
    common->queue_size = 16;
    common->queue_notify_off = 2;
    const auto& api = driver_host_api();
    void* handle = api.open(kOwner.index, 8, 0);
    ASSERT_NE(handle, nullptr);
    const auto active_before = dma_allocation_count();
    for(unsigned attempt = 0; attempt < 2; ++attempt)
    {
        uint32_t capacity = 0;
        EXPECT_EQ(api.queue(handle, 0, 1, &capacity), nullptr);
        EXPECT_EQ(capacity, 0u);
        EXPECT_EQ(common->queue_enable, 0u);
        EXPECT_EQ(common->queue_desc, 0u);
        EXPECT_EQ(dma_allocation_count(), active_before);
    }
    EXPECT_EQ(api.stop(handle), 1);
}

TEST_F(VirtioPciTransportTest, FailedModuleQueueSetupReleasesUnpublishedDmaOnEveryAttempt)
{
    // The module and kernel allocator admit initialization once per process.
    ASSERT_EXIT(
        {
            verify_rejected_module_queue_cleanup();
            std::_Exit(HasFailure() ? EXIT_FAILURE : EXIT_SUCCESS);
        },
        testing::ExitedWithCode(EXIT_SUCCESS),
        "");
}
}  // namespace
