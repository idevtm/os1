#include <gtest/gtest.h>
#include <os1/observe.h>
#include <stddef.h>

TEST(ObserveAbi, ConstantsAreStable)
{
    EXPECT_EQ(1, OS1_OBSERVE_ABI_VERSION);
    EXPECT_EQ(1, OS1_OBSERVE_SYSTEM);
    EXPECT_EQ(2, OS1_OBSERVE_PROCESSES);
    EXPECT_EQ(3, OS1_OBSERVE_CPUS);
    EXPECT_EQ(4, OS1_OBSERVE_PCI);
    EXPECT_EQ(5, OS1_OBSERVE_INITRD);
    EXPECT_EQ(6, OS1_OBSERVE_EVENTS);
    EXPECT_EQ(7, OS1_OBSERVE_DEVICES);
    EXPECT_EQ(8, OS1_OBSERVE_RESOURCES);
    EXPECT_EQ(9, OS1_OBSERVE_IRQS);
    EXPECT_EQ(10, OS1_OBSERVE_KMEM);
    EXPECT_EQ(11, OS1_OBSERVE_ACPI);
    EXPECT_EQ(12, OS1_OBSERVE_USB);
    EXPECT_EQ(14, OS1_OBSERVE_USB_INPUT_FAILURES);
    EXPECT_EQ(16, OS1_OBSERVE_WALL_CLOCK);
    EXPECT_EQ(256, OS1_OBSERVE_EVENT_RING_CAPACITY);
    EXPECT_EQ(32, OS1_OBSERVE_DRIVER_NAME_BYTES);
    EXPECT_EQ(32, OS1_OBSERVE_KMEM_NAME_BYTES);
    EXPECT_EQ(1, OS1_OBSERVE_RESOURCE_PCI_BAR);
    EXPECT_EQ(2, OS1_OBSERVE_RESOURCE_DMA);
    EXPECT_EQ(1, OS1_KERNEL_EVENT_TRAP);
    EXPECT_EQ(2, OS1_KERNEL_EVENT_SCHED_TRANSITION);
    EXPECT_EQ(3, OS1_KERNEL_EVENT_IRQ);
    EXPECT_EQ(4, OS1_KERNEL_EVENT_BLOCK_IO);
    EXPECT_EQ(5, OS1_KERNEL_EVENT_PCI_BIND);
    EXPECT_EQ(6, OS1_KERNEL_EVENT_USER_COPY_FAILURE);
    EXPECT_EQ(7, OS1_KERNEL_EVENT_SMOKE_MARKER);
    EXPECT_EQ(8, OS1_KERNEL_EVENT_TIMER_SOURCE);
    EXPECT_EQ(9, OS1_KERNEL_EVENT_NET_RX);
    EXPECT_EQ(10, OS1_KERNEL_EVENT_KMEM_CORRUPTION);
    EXPECT_EQ(11, OS1_KERNEL_EVENT_AP_ONLINE);
    EXPECT_EQ(12, OS1_KERNEL_EVENT_AP_TICK);
    EXPECT_EQ(13, OS1_KERNEL_EVENT_IPI_RESCHED);
    EXPECT_EQ(14, OS1_KERNEL_EVENT_KERNEL_THREAD_PING);
    EXPECT_EQ(15, OS1_KERNEL_EVENT_IPI_TLB_SHOOTDOWN);
    EXPECT_EQ(16, OS1_KERNEL_EVENT_THREAD_MIGRATE);
    EXPECT_EQ(17, OS1_KERNEL_EVENT_RUNQ_DEPTH);
    EXPECT_EQ(1, OS1_KERNEL_EVENT_TIMER_SOURCE_PIT);
    EXPECT_EQ(2, OS1_KERNEL_EVENT_TIMER_SOURCE_LAPIC);
}

TEST(ObserveAbi, HeaderLayoutIsPacked)
{
    EXPECT_EQ(16u, sizeof(Os1ObserveHeader));
    EXPECT_EQ(0u, offsetof(Os1ObserveHeader, abi_version));
    EXPECT_EQ(4u, offsetof(Os1ObserveHeader, kind));
    EXPECT_EQ(8u, offsetof(Os1ObserveHeader, record_size));
    EXPECT_EQ(12u, offsetof(Os1ObserveHeader, record_count));
}

