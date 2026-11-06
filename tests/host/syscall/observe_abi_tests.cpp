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
