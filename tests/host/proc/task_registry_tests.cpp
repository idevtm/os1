#include "arch/x86_64/apic/ipi.hpp"
#include "arch/x86_64/cpu/cpu.hpp"
#include "core/kernel_state.hpp"
#include "handoff/memory_layout.h"
#include "mm/boot_mapping.hpp"
#include "mm/kmem.hpp"
#include "mm/user_copy.hpp"
#include "mm/virtual_memory.hpp"
#include "proc/process.hpp"
#include "proc/kernel_stack.hpp"
#include "proc/thread.hpp"
#include "sched/scheduler.hpp"
#include "support/lapic_stub.hpp"
#include "support/physical_memory.hpp"
#include "syscall/wait.hpp"

#ifdef assert
#undef assert
#endif

#ifdef static_assert
#undef static_assert
#endif

#include <gtest/gtest.h>

#include <array>
#include <vector>

extern void (*host_tlb_retirement_wait)(cpu*);

namespace
{
constexpr uint64_t kArenaBytes = 32ull * 1024ull * 1024ull;
constexpr uint64_t kBitmapPhysical = 0x300000;
constexpr size_t kRegistryGrowthCount = 48;

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

void noop_thread_entry() {}

struct HostApCpuScope
{
    HostApCpuScope() : saved_next(g_cpu_boot->next), saved_current(g_cpu_host_current)
    {
        ap.self = &ap;
        ap.next = saved_next;
        ap.id = 1;
        ap.booted = 1;
        ap.magic = CPU_MAGIC;
        g_cpu_boot->next = &ap;
        g_cpu_host_current = g_cpu_boot;
    }

    ~HostApCpuScope()
    {
        g_cpu_boot->next = saved_next;
        g_cpu_host_current = saved_current;
    }

    cpu ap{};
    cpu* saved_next = nullptr;
    cpu* saved_current = nullptr;
};
}  // namespace

TEST(TaskRegistry, GuardedStacksShareSupervisorMappingsAndReleaseBacking)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    VirtualMemory kernel(frames);
    ASSERT_TRUE(kernel.map_physical(kKernelVirtualBase, 0x100000, 1, PageFlags::Write));
    ASSERT_TRUE(initialize_kernel_stacks(frames, kernel.root()));

    // Clone before mapping a stack, as happens for an existing process.
    VirtualMemory process(frames);
    ASSERT_TRUE(process.clone_kernel_mappings(kernel.root()));
    uint64_t physical = 0, top = 0;
    ASSERT_TRUE(allocate_kernel_stack(frames, physical, top));
    const uint64_t bottom = top - kKernelThreadStackPages * kPageSize;

    uint64_t translated = 0, flags = 0;
    EXPECT_FALSE(process.translate(bottom - kPageSize, translated, flags));
    EXPECT_FALSE(process.translate(top, translated, flags));
    for(size_t page = 0; page < kKernelThreadStackPages; ++page)
    {
        ASSERT_TRUE(process.translate(bottom + page * kPageSize, translated, flags));
        EXPECT_EQ(physical + page * kPageSize, translated);
        EXPECT_EQ(
            static_cast<uint64_t>(PageFlags::Present | PageFlags::Write | PageFlags::NoExecute),
            flags);
    }
    EXPECT_FALSE(map_direct_range(kernel, kDirectMapPhysicalLimit - 1, 2));
    EXPECT_EQ(kInvalidPhysicalAddress, virt_to_phys(bottom));

    const auto before_release = frames.free_page_count();
    {
        HostApCpuScope cpus;
        ASSERT_TRUE(ipi_initialize());
        static VirtualMemory* remote = nullptr;
        static PageFrameContainer* allocator = nullptr;
        static uint64_t address = 0, free_pages = 0;
        static unsigned acknowledgments = 0;
        remote = &process;
        allocator = &frames;
        address = bottom;
        free_pages = before_release;
        acknowledgments = 0;
        host_tlb_retirement_wait = [](cpu* target) {
            if(target == cpu_cur())
                return;
            uint64_t physical_address = 0, leaf_flags = 0;
            EXPECT_FALSE(remote->translate(address, physical_address, leaf_flags));
            EXPECT_EQ(free_pages, allocator->free_page_count());
            auto* saved = g_cpu_host_current;
            g_cpu_host_current = target;
            ipi_poll_tlb_shootdown();
            g_cpu_host_current = saved;
            ++acknowledgments;
        };
        release_kernel_stack(frames, physical);
        host_tlb_retirement_wait = nullptr;
        EXPECT_EQ(1u, acknowledgments);
    }
    EXPECT_EQ(before_release + kKernelThreadStackPages, frames.free_page_count());
    EXPECT_FALSE(process.translate(bottom, translated, flags));
    EXPECT_FALSE(process.translate(top - 1, translated, flags));

    uint64_t reused = 0, reused_top = 0;
    ASSERT_TRUE(allocate_kernel_stack(frames, reused, reused_top));
    EXPECT_EQ(physical, reused);
    EXPECT_EQ(top, reused_top);
    release_kernel_stack(frames, reused);
}