TEST(ObserveAbi, RecordLayoutsArePacked)
{
    EXPECT_EQ(128u, sizeof(Os1ObserveSystemRecord));
    EXPECT_EQ(64u, offsetof(Os1ObserveSystemRecord, bootloader_name));

    EXPECT_EQ(68u, sizeof(Os1ObserveProcessRecord));
    EXPECT_EQ(36u, offsetof(Os1ObserveProcessRecord, name));

    EXPECT_EQ(64u, sizeof(Os1ObserveCpuRecord));
    EXPECT_EQ(12u, offsetof(Os1ObserveCpuRecord, current_pid));
    EXPECT_EQ(28u, offsetof(Os1ObserveCpuRecord, runq_depth));
    EXPECT_EQ(32u, offsetof(Os1ObserveCpuRecord, timer_ticks));
    EXPECT_EQ(56u, offsetof(Os1ObserveCpuRecord, migrate_out));

    EXPECT_EQ(17u, sizeof(Os1ObservePciBar));
    EXPECT_EQ(119u, sizeof(Os1ObservePciRecord));
    EXPECT_EQ(17u, offsetof(Os1ObservePciRecord, bars));

    EXPECT_EQ(40u, sizeof(Os1ObserveDeviceRecord));
    EXPECT_EQ(8u, offsetof(Os1ObserveDeviceRecord, driver_name));

    EXPECT_EQ(32u, sizeof(Os1ObserveResourceRecord));
    EXPECT_EQ(0u, offsetof(Os1ObserveResourceRecord, base));
    EXPECT_EQ(16u, offsetof(Os1ObserveResourceRecord, flags));
    EXPECT_EQ(28u, offsetof(Os1ObserveResourceRecord, kind));

    EXPECT_EQ(16u, sizeof(Os1ObserveIrqRecord));
    EXPECT_EQ(0u, offsetof(Os1ObserveIrqRecord, vector));
    EXPECT_EQ(12u, offsetof(Os1ObserveIrqRecord, gsi));

    EXPECT_EQ(72u, sizeof(Os1ObserveInitrdRecord));
    EXPECT_EQ(64u, offsetof(Os1ObserveInitrdRecord, size));

    EXPECT_EQ(80u, sizeof(Os1ObserveEventRecord));
    EXPECT_EQ(0u, offsetof(Os1ObserveEventRecord, sequence));
    EXPECT_EQ(8u, offsetof(Os1ObserveEventRecord, tick_count));
    EXPECT_EQ(16u, offsetof(Os1ObserveEventRecord, pid));
    EXPECT_EQ(24u, offsetof(Os1ObserveEventRecord, tid));
    EXPECT_EQ(32u, offsetof(Os1ObserveEventRecord, arg0));
    EXPECT_EQ(64u, offsetof(Os1ObserveEventRecord, type));
    EXPECT_EQ(68u, offsetof(Os1ObserveEventRecord, flags));
    EXPECT_EQ(72u, offsetof(Os1ObserveEventRecord, cpu));

    EXPECT_EQ(88u, sizeof(Os1ObserveKmemRecord));
    EXPECT_EQ(0u, offsetof(Os1ObserveKmemRecord, cache_index));
    EXPECT_EQ(4u, offsetof(Os1ObserveKmemRecord, object_size));
    EXPECT_EQ(16u, offsetof(Os1ObserveKmemRecord, slab_count));
    EXPECT_EQ(32u, offsetof(Os1ObserveKmemRecord, alloc_count));
    EXPECT_EQ(56u, offsetof(Os1ObserveKmemRecord, name));

    EXPECT_EQ(28u, sizeof(Os1ObserveAcpiRecord));
    EXPECT_EQ(0u, offsetof(Os1ObserveAcpiRecord, route_probe_count));
    EXPECT_EQ(4u, offsetof(Os1ObserveAcpiRecord, route_success_count));
    EXPECT_EQ(8u, offsetof(Os1ObserveAcpiRecord, power_probe_count));
    EXPECT_EQ(12u, offsetof(Os1ObserveAcpiRecord, power_success_count));
    EXPECT_EQ(16u, offsetof(Os1ObserveAcpiRecord, route_irq));
    EXPECT_EQ(20u, offsetof(Os1ObserveAcpiRecord, route_flags));
    EXPECT_EQ(22u, offsetof(Os1ObserveAcpiRecord, route_bus));
    EXPECT_EQ(26u, offsetof(Os1ObserveAcpiRecord, route_source_is_gsi));
}

