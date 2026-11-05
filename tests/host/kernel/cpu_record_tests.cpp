#include <gtest/gtest.h>

#ifdef assert
#undef assert
#endif

#include <array>

#include "arch/x86_64/cpu/cpu.hpp"
#include "arch/x86_64/cpu/exception_stack.hpp"
#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "proc/thread.hpp"
#include "support/physical_memory.hpp"

#ifdef static_assert
#undef static_assert
#endif

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

void idle_entry()
{
}

struct HostCpuScope
{
    explicit HostCpuScope(cpu* ap) : saved_next(g_cpu_boot->next), saved_current(g_cpu_host_current)
    {
        g_cpu_boot->next = ap;
        g_cpu_host_current = g_cpu_boot;
    }

    ~HostCpuScope()
    {
        g_cpu_boot->next = saved_next;
        g_cpu_boot->idle_thread = nullptr;
        g_cpu_host_current = saved_current;
    }

    cpu* saved_next;
    cpu* saved_current;
};
}  // namespace

TEST(CpuRecord, IdleThreadPointersAreCpuLocal)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);

    cpu ap_cpu{};
    ap_cpu.self = &ap_cpu;
    ap_cpu.id = 1;
    ap_cpu.magic = CPU_MAGIC;

    HostCpuScope cpu_scope(&ap_cpu);
    ASSERT_TRUE(init_tasks(frames));
    Process* kernel_process = create_kernel_process(0);
    ASSERT_NE(nullptr, kernel_process);

    Thread* bsp_idle = create_idle_thread_for_cpu(kernel_process, g_cpu_boot, idle_entry, frames);
    Thread* ap_idle = create_idle_thread_for_cpu(kernel_process, &ap_cpu, idle_entry, frames);

    ASSERT_NE(nullptr, bsp_idle);
    ASSERT_NE(nullptr, ap_idle);
    EXPECT_NE(bsp_idle, ap_idle);
    EXPECT_EQ(ThreadState::Ready, bsp_idle->state);
    EXPECT_EQ(ThreadState::Blocked, ap_idle->state);
    EXPECT_EQ(bsp_idle, idle_thread_for_cpu(g_cpu_boot));
    EXPECT_EQ(ap_idle, idle_thread_for_cpu(&ap_cpu));

    g_cpu_host_current = g_cpu_boot;
    EXPECT_EQ(bsp_idle, idle_thread());

    g_cpu_host_current = &ap_cpu;
    EXPECT_EQ(ap_idle, idle_thread());
}

TEST(CpuRecord, RunQueueStartsEmptyOnConstructedRecords)
{
    cpu record{};

    EXPECT_EQ(nullptr, record.runq.head);
    EXPECT_EQ(nullptr, record.runq.tail);
    EXPECT_EQ(0u, record.runq.length);
    EXPECT_FALSE(record.runq.lock.locked());
}

TEST(CpuRecord, BspSchedulerIgnoresIdleThreadsOwnedByOtherCpus)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);

    cpu ap_cpu{};
    ap_cpu.self = &ap_cpu;
    ap_cpu.id = 1;
    ap_cpu.magic = CPU_MAGIC;

    HostCpuScope cpu_scope(&ap_cpu);
    ASSERT_TRUE(init_tasks(frames));
    Process* kernel_process = create_kernel_process(0);
    ASSERT_NE(nullptr, kernel_process);

    Thread* bsp_idle = create_idle_thread_for_cpu(kernel_process, g_cpu_boot, idle_entry, frames);
    Thread* ap_idle = create_idle_thread_for_cpu(kernel_process, &ap_cpu, idle_entry, frames);
    Thread* worker = create_kernel_thread(kernel_process, idle_entry, frames);

    ASSERT_NE(nullptr, bsp_idle);
    ASSERT_NE(nullptr, ap_idle);
    ASSERT_NE(nullptr, worker);

    ap_idle->state = ThreadState::Running;
    g_cpu_host_current = g_cpu_boot;

    EXPECT_EQ(worker, next_runnable_thread(nullptr));
    EXPECT_EQ(2u, runnable_thread_count());
}

TEST(CpuRecord, ExceptionStackExhaustionRejectsBspAndAp)
{
    PageFrameContainer exhausted;
    cpu bsp{}, ap{};
    EXPECT_FALSE(cpu_prepare_exception_stack(bsp, exhausted));
    EXPECT_FALSE(cpu_prepare_exception_stack(ap, exhausted));
    EXPECT_EQ(0u, bsp.tss.ist1);
    EXPECT_EQ(0u, ap.tss.ist1);
    EXPECT_EQ(0, ap.booted);
}

TEST(CpuRecord, ExceptionStacksAreDistinctAndRepeatedPreparationDoesNotAllocate)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    auto frames = initialized_frames();
    cpu bsp{}, ap{};
    const auto before = frames.free_page_count();
    ASSERT_TRUE(cpu_prepare_exception_stack(bsp, frames));
    ASSERT_TRUE(cpu_prepare_exception_stack(ap, frames));
    EXPECT_NE(0u, bsp.tss.ist1);
    EXPECT_NE(bsp.tss.ist1, ap.tss.ist1);
    EXPECT_EQ(before - 2, frames.free_page_count());
    PageFrameContainer exhausted;
    EXPECT_TRUE(cpu_prepare_exception_stack(bsp, exhausted));
    EXPECT_EQ(before - 2, frames.free_page_count());
}
