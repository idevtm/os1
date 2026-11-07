#include <gtest/gtest.h>

#ifdef assert
#undef assert
#endif

#ifdef static_assert
#undef static_assert
#endif

#include "arch/x86_64/cpu/cpu.hpp"
#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "proc/thread.hpp"
#include "sched/scheduler.hpp"
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
    owner->balance_idle_ticks = 0;
}

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

Thread* make_blocked_user_thread(Process* process, PageFrameContainer& frames)
{
    Thread* thread = create_user_thread(process, 0x1000, 0x2000, frames, false);
    EXPECT_NE(nullptr, thread);
    return thread;
}
}  // namespace

TEST(LoadBalancer, PullsTailFromLargestDonor)
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

    Process* user_process = create_user_process("worker", 0);
    ASSERT_NE(nullptr, user_process);

    Thread* first = make_blocked_user_thread(user_process, frames);
    Thread* second = make_blocked_user_thread(user_process, frames);
    Thread* third = make_blocked_user_thread(user_process, frames);
    Thread* other = make_blocked_user_thread(user_process, frames);

    mark_thread_ready(first, &first_ap);
    mark_thread_ready(second, &first_ap);
    mark_thread_ready(third, &first_ap);
    mark_thread_ready(other, &second_ap);

    ASSERT_TRUE(scheduler_balance_cpu(g_cpu_boot, 64, true));
    EXPECT_EQ(1u, cpu_run_queue_length(g_cpu_boot));
    EXPECT_EQ(2u, cpu_run_queue_length(&first_ap));
    EXPECT_EQ(1u, cpu_run_queue_length(&second_ap));
    EXPECT_EQ(g_cpu_boot, third->scheduler_cpu);
    EXPECT_EQ(g_cpu_boot, third->run_queue_cpu);
    EXPECT_EQ(1u, g_cpu_boot->migrate_in);
    EXPECT_EQ(1u, first_ap.migrate_out);

    g_cpu_host_current = g_cpu_boot;
    EXPECT_EQ(third, next_runnable_thread(nullptr));
}

TEST(LoadBalancer, RespectsAffinityMask)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);

    cpu ap_cpu{};
    ap_cpu.self = &ap_cpu;
    ap_cpu.id = 1;
    ap_cpu.booted = 1;
    ap_cpu.magic = CPU_MAGIC;

    HostCpuChainScope cpu_scope(&ap_cpu, nullptr);
    ASSERT_TRUE(init_tasks(frames));

    Process* kernel_process = create_kernel_process(0);
    ASSERT_NE(nullptr, kernel_process);
    ASSERT_NE(nullptr, create_idle_thread_for_cpu(kernel_process, g_cpu_boot, idle_entry, frames));
    ASSERT_NE(nullptr, create_idle_thread_for_cpu(kernel_process, &ap_cpu, idle_entry, frames));

    Process* user_process = create_user_process("worker", 0);
    ASSERT_NE(nullptr, user_process);

    Thread* pinned = make_blocked_user_thread(user_process, frames);
    pinned->affinity_mask = 1ull << ap_cpu.id;
    mark_thread_ready(pinned, &ap_cpu);

    EXPECT_FALSE(scheduler_balance_cpu(g_cpu_boot, 64, true));
    EXPECT_EQ(0u, cpu_run_queue_length(g_cpu_boot));
    EXPECT_EQ(1u, cpu_run_queue_length(&ap_cpu));
    EXPECT_EQ(&ap_cpu, pinned->scheduler_cpu);
}

TEST(LoadBalancer, MigrationCooldownPreventsImmediateOscillation)
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

    Process* user_process = create_user_process("worker", 0);
    ASSERT_NE(nullptr, user_process);

    Thread* stable_boot = make_blocked_user_thread(user_process, frames);
    stable_boot->affinity_mask = 1ull << g_cpu_boot->id;
    mark_thread_ready(stable_boot, g_cpu_boot);

    Thread* first = make_blocked_user_thread(user_process, frames);
    Thread* second = make_blocked_user_thread(user_process, frames);
    Thread* third = make_blocked_user_thread(user_process, frames);
    mark_thread_ready(first, &first_ap);
    mark_thread_ready(second, &first_ap);
    mark_thread_ready(third, &first_ap);

    ASSERT_TRUE(scheduler_balance_cpu(g_cpu_boot, 100, true));
    EXPECT_EQ(g_cpu_boot, third->scheduler_cpu);
    EXPECT_EQ(100u, third->last_migration_tick);

    EXPECT_FALSE(scheduler_balance_cpu(&second_ap, 120, true));
    EXPECT_EQ(g_cpu_boot, third->scheduler_cpu);
    EXPECT_EQ(g_cpu_boot, third->run_queue_cpu);
    EXPECT_EQ(0u, second_ap.migrate_in);
}