TEST(TaskRegistry, GuardedStackAllocationReturnsBackingIfPageTablesCannotGrow)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    VirtualMemory kernel(frames);
    ASSERT_TRUE(kernel.map_physical(kKernelVirtualBase, 0x100000, 1, PageFlags::Write));
    ASSERT_TRUE(initialize_kernel_stacks(frames, kernel.root()));

    std::vector<uint64_t> held_pages;
    while(frames.free_page_count() > kKernelThreadStackPages)
    {
        uint64_t held = 0;
        ASSERT_TRUE(frames.allocate(held));
        held_pages.push_back(held);
    }

    uint64_t physical = 0, top = 0;
    EXPECT_FALSE(allocate_kernel_stack(frames, physical, top));
    EXPECT_EQ(0u, physical);
    EXPECT_EQ(0u, top);
    EXPECT_EQ(kKernelThreadStackPages, frames.free_page_count());
    for(uint64_t held : held_pages)
    {
        EXPECT_TRUE(frames.free(held));
    }
}

TEST(TaskRegistry, GuardedStackAllocationRollsBackPartialMapping)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    VirtualMemory kernel(frames);

    constexpr uint64_t stack_bytes = kKernelThreadStackPages * kPageSize;
    constexpr uint64_t slot = 409;
    constexpr uint64_t backing = slot * stack_bytes;
    constexpr uint64_t bottom =
        kKernelStackVirtualBase + slot * (stack_bytes + kPageSize) + kPageSize;
    constexpr uint64_t leaf_table_bytes = 512 * kPageSize;
    static_assert(bottom % leaf_table_bytes == leaf_table_bytes - 2 * kPageSize);

    // The first two stack pages fit in an existing leaf table.
    constexpr uint64_t neighbor = bottom - 2 * kPageSize;
    ASSERT_TRUE(kernel.map_physical(neighbor, 0x100000, 1, PageFlags::Write));
    ASSERT_TRUE(initialize_kernel_stacks(frames, kernel.root()));
    ASSERT_TRUE(frames.reserve_range(0, backing));
    ASSERT_TRUE(frames.reserve_range(backing + stack_bytes, kArenaBytes - backing - stack_bytes));
    ASSERT_EQ(kKernelThreadStackPages, frames.free_page_count());

    uint64_t physical = 0, top = 0;
    EXPECT_FALSE(allocate_kernel_stack(frames, physical, top));
    EXPECT_EQ(0u, physical);
    EXPECT_EQ(0u, top);
    EXPECT_EQ(kKernelThreadStackPages, frames.free_page_count());

    uint64_t translated = 0, flags = 0;
    for(size_t offset = 0; offset < stack_bytes; offset += kPageSize)
    {
        EXPECT_FALSE(kernel.translate(bottom + offset, translated, flags));
    }
    ASSERT_TRUE(kernel.translate(neighbor, translated, flags));
    EXPECT_EQ(0x100000u, translated);
}

TEST(TaskRegistry, ProcessesGrowPastLegacyLimit)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);

    ASSERT_TRUE(init_tasks(frames));

    std::vector<Process*> processes;
    processes.reserve(kRegistryGrowthCount);
    for(size_t index = 0; index < kRegistryGrowthCount; ++index)
    {
        Process* process = create_user_process("worker", 0);
        ASSERT_NE(nullptr, process);
        processes.push_back(process);
    }

    size_t count = 0;
    for(Process* process = first_process(); nullptr != process; process = next_process(process))
    {
        ++count;
    }

    EXPECT_EQ(processes.size(), count);
    EXPECT_EQ(processes.front(), first_process());

    for(Process* process = first_process(); nullptr != process;)
    {
        Process* next = next_process(process);
        EXPECT_TRUE(reap_process(process, frames));
        process = next;
    }

    EXPECT_EQ(nullptr, first_process());
}

