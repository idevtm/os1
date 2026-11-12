#include "debug/event_ring.hpp"

#include "debug/log.hpp"
#include "sync/smp.hpp"

#if !defined(OS1_HOST_TEST)
#include "arch/x86_64/cpu/cpu.hpp"
#include "core/kernel_state.hpp"
#include "proc/thread.hpp"
#endif

namespace kernel_event
{
namespace
{
KernelEventRing g_event_ring;

Spinlock g_event_ring_lock("event_ring");

#if !defined(OS1_HOST_TEST)

[[nodiscard]] uint32_t current_cpu_id()
{
    return (nullptr != g_cpu_boot) ? static_cast<uint32_t>(cpu_cur()->id) : 0;
}

[[nodiscard]] Thread* safe_current_thread()
{
    return (nullptr != g_cpu_boot) ? current_thread() : nullptr;
}
#endif

[[nodiscard]] uint32_t min_u32(uint32_t left, uint32_t right)
{
    return (left < right) ? left : right;
}
}  // namespace

void KernelEventRing::reset()
{
    for(uint32_t i = 0; i < kEventRingCapacity; ++i)
    {
        records_[i] = {};
    }
    next_sequence_ = 1;
    head_ = 0;
    count_ = 0;
}

Os1ObserveEventRecord KernelEventRing::append(uint64_t tick_count,
                                              uint32_t type,
                                              uint32_t flags,
                                              uint32_t cpu,
                                              uint64_t pid,
                                              uint64_t tid,
                                              uint64_t arg0,
                                              uint64_t arg1,
                                              uint64_t arg2,
                                              uint64_t arg3)
{
    Os1ObserveEventRecord record{};
    record.sequence = next_sequence_++;
    record.tick_count = tick_count;
    record.pid = pid;
    record.tid = tid;
    record.arg0 = arg0;
    record.arg1 = arg1;
    record.arg2 = arg2;
    record.arg3 = arg3;
    record.type = type;
    record.flags = flags;
    record.cpu = cpu;

    records_[head_] = record;
    head_ = (head_ + 1) % kEventRingCapacity;
    if(count_ < kEventRingCapacity)
    {
        ++count_;
    }
    return record;
}

uint32_t KernelEventRing::snapshot(Os1ObserveEventRecord* records, uint32_t max_records) const
{
    if((nullptr == records) || (0 == max_records))
    {
        return 0;
    }

    const uint32_t copy_count = min_u32(count_, max_records);
    const uint32_t start = (head_ + kEventRingCapacity - copy_count) % kEventRingCapacity;
    for(uint32_t i = 0; i < copy_count; ++i)
    {
        records[i] = records_[(start + i) % kEventRingCapacity];
    }
    return copy_count;
}

uint32_t KernelEventRing::count() const
{
    return count_;
}

uint64_t KernelEventRing::next_sequence() const
{
    return next_sequence_;
}

void record(
    uint32_t type, uint32_t flags, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
#if defined(OS1_HOST_TEST)
    record_subject(type, flags, 0, 0, arg0, arg1, arg2, arg3);
#else
    uint64_t pid = 0;
    uint64_t tid = 0;

    IrqSpinGuard guard(g_event_ring_lock);
    Thread* thread = safe_current_thread();
    if(nullptr != thread)
    {
        tid = thread->tid;
        if(nullptr != thread->process)
        {
            pid = thread->process->pid;
        }
    }
    diagnostic_log::event(type, flags, pid, arg0, arg1, thread ? thread->process : nullptr);
    g_event_ring.append(
        g_timer_ticks, type, flags, current_cpu_id(), pid, tid, arg0, arg1, arg2, arg3);
#endif
}

void record_subject(uint32_t type,
                    uint32_t flags,
                    uint64_t pid,
                    uint64_t tid,
                    uint64_t arg0,
                    uint64_t arg1,
                    uint64_t arg2,
                    uint64_t arg3)
{
    IrqSpinGuard guard(g_event_ring_lock);
#if defined(OS1_HOST_TEST)
    g_event_ring.append(0, type, flags, 0, pid, tid, arg0, arg1, arg2, arg3);
#else
    Thread* thread = safe_current_thread();
    diagnostic_log::event(type, flags, pid, arg0, arg1, thread ? thread->process : nullptr);
    g_event_ring.append(
        g_timer_ticks, type, flags, current_cpu_id(), pid, tid, arg0, arg1, arg2, arg3);
#endif
}

bool snapshot_all(Os1ObserveEventRecord* records, uint32_t capacity, uint32_t& count)
{
    IrqSpinGuard guard(g_event_ring_lock);
    count = g_event_ring.count();
    if(count > capacity)
        return false;
    return g_event_ring.snapshot(records, capacity) == count;
}

}  // namespace kernel_event