TEST(LoadBalancer, IdleTickPathTriggersForcedBalance)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);

    cpu ap_cpu{};
    ap_cpu.self = &ap_cpu;
    ap_cpu.id = 1;
    ap_cpu.booted = 1;
    ap_cpu.magic = CPU_MAGIC;

    HostCpuChainScope cpu_scope(&ap_cpu, nullptr);
    ASSERT_TRUE(init_tasks(frames));

    Process* kernel_process = create_kernel_process(0);
    ASSERT_NE(nullptr, kernel_process);
    Thread* boot_idle = create_idle_thread_for_cpu(kernel_process, g_cpu_boot, idle_entry, frames);
    ASSERT_NE(nullptr, boot_idle);
    ASSERT_NE(nullptr, create_idle_thread_for_cpu(kernel_process, &ap_cpu, idle_entry, frames));

    Process* user_process = create_user_process("worker", 0);
    ASSERT_NE(nullptr, user_process);

    Thread* first = make_blocked_user_thread(user_process, frames);
    Thread* second = make_blocked_user_thread(user_process, frames);
    mark_thread_ready(first, &ap_cpu);
    mark_thread_ready(second, &ap_cpu);

    g_cpu_host_current = g_cpu_boot;
    set_current_thread(boot_idle);
    for(uint64_t tick = 1; tick < 4; ++tick)
    {
        g_cpu_boot->timer_ticks = tick;
        scheduler_handle_timer_tick();
        EXPECT_EQ(0u, cpu_run_queue_length(g_cpu_boot));
    }

    g_cpu_boot->timer_ticks = 4;
    scheduler_handle_timer_tick();
    EXPECT_EQ(1u, cpu_run_queue_length(g_cpu_boot));
    EXPECT_EQ(1u, cpu_run_queue_length(&ap_cpu));
    EXPECT_EQ(1u, g_cpu_boot->migrate_in);
}

TEST(LoadBalancer, DoesNotStealStackBeforeDonorHandoff)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);
    cpu donor{};
    donor.self = &donor;
    donor.id = 1;
    donor.booted = 1;
    donor.magic = CPU_MAGIC;
    HostCpuChainScope cpu_scope(&donor, nullptr);
    ASSERT_TRUE(init_tasks(frames));
    Process* process = create_user_process("handoff", 0);
    ASSERT_NE(nullptr, process);
    Thread* parked = make_blocked_user_thread(process, frames);
    Thread* outgoing = make_blocked_user_thread(process, frames);
    ASSERT_NE(nullptr, parked);
    ASSERT_NE(nullptr, outgoing);
    mark_thread_ready(parked, &donor);
    mark_thread_ready(outgoing, &donor);
    // schedule_next has requeued its current thread, but assembly has not left its stack.
    parked->affinity_mask = 1ull << donor.id;
    donor.current_thread = outgoing;
    EXPECT_FALSE(scheduler_balance_cpu(g_cpu_boot, 64, true));
    EXPECT_EQ(&donor, outgoing->run_queue_cpu);
    EXPECT_EQ(2u, donor.runq.length);
    EXPECT_EQ(0u, g_cpu_boot->runq.length);
    // Publication occurs only after restore_thread leaves the outgoing stack and CR3.
    donor.current_thread = nullptr;
    ASSERT_TRUE(scheduler_balance_cpu(g_cpu_boot, 128, true));
    EXPECT_EQ(outgoing, next_runnable_thread(nullptr));
    EXPECT_EQ(&donor, parked->run_queue_cpu);
}
