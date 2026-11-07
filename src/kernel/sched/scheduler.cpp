// Round-robin scheduler handoff wrapper around the current task-table API.
#include "sched/scheduler.hpp"

#include "arch/x86_64/cpu/cpu.hpp"
#include "core/kernel_state.hpp"
#include "debug/event_ring.hpp"
#include "proc/thread.hpp"
#include "syscall/wait.hpp"

namespace
{
constexpr uint64_t kBalanceIntervalTicks = 64;
constexpr uint64_t kIdleBalanceTriggerTicks = 4;
constexpr uint64_t kMigrationCooldownTicks = 64;

bool g_smp_balancer_enabled = true;

[[nodiscard]] uint64_t thread_pid(const Thread* thread)
{
    return (thread && thread->process) ? thread->process->pid : 0;
}

[[nodiscard]] uint64_t thread_tid(const Thread* thread)
{
    return thread ? thread->tid : 0;
}

[[nodiscard]] bool cpu_is_schedulable(const cpu* owner)
{
    return (nullptr != owner) && ((owner == g_cpu_boot) || (0u != owner->booted));
}

[[nodiscard]] size_t schedulable_cpu_count()
{
    size_t count = 0;
    for(cpu* owner = g_cpu_boot; nullptr != owner; owner = owner->next)
    {
        if(cpu_is_schedulable(owner))
        {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] size_t snapshot_run_queue_length(cpu* owner)
{
    if(nullptr == owner)
    {
        return 0u;
    }

    IrqSpinGuard guard(owner->runq.lock);
    return owner->runq.length;
}

[[nodiscard]] bool thread_allows_cpu(const Thread* thread, const cpu* owner)
{
    if((nullptr == thread) || (nullptr == owner))
    {
        return false;
    }
    if(owner->id >= 64)
    {
        return true;
    }
    return 0u != (thread->affinity_mask & (1ull << owner->id));
}

[[nodiscard]] bool thread_is_migratable(const Thread* thread, const cpu* owner, uint64_t now)
{
    if((nullptr == thread) || (ThreadState::Ready != thread->state) || !thread_allows_cpu(thread, owner))
    {
        return false;
    }
    if(0u == thread->last_migration_tick)
    {
        return true;
    }
    return (now - thread->last_migration_tick) >= kMigrationCooldownTicks;
}

Thread* detach_last_migratable_thread_locked(cpu* donor, const cpu* receiver, uint64_t now)
{
    if((nullptr == donor) || (nullptr == receiver))
    {
        return nullptr;
    }

    Thread* previous = nullptr;
    Thread* candidate_previous = nullptr;
    Thread* candidate = nullptr;
    for(Thread* thread = donor->runq.head; nullptr != thread; thread = thread->next)
    {
        // schedule_next queues its current thread before assembly leaves its stack.
        // The donor lock prevents a new dequeue while we inspect that publication.
        // restore_thread publishes the successor only after leaving the old stack/CR3.
        if(thread != __atomic_load_n(&donor->current_thread, __ATOMIC_ACQUIRE) &&
           thread_is_migratable(thread, receiver, now))
        {
            candidate_previous = previous;
            candidate = thread;
        }
        previous = thread;
    }

    if(nullptr == candidate)
    {
        return nullptr;
    }

    Thread* next = candidate->next;
    if(nullptr != candidate_previous)
    {
        candidate_previous->next = next;
    }
    else
    {
        donor->runq.head = next;
    }
    if(donor->runq.tail == candidate)
    {
        donor->runq.tail = candidate_previous;
    }
    candidate->next = nullptr;
    candidate->run_queue_cpu = nullptr;
    if(0u != donor->runq.length)
    {
        --donor->runq.length;
    }
    ++donor->dequeue_count;
    return candidate;
}

void append_ready_thread_locked(cpu* owner, Thread* thread)
{
    if((nullptr == owner) || (nullptr == thread))
    {
        return;
    }

    thread->next = nullptr;
    if(nullptr != owner->runq.tail)
    {
        owner->runq.tail->next = thread;
    }
    else
    {
        owner->runq.head = thread;
    }
    owner->runq.tail = thread;
    thread->run_queue_cpu = owner;
    ++owner->runq.length;
    ++owner->enqueue_count;
}

void unlock_run_queue_pair(cpu* first, cpu* second)
{
    if((nullptr != second) && (second != first))
    {
        second->runq.lock.unlock();
    }
    if(nullptr != first)
    {
        first->runq.lock.unlock();
    }
}

void record_run_queue_depth(cpu* owner, size_t depth)
{
    if(nullptr == owner)
    {
        return;
    }
    if(owner->last_recorded_runq_depth == depth)
    {
        return;
    }
    owner->last_recorded_runq_depth = depth;
    kernel_event::record(OS1_KERNEL_EVENT_RUNQ_DEPTH,
                         OS1_KERNEL_EVENT_FLAG_SUCCESS,
                         owner->id,
                         depth,
                         0,
                         0);
}
}  // namespace

bool scheduler_balance_cpu(cpu* target_cpu, uint64_t now, bool force)
{
    if(!g_smp_balancer_enabled || (nullptr == target_cpu) || !cpu_is_schedulable(target_cpu))
    {
        return false;
    }

    const size_t cpu_count = schedulable_cpu_count();
    if(cpu_count < 2u)
    {
        return false;
    }

    size_t total_depth = 0;
    size_t local_depth = 0;
    size_t donor_depth = 0;
    cpu* donor = nullptr;
    for(cpu* owner = g_cpu_boot; nullptr != owner; owner = owner->next)
    {
        if(!cpu_is_schedulable(owner))
        {
            continue;
        }

        const size_t depth = snapshot_run_queue_length(owner);
        total_depth += depth;
        if(owner == target_cpu)
        {
            local_depth = depth;
            continue;
        }
        if(depth > donor_depth)
        {
            donor = owner;
            donor_depth = depth;
        }
    }

    record_run_queue_depth(target_cpu, local_depth);
    if((nullptr == donor) || (0u == donor_depth) || (donor_depth <= (local_depth + 1u)))
    {
        return false;
    }

    const size_t target_depth = (total_depth + cpu_count - 1u) / cpu_count;
    if(!force && ((local_depth + 1u) >= target_depth))
    {
        return false;
    }

    cpu* first_lock = (target_cpu->id <= donor->id) ? target_cpu : donor;
    cpu* second_lock = (first_lock == target_cpu) ? donor : target_cpu;

    IrqGuard irq_guard;
    first_lock->runq.lock.lock();
    if(second_lock != first_lock)
    {
        second_lock->runq.lock.lock();
    }

    local_depth = target_cpu->runq.length;
    donor_depth = donor->runq.length;
    if((0u == donor_depth) || (donor_depth <= (local_depth + 1u)))
    {
        unlock_run_queue_pair(first_lock, second_lock);
        return false;
    }
    if(!force && ((local_depth + 1u) >= target_depth))
    {
        unlock_run_queue_pair(first_lock, second_lock);
        return false;
    }

    Thread* migrated = detach_last_migratable_thread_locked(donor, target_cpu, now);
    if(nullptr == migrated)
    {
        unlock_run_queue_pair(first_lock, second_lock);
        return false;
    }

    append_ready_thread_locked(target_cpu, migrated);
    migrated->scheduler_cpu = target_cpu;
    migrated->last_migration_tick = now;
    ++target_cpu->migrate_in;
    ++donor->migrate_out;
    kernel_event::record(OS1_KERNEL_EVENT_THREAD_MIGRATE,
                         OS1_KERNEL_EVENT_FLAG_SUCCESS,
                         donor->id,
                         target_cpu->id,
                         migrated->tid,
                         0);

    unlock_run_queue_pair(first_lock, second_lock);
    return true;
}

void scheduler_handle_timer_tick()
{
    cpu* local_cpu = cpu_cur();
    if(!g_smp_balancer_enabled || (nullptr == local_cpu))
    {
        return;
    }

    const size_t local_depth = snapshot_run_queue_length(local_cpu);
    const bool idle_without_work = (current_thread() == idle_thread()) && (0u == local_depth);
    if(idle_without_work)
    {
        ++local_cpu->balance_idle_ticks;
    }
    else
    {
        local_cpu->balance_idle_ticks = 0;
    }

    const bool periodic_check = (0u != local_cpu->timer_ticks) &&
                                (0u == (local_cpu->timer_ticks % kBalanceIntervalTicks));
    const bool idle_check = idle_without_work &&
                            (local_cpu->balance_idle_ticks >= kIdleBalanceTriggerTicks) &&
                            (0u == (local_cpu->balance_idle_ticks % kIdleBalanceTriggerTicks));
    if(periodic_check || idle_check)
    {
        (void)scheduler_balance_cpu(local_cpu, local_cpu->timer_ticks, idle_check);
    }
}

Thread* schedule_next(bool keep_current)
{
    Thread* current = current_thread();
    const bool was_blocked = current && (ThreadState::Blocked == current->state);
    if(cpu_on_boot())
    {
        reap_dead_threads(page_frames);
    }
    cpu* local_cpu = cpu_cur();
    // Reaping can complete the current thread's wait before the CPU handoff.
    if((keep_current || was_blocked) && current && (current != idle_thread()) &&
       (ThreadState::Running == current->state))
    {
        mark_thread_ready(current, local_cpu);
    }

    local_cpu->reschedule_pending = 0;
    Thread* next = next_runnable_thread(current);
    if(nullptr == next)
    {
        next = local_cpu->idle_thread;
    }
    if(nullptr != next)
    {
        next->state = ThreadState::Running;
        if((nullptr != next->process) && (ProcessState::Dying != next->process->state))
        {
            next->process->state = ProcessState::Running;
        }
    }
    if(next != current)
    {
        kernel_event::record(OS1_KERNEL_EVENT_SCHED_TRANSITION,
                             0,
                             thread_pid(current),
                             thread_tid(current),
                             thread_pid(next),
                             thread_tid(next));
    }
    return next;
}