TEST(ObserveAbi, UsbBootRecordLayout)
{
    EXPECT_EQ(64u, sizeof(Os1ObserveUsbRecord));
    EXPECT_EQ(32u, offsetof(Os1ObserveUsbRecord, sectors));
    EXPECT_EQ(40u, offsetof(Os1ObserveUsbRecord, stage));
}

TEST(ObserveAbi, UsbInputFailureLayout)
{
    EXPECT_EQ(72u, sizeof(Os1ObserveUsbInputFailure));
    EXPECT_EQ(16u, offsetof(Os1ObserveUsbInputFailure, input_device));
    EXPECT_EQ(24u, offsetof(Os1ObserveUsbInputFailure, catalog_id));
    EXPECT_EQ(56u, offsetof(Os1ObserveUsbInputFailure, token));
    EXPECT_EQ(68u, offsetof(Os1ObserveUsbInputFailure, flags));
}

#include <array>
#include <thread>
#include <vector>

#include "core/wall_clock.hpp"
#include "debug/event_ring.hpp"
#include "drivers/bus/device.hpp"
#include "drivers/usb/diagnostics.hpp"
#include "handoff/memory_layout.h"
#include "mm/dma.hpp"
#include "mm/kmem.hpp"
#include "mm/user_copy.hpp"
#include "mm/virtual_memory.hpp"
#include "platform/irq_registry.hpp"
#include "platform/state.hpp"
#include "proc/thread.hpp"
#include "support/physical_memory.hpp"
#include "syscall/observe.hpp"

class UsbInputObserve : public ::testing::Test
{
protected:
    os1::host_test::PhysicalMemoryArena arena{32ull * 1024 * 1024};
    PageFrameContainer frames;
    Thread* thread{};
    void SetUp() override
    {
        usb::diagnostics::reset_for_test();
        std::array<BootMemoryRegion, 1> regions{{{0, arena.size(), BootMemoryType::Usable, 0}}};
        ASSERT_TRUE(frames.initialize(regions, 0x300000, kPageFrameBitmapQwordLimit));
        kmem_init(frames);
        ASSERT_TRUE(init_tasks(frames));
        VirtualMemory vm(frames);
        ASSERT_TRUE(vm.allocate_and_map(kUserImageBase, 1, PageFlags::User | PageFlags::Write));
        auto* process = create_user_process("usb-observe", vm.root());
        ASSERT_NE(nullptr, process);
        thread = create_user_thread(process, kUserImageBase, kUserImageBase + 4096, frames, false);
        ASSERT_NE(nullptr, thread);
        set_current_thread(thread);
    }
    void TearDown() override
    {
        set_current_thread(nullptr);
        usb::diagnostics::reset_for_test();
    }
};
TEST_F(UsbInputObserve, ReadsWallClockWithoutLogAuthority)
{
    const ObserveContext context{.frames = &frames};
    struct Reset
    {
        ~Reset()
        {
            wall_clock_initialize(0);
        }
    } reset;
    Os1ObserveWallClock sample{};
    const auto read = [&](uint64_t address, size_t bytes = sizeof(sample)) {
        return sys_observe(context, OS1_OBSERVE_WALL_CLOCK, address, bytes);
    };
    wall_clock_initialize(0);
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, read(kUserImageBase));
    constexpr uint64_t instant = 1794096000123456789ull;
    wall_clock_initialize(instant);
    EXPECT_EQ(OS1_ERR_INVALID, read(kUserImageBase, sizeof(sample) - 1));
    EXPECT_EQ(OS1_ERR_FAULT, read(kUserImageBase + 4096));
    EXPECT_EQ(sizeof(sample), read(kUserImageBase));
    ASSERT_TRUE(copy_from_user(frames, thread, kUserImageBase, &sample, sizeof(sample)));
    EXPECT_EQ(instant, sample.unix_ns);
}

