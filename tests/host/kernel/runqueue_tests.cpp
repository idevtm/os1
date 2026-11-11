#include <gtest/gtest.h>

#ifdef assert
#undef assert
#endif

#ifdef static_assert
#undef static_assert
#endif

#include "arch/x86_64/cpu/cpu.hpp"
#include "arch/x86_64/apic/ipi.hpp"
#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "proc/thread.hpp"
#include "support/lapic_stub.hpp"
#include "support/physical_memory.hpp"

#include <array>

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

void reset_host_cpu_scheduler_state(cpu* owner)
{
    if(nullptr == owner)
    {
        return;
    }

    owner->current_thread = nullptr;
    owner->idle_thread = nullptr;
    owner->runq.head = nullptr;
    owner->runq.tail = nullptr;
    owner->runq.length = 0;
    owner->timer_ticks = 0;
    owner->reschedule_pending = 0;
    owner->irq_stack_thread = nullptr;
    owner->enqueue_count = 0;
    owner->dequeue_count = 0;
    owner->idle_ticks = 0;
    owner->kernel_thread_ping_count = 0;
    owner->migrate_in = 0;
    owner->migrate_out = 0;
}

struct HostCpuScope
{
    explicit HostCpuScope(cpu* ap_cpu)
        : saved_next(g_cpu_boot->next), saved_current(g_cpu_host_current), ap(ap_cpu)
    {
        reset_host_cpu_scheduler_state(g_cpu_boot);
        reset_host_cpu_scheduler_state(ap);
        g_cpu_boot->next = ap;
        g_cpu_host_current = g_cpu_boot;
    }

    ~HostCpuScope()
    {
        reset_host_cpu_scheduler_state(g_cpu_boot);
        reset_host_cpu_scheduler_state(ap);
        g_cpu_boot->next = saved_next;
        g_cpu_host_current = saved_current;
    }

    cpu* saved_next;
    cpu* saved_current;
    cpu* ap;
};

struct HostCpuChainScope
{
    HostCpuChainScope(cpu* first_ap, cpu* second_ap)
        : saved_next(g_cpu_boot->next), saved_current(g_cpu_host_current), first(first_ap), second(second_ap)
    {
        reset_host_cpu_scheduler_state(g_cpu_boot);
        reset_host_cpu_scheduler_state(first);
        reset_host_cpu_scheduler_state(second);
        g_cpu_boot->next = first;
        if(nullptr != first)
        {
            first->next = second;
        }
        if(nullptr != second)
        {
            second->next = nullptr;
        }
        g_cpu_host_current = g_cpu_boot;
    }

    ~HostCpuChainScope()
    {
        reset_host_cpu_scheduler_state(g_cpu_boot);
        reset_host_cpu_scheduler_state(first);
        reset_host_cpu_scheduler_state(second);
        g_cpu_boot->next = saved_next;
        if(nullptr != first)
        {
            first->next = nullptr;
        }
        if(nullptr != second)
        {
            second->next = nullptr;
        }
        g_cpu_host_current = saved_current;
    }

    cpu* saved_next;
    cpu* saved_current;
    cpu* first;
    cpu* second;
};
}  // namespace

TEST(RunQueue, PopsReadyThreadsInFifoOrder)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);

    reset_host_cpu_scheduler_state(g_cpu_boot);

    ASSERT_TRUE(init_tasks(frames));
    Process* kernel_process = create_kernel_process(0);
    ASSERT_NE(nullptr, kernel_process);

    Thread* idle = create_kernel_thread(kernel_process, idle_entry, frames);
    Thread* worker_a = create_kernel_thread(kernel_process, idle_entry, frames);
    Thread* worker_b = create_kernel_thread(kernel_process, idle_entry, frames);

    ASSERT_NE(nullptr, idle);
    ASSERT_NE(nullptr, worker_a);
    ASSERT_NE(nullptr, worker_b);
    EXPECT_EQ(idle, idle_thread());
    EXPECT_EQ(2u, cpu_run_queue_length(g_cpu_boot));

    EXPECT_EQ(worker_a, next_runnable_thread(nullptr));
    EXPECT_EQ(worker_b, next_runnable_thread(nullptr));
    EXPECT_EQ(nullptr, next_runnable_thread(nullptr));
    EXPECT_EQ(0u, cpu_run_queue_length(g_cpu_boot));
}

