#include "sync/wait_queue.hpp"

#include "arch/x86_64/cpu/cpu.hpp"
#include "console/console_input.hpp"
#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "proc/thread.hpp"
#include "support/physical_memory.hpp"

#ifdef assert
#undef assert
#endif

#ifdef static_assert
#undef static_assert
#endif

#include <gtest/gtest.h>

#include <array>

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

Thread make_thread(uint64_t tid)
{
    Thread thread{};
    thread.tid = tid;
    thread.state = ThreadState::Blocked;
    return thread;
}

Thread* make_blocked_user_thread(PageFrameContainer& frames)
{
    Process* process = create_user_process("waiter", 0);
    EXPECT_NE(nullptr, process);
    if(nullptr == process)
    {
        return nullptr;
    }

    Thread* thread = create_user_thread(process, 0x1000, 0x2000, frames, false);
    EXPECT_NE(nullptr, thread);
    return thread;
}
}  // namespace

TEST(WaitQueue, EnqueuesAndDequeuesInFifoOrder)
{
    WaitQueue queue("test-wait-queue");
    Thread first = make_thread(1);
    Thread second = make_thread(2);
    Thread third = make_thread(3);

    EXPECT_TRUE(wait_queue_empty(queue));

    wait_queue_enqueue(queue, &first);
    wait_queue_enqueue(queue, &second);
    wait_queue_enqueue(queue, &third);

    EXPECT_EQ(3u, wait_queue_count(queue));
    EXPECT_EQ(&first, wait_queue_dequeue(queue));
    EXPECT_EQ(&second, wait_queue_dequeue(queue));
    EXPECT_EQ(&third, wait_queue_dequeue(queue));
    EXPECT_EQ(nullptr, wait_queue_dequeue(queue));
    EXPECT_TRUE(wait_queue_empty(queue));
    EXPECT_EQ(nullptr, first.wait_link);
    EXPECT_EQ(nullptr, second.wait_link);
    EXPECT_EQ(nullptr, third.wait_link);
}

TEST(WaitQueue, DequeueAllDetachesListInOrder)
{
    WaitQueue queue("test-wait-queue");
    Thread first = make_thread(1);
    Thread second = make_thread(2);
    Thread third = make_thread(3);

    wait_queue_enqueue(queue, &first);
    wait_queue_enqueue(queue, &second);
    wait_queue_enqueue(queue, &third);

    Thread* list = wait_queue_dequeue_all(queue);
    EXPECT_TRUE(wait_queue_empty(queue));
    ASSERT_EQ(&first, list);
    ASSERT_EQ(&second, list->wait_link);
    ASSERT_EQ(&third, list->wait_link->wait_link);
    EXPECT_EQ(nullptr, list->wait_link->wait_link->wait_link);
}

TEST(WaitQueue, DuplicateEnqueueIsIgnored)
{
    WaitQueue queue("test-wait-queue");
    Thread thread = make_thread(1);

    wait_queue_enqueue(queue, &thread);
    wait_queue_enqueue(queue, &thread);

    EXPECT_EQ(1u, wait_queue_count(queue));
    EXPECT_EQ(&thread, wait_queue_dequeue(queue));
    EXPECT_EQ(nullptr, wait_queue_dequeue(queue));
}

TEST(WaitQueue, LockedHelpersSupportExplicitGuard)
{
    WaitQueue queue("test-wait-queue");
    Thread first = make_thread(1);
    Thread second = make_thread(2);

    {
        IrqSpinGuard guard(queue.lock);
        wait_queue_enqueue_locked(queue, &first);
        wait_queue_enqueue_locked(queue, &second);
        EXPECT_FALSE(wait_queue_empty_locked(queue));
        EXPECT_EQ(&first, wait_queue_dequeue_locked(queue));
    }

    EXPECT_EQ(&second, wait_queue_dequeue(queue));
    EXPECT_TRUE(wait_queue_empty(queue));
}

TEST(Completion, SignalIsOneShotAndResettable)
{
    Completion completion("test-completion");

    EXPECT_FALSE(completion_done(completion));
    EXPECT_TRUE(completion_signal(completion));
    EXPECT_TRUE(completion_done(completion));
    EXPECT_FALSE(completion_signal(completion));

    completion_reset(completion);

    EXPECT_FALSE(completion_done(completion));
    EXPECT_TRUE(completion_signal(completion));
    EXPECT_TRUE(completion_done(completion));
}

TEST(WaitQueue, WakeOneMarksBlockedThreadReady)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);

    ASSERT_TRUE(init_tasks(frames));
    WaitQueue queue("test-wait-queue");
    Thread* blocked = make_blocked_user_thread(frames);
    ASSERT_NE(nullptr, blocked);

    wait_queue_enqueue(queue, blocked);

    EXPECT_EQ(1u, wait_queue_wake_one(queue));
    EXPECT_EQ(ThreadState::Ready, blocked->state);
    EXPECT_EQ(g_cpu_boot, blocked->run_queue_cpu);
    EXPECT_TRUE(wait_queue_empty(queue));
}

TEST(WaitQueue, WakeAllPreservesFifoOrder)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);

    ASSERT_TRUE(init_tasks(frames));
    WaitQueue queue("test-wait-queue");
    Thread* first = make_blocked_user_thread(frames);
    Thread* second = make_blocked_user_thread(frames);
    Thread* third = make_blocked_user_thread(frames);
    ASSERT_NE(nullptr, first);
    ASSERT_NE(nullptr, second);
    ASSERT_NE(nullptr, third);

    wait_queue_enqueue(queue, first);
    wait_queue_enqueue(queue, second);
    wait_queue_enqueue(queue, third);

    EXPECT_EQ(3u, wait_queue_wake_all(queue));
    EXPECT_TRUE(wait_queue_empty(queue));
    EXPECT_EQ(first, next_runnable_thread(nullptr));
    EXPECT_EQ(second, next_runnable_thread(nullptr));
    EXPECT_EQ(third, next_runnable_thread(nullptr));
}