TEST_F(UsbInputObserve, CopiesRetainedFailureAndRejectsShortOrUnmappedBuffers)
{
    const ObserveContext context{.frames = &frames};
    EXPECT_EQ(16, sys_observe(context, OS1_OBSERVE_USB_INPUT_FAILURES, kUserImageBase, 4096));
    Os1ObserveUsbInputFailure failure{};
    failure.input_device = 7;
    failure.catalog_id = 1376640;
    failure.reason = OS1_USB_INPUT_TRANSFER;
    failure.token = 0x30d52;
    usb::diagnostics::input_failure(failure);
    EXPECT_EQ(88, sys_observe(context, OS1_OBSERVE_USB_INPUT_FAILURES, kUserImageBase, 4096));
    Os1ObserveHeader header{};
    ASSERT_TRUE(copy_from_user(frames, thread, kUserImageBase, &header, sizeof(header)));
    EXPECT_EQ(14u, header.kind);
    EXPECT_EQ(1u, header.record_count);
    EXPECT_EQ(72u, header.record_size);
    Os1ObserveUsbInputFailure copied{};
    ASSERT_TRUE(copy_from_user(frames, thread, kUserImageBase + 16, &copied, sizeof(copied)));
    EXPECT_EQ(7u, copied.input_device);
    EXPECT_EQ(0x30d52u, copied.token);
    EXPECT_EQ(-1, sys_observe(context, OS1_OBSERVE_USB_INPUT_FAILURES, kUserImageBase, 87));
    EXPECT_EQ(-1,
              sys_observe(context, OS1_OBSERVE_USB_INPUT_FAILURES, kUserImageBase + 4096, 4096));
    EXPECT_EQ(1u, usb::diagnostics::input_failure_count());  // Reads do not consume evidence.
}

TEST_F(UsbInputObserve, LiveSnapshotsRejectShortBuffersWithoutWriting)
{
    platform_reset_state();
    const DeviceId owner{DeviceBus::Platform, 7};
    ASSERT_TRUE(device_binding_publish(owner, 0, "snapshot-test", nullptr));
    ASSERT_TRUE(platform_register_isa_irq_route(owner, 5, 5, 0, 37));
    DmaBuffer dma{};
    ASSERT_TRUE(dma_allocate_buffer(frames, owner, 128, DmaDirection::Bidirectional, dma));

    const ObserveContext context{.frames = &frames};
    for(const auto [kind, size] : std::array<std::pair<uint32_t, size_t>, 4>{{
            {OS1_OBSERVE_PROCESSES, sizeof(Os1ObserveProcessRecord)},
            {OS1_OBSERVE_DEVICES, sizeof(Os1ObserveDeviceRecord)},
            {OS1_OBSERVE_RESOURCES, sizeof(Os1ObserveResourceRecord)},
            {OS1_OBSERVE_IRQS, sizeof(Os1ObserveIrqRecord)},
        }})
    {
        const auto bytes = sizeof(Os1ObserveHeader) + size;
        std::array<uint8_t, 100> canary;
        canary.fill(0xa5);
        ASSERT_TRUE(copy_to_user(frames, thread, kUserImageBase, canary.data(), canary.size()));
        EXPECT_EQ(-1, sys_observe(context, kind, kUserImageBase, bytes - 1));
        std::array<uint8_t, 100> observed{};
        ASSERT_TRUE(
            copy_from_user(frames, thread, kUserImageBase, observed.data(), observed.size()));
        EXPECT_EQ(canary, observed);
        EXPECT_EQ(static_cast<long>(bytes), sys_observe(context, kind, kUserImageBase, bytes));
        Os1ObserveHeader header{};
        ASSERT_TRUE(copy_from_user(frames, thread, kUserImageBase, &header, sizeof(header)));
        EXPECT_EQ(kind, header.kind);
        EXPECT_EQ(1u, header.record_count);
        EXPECT_EQ(size, header.record_size);
        uint8_t after = 0;
        ASSERT_TRUE(copy_from_user(frames, thread, kUserImageBase + bytes, &after, sizeof(after)));
        EXPECT_EQ(0xa5, after);
    }

    dma_release_buffer(frames, dma);
    EXPECT_TRUE(platform_release_irq_route(37));
    device_binding_remove(owner);
    EXPECT_EQ(
        sizeof(Os1ObserveHeader),
        sys_observe(context, OS1_OBSERVE_RESOURCES, kUserImageBase, sizeof(Os1ObserveHeader)));
    EXPECT_EQ(sizeof(Os1ObserveHeader),
              sys_observe(context, OS1_OBSERVE_IRQS, kUserImageBase, sizeof(Os1ObserveHeader)));
    EXPECT_EQ(sizeof(Os1ObserveHeader),
              sys_observe(context, OS1_OBSERVE_DEVICES, kUserImageBase, sizeof(Os1ObserveHeader)));
    platform_reset_state();
}