TEST(RunQueue, KernelWakeTargetUsesAssignedCpu)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);

    cpu ap_cpu{};
    ap_cpu.self = &ap_cpu;
    ap_cpu.id = 1;
    ap_cpu.booted = 1;
    ap_cpu.magic = CPU_MAGIC;

    HostCpuScope cpu_scope(&ap_cpu);
    ASSERT_TRUE(init_tasks(frames));
    ASSERT_TRUE(ipi_initialize());
    lapic_stub_reset();
    Process* kernel_process = create_kernel_process(0);
    ASSERT_NE(nullptr, kernel_process);
    ASSERT_NE(nullptr, create_idle_thread_for_cpu(kernel_process, g_cpu_boot, idle_entry, frames));
    ASSERT_NE(nullptr, create_idle_thread_for_cpu(kernel_process, &ap_cpu, idle_entry, frames));

    Thread* blocked = create_kernel_thread(kernel_process, idle_entry, frames);
    ASSERT_NE(nullptr, blocked);
    g_cpu_host_current = &ap_cpu;
    EXPECT_EQ(blocked, next_runnable_thread(nullptr));
    blocked->state = ThreadState::Blocked;
    blocked->scheduler_cpu = &ap_cpu;
    EXPECT_EQ(nullptr, blocked->run_queue_cpu);

    g_cpu_host_current = g_cpu_boot;
    mark_thread_ready(blocked);

    EXPECT_EQ(ThreadState::Ready, blocked->state);
    EXPECT_EQ(&ap_cpu, blocked->scheduler_cpu);
    EXPECT_EQ(&ap_cpu, blocked->run_queue_cpu);
    EXPECT_EQ(1u, cpu_run_queue_length(&ap_cpu));
    EXPECT_EQ(0u, cpu_run_queue_length(g_cpu_boot));
    EXPECT_EQ(1u, lapic_stub_icr_send_count());
    EXPECT_EQ(static_cast<uint32_t>(ap_cpu.id) << 24, lapic_stub_last_icr_high());
    EXPECT_EQ(ipi_reschedule_vector(), static_cast<uint8_t>(lapic_stub_last_icr_low()));

    g_cpu_host_current = &ap_cpu;
    EXPECT_EQ(blocked, next_runnable_thread(nullptr));
    EXPECT_EQ(0u, cpu_run_queue_length(&ap_cpu));
}

TEST(RunQueue, TimedOutApIsRejectedAsAnExplicitOrRememberedTarget)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);

    cpu offline{};
    offline.id = 1;
    ASSERT_TRUE(cpu_ap_finish_startup(&offline, ApStartupState::TimedOut));
    HostCpuScope cpu_scope(&offline);
    ASSERT_TRUE(init_tasks(frames));
    ASSERT_TRUE(ipi_initialize());
    lapic_stub_reset();

    Process* process = create_kernel_process(0);
    ASSERT_NE(nullptr, process);
    ASSERT_NE(nullptr, create_idle_thread_for_cpu(process, g_cpu_boot, idle_entry, frames));
    ASSERT_NE(nullptr, create_idle_thread_for_cpu(process, &offline, idle_entry, frames));
    EXPECT_EQ(nullptr, create_kernel_thread(process, idle_entry, frames, &offline));

    Thread* explicit_worker = create_kernel_thread(process, idle_entry, frames, nullptr, false);
    Thread* remembered_worker = create_kernel_thread(process, idle_entry, frames, nullptr, false);
    ASSERT_NE(nullptr, explicit_worker);
    ASSERT_NE(nullptr, remembered_worker);
    remembered_worker->scheduler_cpu = &offline;
    wake_blocked_thread(explicit_worker, &offline);
    wake_blocked_thread(remembered_worker);

    EXPECT_EQ(g_cpu_boot, explicit_worker->run_queue_cpu);
    EXPECT_EQ(g_cpu_boot, remembered_worker->run_queue_cpu);
    EXPECT_EQ(0u, cpu_run_queue_length(&offline));
    EXPECT_EQ(2u, cpu_run_queue_length(g_cpu_boot));
    EXPECT_EQ(0u, lapic_stub_icr_send_count());
}