TEST(TaskRegistry, ThreadsGrowPastLegacyLimitAndReapCleanly)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);

    ASSERT_TRUE(init_tasks(frames));
    Process* kernel_process = create_kernel_process(0);
    ASSERT_NE(nullptr, kernel_process);

    std::vector<Thread*> threads;
    threads.reserve(kRegistryGrowthCount);
    for(size_t index = 0; index < kRegistryGrowthCount; ++index)
    {
        Thread* thread = create_kernel_thread(kernel_process, noop_thread_entry, frames);
        ASSERT_NE(nullptr, thread);
        threads.push_back(thread);
    }

    size_t count = 0;
    for(Thread* thread = first_thread(); nullptr != thread; thread = next_thread(thread))
    {
        ++count;
    }

    ASSERT_EQ(threads.size(), count);
    ASSERT_EQ(threads.front(), idle_thread());
    EXPECT_EQ(threads.size(), runnable_thread_count());
    ASSERT_LT(2u, threads.size());
    EXPECT_EQ(threads[1], next_runnable_thread(nullptr));
    EXPECT_EQ(threads[2], next_runnable_thread(nullptr));

    for(Thread* thread : threads)
    {
        thread->state = ThreadState::Dying;
    }

    reap_dead_threads(frames);

    EXPECT_EQ(nullptr, first_thread());
    EXPECT_EQ(nullptr, first_process());
    EXPECT_EQ(0u, runnable_thread_count());
}

TEST(TaskRegistry, ReaperCollectsPublishedOrphanAfterParentWaitpid)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);
    ASSERT_TRUE(init_tasks(frames));

    Process* grandparent = create_user_process("grandparent", 0);
    ASSERT_NE(nullptr, grandparent);
    Process* parent = create_user_process("parent", 0, grandparent);
    ASSERT_NE(nullptr, parent);
    VirtualMemory child_vm(frames);
    ASSERT_TRUE(child_vm.allocate_and_map(kUserImageBase, 1, PageFlags::User | PageFlags::Write));
    Process* child = create_user_process("child", child_vm.root(), parent);
    ASSERT_NE(nullptr, child);
    Thread* child_thread = create_user_thread(child, kUserImageBase, kUserStackTop, frames, false);
    ASSERT_NE(nullptr, child_thread);
    Thread* parent_thread =
        create_user_thread(parent, kUserImageBase, kUserStackTop, frames, false);
    ASSERT_NE(nullptr, parent_thread);
    const auto parent_pid = parent->pid;

    set_current_thread(child_thread);
    mark_current_thread_dying(7);
    set_current_thread(nullptr);
    reap_dead_threads(frames);
    ASSERT_EQ(child, next_process(parent));
    EXPECT_TRUE(child->exit_cleanup_complete);

    set_current_thread(parent_thread);
    mark_current_thread_dying(8);
    set_current_thread(nullptr);
    reap_dead_threads(frames);
    Thread waiter{};
    waiter.process = grandparent;
    long result = -1;
    ASSERT_TRUE(try_complete_wait_pid(frames, &waiter, parent_pid, 0, result));
    ASSERT_EQ(static_cast<long>(parent_pid), result);
    ASSERT_EQ(child, next_process(grandparent));
    EXPECT_EQ(nullptr, child->parent);

    const auto before = frames.free_page_count();
    reap_dead_threads(frames);
    EXPECT_EQ(nullptr, next_process(grandparent));
    const auto after = frames.free_page_count();
    EXPECT_GE(after, before + 5);
    reap_dead_threads(frames);
    EXPECT_EQ(after, frames.free_page_count());
    EXPECT_TRUE(reap_process(grandparent, frames));
}