TEST(ObserveEvents, CallerOwnedSnapshotsSurviveConcurrentPublishing)
{
    using namespace kernel_event;
    record_subject(OS1_KERNEL_EVENT_SMOKE_MARKER, 0, 0, 0);
    std::array<Os1ObserveEventRecord, OS1_OBSERVE_EVENT_RING_CAPACITY> first{}, second{};
    uint32_t first_count = 0;
    ASSERT_TRUE(snapshot_all(first.data(), first.size(), first_count));
    const auto first_sequence = first[first_count - 1].sequence;
    std::thread writer([] {
        for(unsigned i = 0; i < OS1_OBSERVE_EVENT_RING_CAPACITY * 2; ++i)
            record_subject(OS1_KERNEL_EVENT_SMOKE_MARKER, 0, 0, 0);
    });
    bool coherent = true;
    uint32_t count = 0;
    for(unsigned i = 0; i < 64; ++i)
    {
        coherent &= snapshot_all(second.data(), second.size(), count);
        for(uint32_t index = 1; index < count; ++index)
            coherent &= second[index].sequence == second[index - 1].sequence + 1;
    }
    writer.join();
    EXPECT_TRUE(coherent);
    EXPECT_EQ(first_sequence, first[first_count - 1].sequence);
    second[0].sequence = 0;
    EXPECT_FALSE(snapshot_all(second.data(), 1, count));
    EXPECT_GT(count, 1u);
    EXPECT_EQ(0u, second[0].sequence);
}

extern void (*host_observe_snapshot_hook)(uint32_t, void*);
extern void* host_observe_snapshot_state;

namespace
{
class ObserveCheckpoint
{
public:
    ObserveCheckpoint(void (*hook)(uint32_t, void*), void* state)
    {
        host_observe_snapshot_state = state;
        host_observe_snapshot_hook = hook;
    }
    ~ObserveCheckpoint()
    {
        host_observe_snapshot_hook = nullptr;
        host_observe_snapshot_state = nullptr;
    }
};
}  // namespace

TEST_F(UsbInputObserve, RegistryGrowthAfterSizingRetriesWithFreshCapacity)
{
    platform_reset_state();
    struct Changes
    {
        PageFrameContainer& frames;
        DmaBuffer dma{};
        bool ok = false;
        unsigned attempts = 0;
    } changes{frames};
    const DeviceId owner{DeviceBus::Platform, 8};
    const ObserveContext context{.frames = &frames};

    for(uint32_t kind :
        {OS1_OBSERVE_PROCESSES, OS1_OBSERVE_DEVICES, OS1_OBSERVE_RESOURCES, OS1_OBSERVE_IRQS})
    {
        const long initial = sys_observe(context, kind, kUserImageBase, 4096);
        ASSERT_GE(initial, static_cast<long>(sizeof(Os1ObserveHeader)));
        Os1ObserveHeader before{};
        ASSERT_TRUE(copy_from_user(frames, thread, kUserImageBase, &before, sizeof(before)));
        changes.ok = false;
        changes.attempts = 0;
        ObserveCheckpoint checkpoint(
            [](uint32_t selected, void* state) {
                auto& changes = *static_cast<Changes*>(state);
                if(changes.attempts++ != 0)
                    return;
                const DeviceId owner{DeviceBus::Platform, 8};
                switch(selected)
                {
                    case OS1_OBSERVE_PROCESSES:
                        changes.ok = create_user_thread(current_thread()->process,
                                                        kUserImageBase,
                                                        kUserStackTop,
                                                        changes.frames,
                                                        false) != nullptr;
                        break;
                    case OS1_OBSERVE_DEVICES:
                        changes.ok = device_binding_publish(owner, 0, "growth", nullptr);
                        break;
                    case OS1_OBSERVE_RESOURCES:
                        changes.ok = dma_allocate_buffer(
                            changes.frames, owner, 128, DmaDirection::Bidirectional, changes.dma);
                        break;
                    case OS1_OBSERVE_IRQS:
                        changes.ok = platform_register_isa_irq_route(owner, 5, 5, 0, 37);
                        break;
                }
            },
            &changes);
        EXPECT_EQ(initial + before.record_size, sys_observe(context, kind, kUserImageBase, 4096));
        EXPECT_TRUE(changes.ok);
        EXPECT_EQ(2u, changes.attempts);
        Os1ObserveHeader after{};
        ASSERT_TRUE(copy_from_user(frames, thread, kUserImageBase, &after, sizeof(after)));
        EXPECT_EQ(before.record_count + 1, after.record_count);
        EXPECT_EQ(before.record_size, after.record_size);
        EXPECT_EQ(kind, after.kind);
    }

    dma_release_buffer(frames, changes.dma);
    EXPECT_TRUE(platform_release_irq_route(37));
    device_binding_remove(owner);
    platform_reset_state();
}