TEST(RunQueue, KernelThreadsRoundRobinAcrossBootedCpus)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);

    cpu first_ap{};
    first_ap.self = &first_ap;
    first_ap.id = 1;
    first_ap.booted = 1;
    first_ap.magic = CPU_MAGIC;

    cpu second_ap{};
    second_ap.self = &second_ap;
    second_ap.id = 2;
    second_ap.booted = 1;
    second_ap.magic = CPU_MAGIC;

    HostCpuChainScope cpu_scope(&first_ap, &second_ap);
    ASSERT_TRUE(init_tasks(frames));
    Process* kernel_process = create_kernel_process(0);
    ASSERT_NE(nullptr, kernel_process);
    ASSERT_NE(nullptr, create_idle_thread_for_cpu(kernel_process, g_cpu_boot, idle_entry, frames));
    ASSERT_NE(nullptr, create_idle_thread_for_cpu(kernel_process, &first_ap, idle_entry, frames));
    ASSERT_NE(nullptr, create_idle_thread_for_cpu(kernel_process, &second_ap, idle_entry, frames));

    Thread* first_worker = create_kernel_thread(kernel_process, idle_entry, frames);
    Thread* second_worker = create_kernel_thread(kernel_process, idle_entry, frames);
    Thread* third_worker = create_kernel_thread(kernel_process, idle_entry, frames);

    ASSERT_NE(nullptr, first_worker);
    ASSERT_NE(nullptr, second_worker);
    ASSERT_NE(nullptr, third_worker);
    EXPECT_EQ(&first_ap, first_worker->scheduler_cpu);
    EXPECT_EQ(&first_ap, first_worker->run_queue_cpu);
    EXPECT_EQ(&second_ap, second_worker->scheduler_cpu);
    EXPECT_EQ(&second_ap, second_worker->run_queue_cpu);
    EXPECT_EQ(g_cpu_boot, third_worker->scheduler_cpu);
    EXPECT_EQ(g_cpu_boot, third_worker->run_queue_cpu);
    EXPECT_EQ(1u, cpu_run_queue_length(&first_ap));
    EXPECT_EQ(1u, cpu_run_queue_length(&second_ap));
    EXPECT_EQ(1u, cpu_run_queue_length(g_cpu_boot));
}

TEST(RunQueue, UserThreadsPreferLeastLoadedCpu)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);

    cpu ap_cpu{};
    ap_cpu.self = &ap_cpu;
    ap_cpu.id = 1;
    ap_cpu.booted = 1;
    ap_cpu.magic = CPU_MAGIC;

    HostCpuScope cpu_scope(&ap_cpu);
    ASSERT_TRUE(init_tasks(frames));
    Process* kernel_process = create_kernel_process(0);
    ASSERT_NE(nullptr, kernel_process);
    ASSERT_NE(nullptr, create_idle_thread_for_cpu(kernel_process, g_cpu_boot, idle_entry, frames));
    ASSERT_NE(nullptr, create_idle_thread_for_cpu(kernel_process, &ap_cpu, idle_entry, frames));

    Process* user_process = create_user_process("user-worker", 0);
    ASSERT_NE(nullptr, user_process);
    Thread* first_user = create_user_thread(user_process, 0x1000, 0x2000, frames, true);
    Thread* second_user = create_user_thread(user_process, 0x1000, 0x3000, frames, true);

    ASSERT_NE(nullptr, first_user);
    ASSERT_NE(nullptr, second_user);
    EXPECT_EQ(&ap_cpu, first_user->scheduler_cpu);
    EXPECT_EQ(&ap_cpu, first_user->run_queue_cpu);
    EXPECT_EQ(g_cpu_boot, second_user->scheduler_cpu);
    EXPECT_EQ(g_cpu_boot, second_user->run_queue_cpu);
    EXPECT_EQ(1u, cpu_run_queue_length(g_cpu_boot));
    EXPECT_EQ(1u, cpu_run_queue_length(&ap_cpu));
}

TEST(RunQueue, WokenUserThreadsUseLeastLoadedCpu)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);

    cpu ap_cpu{};
    ap_cpu.self = &ap_cpu;
    ap_cpu.id = 1;
    ap_cpu.booted = 1;
    ap_cpu.magic = CPU_MAGIC;

    HostCpuScope cpu_scope(&ap_cpu);
    ASSERT_TRUE(init_tasks(frames));
    Process* kernel_process = create_kernel_process(0);
    ASSERT_NE(nullptr, kernel_process);
    ASSERT_NE(nullptr, create_idle_thread_for_cpu(kernel_process, g_cpu_boot, idle_entry, frames));
    ASSERT_NE(nullptr, create_idle_thread_for_cpu(kernel_process, &ap_cpu, idle_entry, frames));

    Process* user_process = create_user_process("user-worker", 0);
    ASSERT_NE(nullptr, user_process);
    Thread* running_elsewhere = create_user_thread(user_process, 0x1000, 0x2000, frames, true);
    Thread* blocked = create_user_thread(user_process, 0x1000, 0x3000, frames, false);

    ASSERT_NE(nullptr, running_elsewhere);
    ASSERT_NE(nullptr, blocked);
    EXPECT_EQ(&ap_cpu, running_elsewhere->scheduler_cpu);
    EXPECT_EQ(&ap_cpu, running_elsewhere->run_queue_cpu);

    blocked->scheduler_cpu = &ap_cpu;
    mark_thread_ready(blocked);

    EXPECT_EQ(g_cpu_boot, blocked->scheduler_cpu);
    EXPECT_EQ(g_cpu_boot, blocked->run_queue_cpu);
    EXPECT_EQ(1u, cpu_run_queue_length(g_cpu_boot));
    EXPECT_EQ(1u, cpu_run_queue_length(&ap_cpu));
}