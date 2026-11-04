// Thread object model and the public scheduler-facing task API. Process
// ownership lives in proc/process.hpp; this header owns Thread layout, wait
// state, and the assembly-visible offsets used by context-switch code.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/interrupt/trap_frame.hpp"
#include "mm/page_frame.hpp"
#include "proc/process.hpp"
#include "sync/smp.hpp"

struct cpu;
struct Completion;

extern Spinlock g_thread_registry_lock;

constexpr uint16_t kKernelCodeSegment = 0x08;
constexpr uint16_t kKernelDataSegment = 0x10;
constexpr uint16_t kUserDataSegment = 0x1B;
constexpr uint16_t kUserCodeSegment = 0x23;

enum class ThreadState : uint32_t
{
    Free = 0,
    Ready = 1,
    Running = 2,
    Blocked = 3,
    Dying = 4,
};

enum class ThreadWaitReason : uint32_t
{
    None = 0,
    ConsoleRead = 1,
    ChildExit = 2,
    BlockIo = 3,
};

struct ConsoleReadWaitState
{
    OpenInstance* instance = nullptr;
    uint64_t user_buffer = 0;
    uint64_t length = 0;
};

struct ChildExitWaitState
{
    uint64_t user_status_pointer = 0;
    uint64_t pid = 0;
};

struct BlockIoWaitState
{
    Completion* completion = nullptr;
};

struct ThreadWaitState
{
    ThreadWaitReason reason = ThreadWaitReason::None;
    uint32_t reserved = 0;
    union
    {
        ConsoleReadWaitState console_read;
        ChildExitWaitState child_exit;
        BlockIoWaitState block_io;
    };
};

struct Thread
{
    Thread* next = nullptr;
    uint64_t tid = 0;
    Process* process = nullptr;
    ThreadState state = ThreadState::Free;
    bool user_mode = false;
    uint16_t reserved0 = 0;
    uint32_t reserved1 = 0;
    uint64_t address_space_cr3 = 0;
    uint64_t kernel_stack_base = 0;
    uint64_t kernel_stack_top = 0;
    int exit_status = 0;
    TrapFrame frame{};
    ThreadWaitState wait{};
    Thread* registry_next = nullptr;
    Thread* wait_link = nullptr;
    volatile uint32_t io_pins = 0;
    bool console_cancelled = false;
    cpu* scheduler_cpu = nullptr;
    cpu* run_queue_cpu = nullptr;
    uint64_t affinity_mask = ~0ull;
    uint64_t last_migration_tick = 0;
};

#define THREAD_STATIC_ASSERT(name, expr) typedef char thread_static_assert_##name[(expr) ? 1 : -1]

THREAD_STATIC_ASSERT(frame_offset, offsetof(Thread, frame) == 72);
THREAD_STATIC_ASSERT(cr3_offset, offsetof(Thread, address_space_cr3) == 40);
THREAD_STATIC_ASSERT(stack_top_offset, offsetof(Thread, kernel_stack_top) == 56);

#undef THREAD_STATIC_ASSERT

// Initialize the kmem-backed process and thread registries.
bool init_tasks(PageFrameContainer& frames);
// Create a kernel-mode thread with a bootstrap frame that enters `entry`.
Thread* create_kernel_thread(Process* process, void (*entry)(void), PageFrameContainer& frames);
// Create or return the idle thread assigned to one CPU.
Thread* create_idle_thread_for_cpu(Process* process,
                                   cpu* owner,
                                   void (*entry)(void),
                                   PageFrameContainer& frames);
// Create a user-mode thread with an interrupt-return frame for ring 3 entry.
Thread* create_user_thread(Process* process,
                           uint64_t user_rip,
                           uint64_t user_rsp,
                           PageFrameContainer& frames,
                           bool start_ready = true);
// Iterate the live thread registry in creation order.
Thread* first_thread(void);
Thread* next_thread(const Thread* thread);
// Return the thread bound to the current CPU.
Thread* current_thread(void);
// Return the scheduler's idle thread.
Thread* idle_thread(void);
// Return the idle thread assigned to `owner`, if any.
Thread* idle_thread_for_cpu(const cpu* owner);
// Find the next runnable thread after `after` in circular table order.
Thread* next_runnable_thread(Thread* after);
// Rebuild next pointers for runnable threads after state changes.
void relink_runnable_threads();
// Bind the current CPU to `thread`.
void set_current_thread(Thread* thread);
// Mark a thread ready and update its owning process state if needed.
void mark_thread_ready(Thread* thread, cpu* target = nullptr);
// Block the current thread until a block-I/O completion flag is signaled.
void block_current_thread_on_block_io(Completion* completion);
// clear a thread's wait metadata after the wait has completed.
void clear_thread_wait(Thread* thread);
// Wake a blocked thread, preserving the currently running thread state when the
// wake happens from an interrupt on that same thread.
void wake_blocked_thread(Thread* thread, cpu* target = nullptr);
// Return the first thread blocked on a wait reason.
Thread* first_blocked_thread(ThreadWaitReason reason);
// Wake any thread blocked on a matching block-I/O completion flag.
void wake_block_io_waiters(Completion* completion);
// Mark the current thread dying and publish its process exit status.
void mark_current_thread_dying(int exit_status);
// Reclaim a thread record after its stack and owner state have been torn down.
void clear_thread(Thread* thread);
// Reclaim dying threads and their associated process state when possible.
void reap_dead_threads(PageFrameContainer& frames);
// Count threads currently eligible to run.
size_t runnable_thread_count(void);
// Return the number of ready threads queued on `owner`.
size_t cpu_run_queue_length(const cpu* owner);
// Return the first runnable user thread, if any.
Thread* first_runnable_user_thread(void);

#ifdef __cplusplus
extern "C"
{
#endif

    // Assembly context-switch entry: resume execution on the supplied thread frame.
    void start_multi_task(Thread* thread);
    // Move the BSP onto a mapped kernel stack before restoring the first thread.
    void enter_first_thread(Thread* thread, uint64_t stack_top);

#ifdef __cplusplus
}
#endif