TEST_F(UsbInputObserve, SnapshotRetryStopsWhenRegistryKeepsGrowingOrBufferIsSmall)
{
    platform_reset_state();
    const DeviceId owner{DeviceBus::Platform, 8};
    for(uint8_t vector = 64; vector < 104; ++vector)
        ASSERT_TRUE(platform_register_local_apic_irq_route(owner, vector, vector));

    std::array<uint8_t, kPageSize> canary, observed;
    canary.fill(0xa5);
    const ObserveContext context{.frames = &frames};
    ASSERT_TRUE(copy_to_user(frames, thread, kUserImageBase, canary.data(), canary.size()));
    struct Growth
    {
        PageFrameContainer& frames;
        uint64_t free_pages;
        unsigned attempts = 0;
    } growth{frames, frames.free_page_count()};
    {
        ObserveCheckpoint checkpoint(
            [](uint32_t kind, void* state) {
                EXPECT_EQ(OS1_OBSERVE_IRQS, kind);
                auto& growth = *static_cast<Growth*>(state);
                EXPECT_EQ(growth.free_pages - (growth.attempts < 2),
                          growth.frames.free_page_count());
                const uint8_t irq = 5 + growth.attempts++;
                EXPECT_TRUE(platform_register_isa_irq_route(
                    {DeviceBus::Platform, 8}, irq, irq, 0, 32 + irq));
            },
            &growth);
        EXPECT_EQ(-1, sys_observe(context, OS1_OBSERVE_IRQS, kUserImageBase, canary.size()));
        EXPECT_EQ(2u, growth.attempts);
        EXPECT_EQ(-1,
                  sys_observe(context, OS1_OBSERVE_IRQS, kUserImageBase, sizeof(Os1ObserveHeader)));
        EXPECT_EQ(3u, growth.attempts);
    }
    EXPECT_EQ(growth.free_pages, frames.free_page_count());
    ASSERT_TRUE(copy_from_user(frames, thread, kUserImageBase, observed.data(), observed.size()));
    EXPECT_EQ(canary, observed);
    platform_release_irq_routes_for_owner(owner);
    platform_reset_state();
}

TEST_F(UsbInputObserve, ProcessSnapshotReservesCr3AsZeroForEveryRow)
{
    auto* peer = create_user_process("peer-observe", 0x200000);
    ASSERT_NE(nullptr, peer);
    auto* peer_thread = create_user_thread(peer, kUserImageBase, kUserStackTop, frames, false);
    ASSERT_NE(nullptr, peer_thread);
    ASSERT_NE(0u, thread->address_space_cr3);
    ASSERT_NE(0u, peer_thread->address_space_cr3);

    const ObserveContext context{.frames = &frames};
    constexpr auto bytes = sizeof(Os1ObserveHeader) + 2 * sizeof(Os1ObserveProcessRecord);
    ASSERT_EQ(bytes, sys_observe(context, OS1_OBSERVE_PROCESSES, kUserImageBase, bytes));
    std::array<Os1ObserveProcessRecord, 2> rows{};
    ASSERT_TRUE(copy_from_user(
        frames, thread, kUserImageBase + sizeof(Os1ObserveHeader), rows.data(), sizeof(rows)));
    EXPECT_EQ(thread->process->pid, rows[0].pid);
    EXPECT_EQ(peer->pid, rows[1].pid);
    EXPECT_EQ(0u, rows[0].cr3);
    EXPECT_EQ(0u, rows[1].cr3);
}