TEST(TaskRegistry, WaitpidBlocksBeforeChildExitCanBeWoken)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);

    ASSERT_TRUE(init_tasks(frames));

    Process* parent = create_user_process("parent", 0);
    ASSERT_NE(nullptr, parent);
    Process* child = create_user_process("child", 0);
    ASSERT_NE(nullptr, child);
    child->parent = parent;
    const uint64_t child_pid = child->pid;

    Thread* parent_thread = create_user_thread(parent, 0x1000, 0x2000, frames, false);
    ASSERT_NE(nullptr, parent_thread);
    Thread* child_thread = create_user_thread(child, 0x1000, 0x3000, frames, false);
    ASSERT_NE(nullptr, child_thread);

    set_current_thread(parent_thread);
    long result = -1;
    EXPECT_FALSE(try_complete_or_block_wait_pid(frames, parent_thread, child->pid, 0, result));
    EXPECT_EQ(ThreadState::Blocked, parent_thread->state);

    set_current_thread(nullptr);
    child_thread->exit_status = 9;
    child_thread->state = ThreadState::Dying;
    child->exit_status = 9;
    child->state = ProcessState::Zombie;

    reap_dead_threads(frames);

    EXPECT_EQ(ThreadState::Ready, parent_thread->state);
    EXPECT_EQ(child_pid, parent_thread->frame.rax);
    EXPECT_EQ(parent, first_process());
    EXPECT_EQ(nullptr, next_process(parent));

    parent_thread->state = ThreadState::Dying;
    parent->state = ProcessState::Dying;
    reap_dead_threads(frames);
    EXPECT_EQ(nullptr, first_process());
}

TEST(TaskRegistry, ReaperSkipsDyingThreadStillCurrentOnAnotherCpu)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);
    HostApCpuScope cpus;

    ASSERT_TRUE(init_tasks(frames));

    Process* parent = create_user_process("parent", 0);
    ASSERT_NE(nullptr, parent);
    Process* child = create_user_process("child", 0);
    ASSERT_NE(nullptr, child);
    child->parent = parent;
    const uint64_t child_pid = child->pid;

    Thread* parent_thread = create_user_thread(parent, 0x1000, 0x2000, frames, false);
    ASSERT_NE(nullptr, parent_thread);
    Thread* child_thread = create_user_thread(child, 0x1000, 0x3000, frames, false);
    ASSERT_NE(nullptr, child_thread);

    set_current_thread(parent_thread);
    long result = -1;
    ASSERT_FALSE(try_complete_or_block_wait_pid(frames, parent_thread, child_pid, 0, result));
    set_current_thread(nullptr);

    g_cpu_host_current = &cpus.ap;
    set_current_thread(child_thread);
    mark_current_thread_dying(5);

    g_cpu_host_current = g_cpu_boot;
    set_current_thread(nullptr);
    reap_dead_threads(frames);

    EXPECT_EQ(ThreadState::Blocked, parent_thread->state);
    EXPECT_EQ(ThreadState::Dying, child_thread->state);
    EXPECT_EQ(ProcessState::Zombie, child->state);
    EXPECT_EQ(child_thread, cpus.ap.current_thread);

    const auto saved_rax = parent_thread->frame.rax;
    cpus.ap.current_thread = parent_thread;
    reap_dead_threads(frames);
    EXPECT_EQ(ThreadState::Blocked, parent_thread->state);
    EXPECT_EQ(saved_rax, parent_thread->frame.rax);
    EXPECT_TRUE(child->exit_cleanup_complete);

    cpus.ap.current_thread = nullptr;
    reap_dead_threads(frames);

    EXPECT_EQ(ThreadState::Ready, parent_thread->state);
    EXPECT_EQ(child_pid, parent_thread->frame.rax);
    EXPECT_EQ(parent, first_process());
    EXPECT_EQ(nullptr, next_process(parent));

    parent_thread->state = ThreadState::Dying;
    parent->state = ProcessState::Dying;
    reap_dead_threads(frames);
    EXPECT_EQ(nullptr, first_process());
}

TEST(TaskRegistry, WaitpidWakeDuringScheduleKeepsCurrentParentRunnable)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    page_frames = initialized_frames();
    kmem_init(page_frames);
    ASSERT_TRUE(init_tasks(page_frames));

    Process* kernel_process = create_kernel_process(0);
    ASSERT_NE(nullptr, kernel_process);
    Thread* idle = create_kernel_thread(kernel_process, noop_thread_entry, page_frames);
    ASSERT_NE(nullptr, idle);
    Process* parent = create_user_process("parent", 0);
    ASSERT_NE(nullptr, parent);
    Process* child = create_user_process("child", 0);
    ASSERT_NE(nullptr, child);
    child->parent = parent;
    const uint64_t child_pid = child->pid;
    Thread* parent_thread = create_user_thread(parent, 0x1000, 0x2000, page_frames, false);
    ASSERT_NE(nullptr, parent_thread);
    Thread* child_thread = create_user_thread(child, 0x1000, 0x3000, page_frames, false);
    ASSERT_NE(nullptr, child_thread);

    set_current_thread(child_thread);
    mark_current_thread_dying(7);
    set_current_thread(parent_thread);
    long result = -1;
    ASSERT_FALSE(try_complete_or_block_wait_pid(page_frames, parent_thread, child_pid, 0, result));
    ASSERT_EQ(ThreadState::Blocked, parent_thread->state);

    // Scheduling reaps the child and wakes the parent before the CPU handoff.
    Thread* selected = schedule_next(false);
    EXPECT_EQ(parent_thread, selected);
    EXPECT_EQ(child_pid, parent_thread->frame.rax);
    EXPECT_EQ(ThreadWaitReason::None, parent_thread->wait.reason);
    EXPECT_EQ(ThreadState::Running, parent_thread->state);

    set_current_thread(nullptr);
    parent_thread->state = ThreadState::Dying;
    parent->state = ProcessState::Dying;
    idle->state = ThreadState::Dying;
    reap_dead_threads(page_frames);
    EXPECT_EQ(nullptr, first_thread());
    EXPECT_EQ(nullptr, first_process());
    page_frames = {};
}

TEST(TaskRegistry, ReapingUserProcessTriggersTlbShootdown)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);

    ASSERT_TRUE(init_tasks(frames));
    ASSERT_TRUE(ipi_initialize());
    lapic_stub_reset();

    VirtualMemory vm(frames);
    ASSERT_TRUE(vm.allocate_and_map(
        kUserImageBase, 1, PageFlags::Present | PageFlags::User | PageFlags::Write));

    Process* process = create_user_process("worker", vm.root());
    ASSERT_NE(nullptr, process);

    EXPECT_TRUE(reap_process(process, frames));
    EXPECT_EQ(1u, lapic_stub_icr_send_count());
    EXPECT_EQ(ipi_tlb_shootdown_vector(), static_cast<uint8_t>(lapic_stub_last_icr_low()));
}

TEST(TaskRegistry, WaitpidRequiresCleanupPublicationAndPreservesStatusOnCopyFailure)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = initialized_frames();
    kmem_init(frames);
    ASSERT_TRUE(init_tasks(frames));

    VirtualMemory parent_vm(frames);
    ASSERT_TRUE(parent_vm.allocate_and_map(kUserImageBase, 1, PageFlags::User | PageFlags::Write));
    Process* parent = create_user_process("parent", parent_vm.root());
    Process* child = create_user_process("child", 0, parent);
    ASSERT_NE(nullptr, parent);
    ASSERT_NE(nullptr, child);
    child->exit_status = 17;
    child->state = ProcessState::Zombie;
    const auto pid = child->pid;
    Thread waiter{};
    waiter.process = parent;
    waiter.address_space_cr3 = parent_vm.root();

    // Model the interval after thread removal and before cleanup publication.
    long result = -1;
    EXPECT_FALSE(try_complete_wait_pid(frames, &waiter, pid, 0, result));
    ASSERT_EQ(child, next_process(parent));

    child->exit_cleanup_complete = true;
    EXPECT_TRUE(try_complete_wait_pid(frames, &waiter, pid, 1, result));
    EXPECT_EQ(-1, result);
    ASSERT_EQ(child, next_process(parent));
    EXPECT_TRUE(try_complete_wait_pid(frames, &waiter, pid, kUserImageBase, result));
    int status = 0;
    EXPECT_TRUE(copy_from_user(frames, &waiter, kUserImageBase, &status, sizeof(status)));
    EXPECT_EQ(17, status);
    EXPECT_EQ(static_cast<long>(pid), result);
    EXPECT_EQ(nullptr, next_process(parent));
    EXPECT_TRUE(reap_process(parent, frames));
}