TEST_F(UsbInputObserve, EventSnapshotDisclosesOnlyOwnUserFaultAddresses)
{
    const auto pid = thread->process->pid;
    constexpr auto address = kUserImageBase + 0x123;
    constexpr auto fault = kUserImageBase + 0x456;
    constexpr auto kernel_address = 0xffffffff80000123ull;
    constexpr auto trap = OS1_KERNEL_EVENT_TRAP;
    constexpr auto user = OS1_KERNEL_EVENT_FLAG_USER;
    constexpr auto copy = OS1_KERNEL_EVENT_USER_COPY_FAILURE;
    constexpr auto reason = OS1_KERNEL_EVENT_USER_COPY_TRANSLATE;
    const auto emit = [&](uint32_t type,
                          uint32_t flags,
                          uint64_t subject,
                          uint64_t arg0,
                          uint64_t arg1,
                          uint64_t arg2,
                          uint64_t arg3) {
        kernel_event::record_subject(type, flags, subject, thread->tid, arg0, arg1, arg2, arg3);
    };
    emit(trap, user, pid, 14, 5, address, fault);
    emit(trap, user, pid + 1, 14, 5, address, fault);
    emit(trap, 0, pid, 14, 5, kernel_address, kernel_address);
    emit(trap, user, pid, 13, 0, address, fault);
    emit(trap, user, pid, 14, 5, address, kernel_address);
    emit(copy, 0, pid, address, 20, reason, 7);
    emit(copy, 0, pid + 1, address, 20, reason, 7);
    emit(copy, 0, 0, address, 20, reason, 7);
    emit(copy, 0, pid, kernel_address, 20, reason, 7);
    emit(OS1_KERNEL_EVENT_SMOKE_MARKER, 0, 0, 1, 2, 3, 4);
    constexpr auto expected = std::array{
        std::array<uint64_t, 4>{14, 5, address, fault},
        std::array<uint64_t, 4>{14, 5, 0, 0},
        std::array<uint64_t, 4>{14, 5, 0, 0},
        std::array<uint64_t, 4>{13, 0, address, 0},
        std::array<uint64_t, 4>{14, 5, address, 0},
        std::array<uint64_t, 4>{address, 20, reason, 7},
        std::array<uint64_t, 4>{0, 20, reason, 0},
        std::array<uint64_t, 4>{0, 20, reason, 0},
        std::array<uint64_t, 4>{0, 20, reason, 0},
        std::array<uint64_t, 4>{1, 2, 3, 4},
    };

    constexpr auto capacity = OS1_OBSERVE_EVENT_RING_CAPACITY;
    constexpr auto bytes = sizeof(Os1ObserveHeader) + capacity * sizeof(Os1ObserveEventRecord);
    VirtualMemory vm(frames, thread->address_space_cr3);
    ASSERT_TRUE(
        vm.allocate_and_map(kUserImageBase + kPageSize, 5, PageFlags::User | PageFlags::Write));
    std::array<Os1ObserveEventRecord, capacity> raw{};
    uint32_t count = 0;
    ASSERT_TRUE(kernel_event::snapshot_all(raw.data(), capacity, count));
    ASSERT_GE(count, expected.size());

    const ObserveContext context{.frames = &frames};
    ASSERT_EQ(sizeof(Os1ObserveHeader) + count * sizeof(Os1ObserveEventRecord),
              sys_observe(context, OS1_OBSERVE_EVENTS, kUserImageBase, bytes));
    for(size_t i = 0; i < expected.size(); ++i)
    {
        const auto index = count - expected.size() + i;
        const auto offset = sizeof(Os1ObserveHeader) + index * sizeof(Os1ObserveEventRecord);
        Os1ObserveEventRecord row{};
        ASSERT_TRUE(copy_from_user(frames, thread, kUserImageBase + offset, &row, sizeof(row)));
        EXPECT_EQ(raw[index].sequence, row.sequence);
        EXPECT_EQ(raw[index].type, row.type);
        EXPECT_EQ(raw[index].pid, row.pid);
        EXPECT_EQ(expected[i], (std::array{row.arg0, row.arg1, row.arg2, row.arg3})) << i;
    }

    uint32_t after = 0;
    ASSERT_TRUE(kernel_event::snapshot_all(raw.data(), capacity, after));
    EXPECT_EQ(count, after);
    EXPECT_EQ(fault, raw[count - expected.size() + 1].arg3);
    EXPECT_EQ(address, raw[count - expected.size() + 6].arg0);
}

TEST_F(UsbInputObserve, EventSyscallKeepsCapturedRecordsDuringOverlappingRead)
{
    constexpr auto bytes =
        sizeof(Os1ObserveHeader) + sizeof(Os1ObserveEventRecord) * OS1_OBSERVE_EVENT_RING_CAPACITY;
    constexpr auto second = kUserImageBase + 6 * kPageSize;
    VirtualMemory vm(frames, thread->address_space_cr3);
    ASSERT_TRUE(
        vm.allocate_and_map(kUserImageBase + kPageSize, 11, PageFlags::User | PageFlags::Write));
    for(unsigned i = 0; i < OS1_OBSERVE_EVENT_RING_CAPACITY; ++i)
        kernel_event::record_subject(OS1_KERNEL_EVENT_SMOKE_MARKER, 0, 0, 0);

    const ObserveContext context{.frames = &frames};
    struct Overlap
    {
        ObserveContext context;
        long result = -1;
    } overlap{context};
    ObserveCheckpoint checkpoint(
        [](uint32_t kind, void* state) {
            auto& overlap = *static_cast<Overlap*>(state);
            if(kind != OS1_OBSERVE_EVENTS)
                return;
            host_observe_snapshot_hook = nullptr;
            for(unsigned i = 0; i < OS1_OBSERVE_EVENT_RING_CAPACITY; ++i)
                kernel_event::record_subject(OS1_KERNEL_EVENT_SMOKE_MARKER, 0, 0, 0);
            std::thread reader([&] {
                overlap.result = sys_observe(overlap.context, OS1_OBSERVE_EVENTS, second, bytes);
            });
            reader.join();
        },
        &overlap);
    const auto free_before = frames.free_page_count();
    ASSERT_EQ(bytes, sys_observe(context, OS1_OBSERVE_EVENTS, kUserImageBase, bytes));
    ASSERT_EQ(bytes, overlap.result);
    EXPECT_EQ(free_before, frames.free_page_count());

    Os1ObserveHeader first_header{}, second_header{};
    ASSERT_TRUE(
        copy_from_user(frames, thread, kUserImageBase, &first_header, sizeof(first_header)));
    ASSERT_TRUE(copy_from_user(frames, thread, second, &second_header, sizeof(second_header)));
    EXPECT_EQ(OS1_OBSERVE_EVENT_RING_CAPACITY, first_header.record_count);
    EXPECT_EQ(first_header.record_count, second_header.record_count);
    uint64_t first_sequence = 0, second_sequence = 0;
    for(uint32_t i = 0; i < OS1_OBSERVE_EVENT_RING_CAPACITY; ++i)
    {
        const auto offset = sizeof(Os1ObserveHeader) + i * sizeof(Os1ObserveEventRecord);
        Os1ObserveEventRecord a{}, b{};
        ASSERT_TRUE(copy_from_user(frames, thread, kUserImageBase + offset, &a, sizeof(a)));
        ASSERT_TRUE(copy_from_user(frames, thread, second + offset, &b, sizeof(b)));
        if(i == 0)
        {
            first_sequence = a.sequence;
            second_sequence = b.sequence;
        }
        EXPECT_EQ(first_sequence + i, a.sequence);
        EXPECT_EQ(second_sequence + i, b.sequence);
    }
    EXPECT_EQ(first_sequence + OS1_OBSERVE_EVENT_RING_CAPACITY, second_sequence);
}

TEST_F(UsbInputObserve, EventSnapshotAllocationFailureDoesNotWriteHeader)
{
    const ObserveContext context{.frames = &frames};
    const uint64_t canary = 0xa5a5a5a5a5a5a5a5ull;
    ASSERT_TRUE(copy_to_user(frames, thread, kUserImageBase, &canary, sizeof(canary)));
    std::vector<uint64_t> held;
    uint64_t physical = 0;
    while(frames.allocate(physical))
        held.push_back(physical);
    EXPECT_EQ(-1,
              sys_observe(context,
                          OS1_OBSERVE_EVENTS,
                          kUserImageBase,
                          sizeof(Os1ObserveHeader) + 256 * sizeof(Os1ObserveEventRecord)));
    for(auto page : held)
        EXPECT_TRUE(frames.free(page));
    uint64_t observed = 0;
    ASSERT_TRUE(copy_from_user(frames, thread, kUserImageBase, &observed, sizeof(observed)));
    EXPECT_EQ(canary, observed);
}
