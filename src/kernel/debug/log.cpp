#include "debug/log.hpp"

#include <os1/console.h>
#include <os1/observe.h>
#include <os1/resource.h>
#include <os1/syscall_numbers.h>
#include <os1/tmpfs.h>

#include "core/kernel_state.hpp"
#include "core/wall_clock.hpp"
#include "log/ring.hpp"
#include "mm/kmem.hpp"
#include "mm/user_copy.hpp"
#include "proc/thread.hpp"
#include "util/memory.h"
#ifndef OS1_HOST_TEST
#include "arch/x86_64/cpu/cpu.hpp"
#endif
namespace diagnostic_log
{
namespace
{
Spinlock lock{"diagnostic-log"};
os1::log::Ring ring;
uint64_t dropped = 0, identity = 1, boot_id = 0, clock_ns = 0;
unsigned histories = 0;
bool enabled = false, diagnostic = false;
os1_log_record text_record{};
struct History
{
    os1_log_history_record records[32]{};
    uint64_t next = 1, overwritten = 0;
};
uint64_t now()
{
    return __atomic_load_n(&clock_ns, __ATOMIC_RELAXED);
}
uint64_t new_id()
{
    return __atomic_fetch_add(&identity, 1ull, __ATOMIC_RELAXED);
}
Process* current()
{
#ifdef OS1_HOST_TEST
    return nullptr;
#else
    auto* t = g_cpu_boot ? current_thread() : nullptr;
    return t ? t->process : nullptr;
#endif
}
unsigned cpu_id()
{
#ifdef OS1_HOST_TEST
    return 0;
#else
    return g_cpu_boot ? cpu_cur()->id : 0;
#endif
}
void append_locked(os1_log_record r)
{
    r.time_ns = now();
    r.flags |= OS1_LOG_COARSE;
    r.cpu = cpu_id();
    if(!ring.append(r))
        __atomic_fetch_add(&dropped, 1ull, __ATOMIC_RELAXED);
}
size_t copy_text(char* dst, size_t capacity, const char* src, size_t limit, uint32_t& flags)
{
    size_t n = 0;
    while(n + 1 < capacity && n < limit && src[n])
    {
        dst[n] = src[n];
        ++n;
    }
    dst[n] = 0;
    if(n == limit || (n + 1 == capacity && src[n]))
        flags |= OS1_LOG_TRUNCATED;
    return n;
}
}  // namespace
void tick(uint64_t elapsed_ns)
{
    __atomic_fetch_add(&clock_ns, elapsed_ns, __ATOMIC_RELAXED);
}
#ifdef OS1_HOST_TEST
void test_reset()
{
    enabled = false;
    ring.initialize(nullptr, 0);
    dropped = 0;
    identity = 1;
    clock_ns = 0;
    text_record = {};
}
#endif
void initialize(bool trace)
{
    diagnostic = trace;
    size_t bytes = trace ? 1024 * 1024 : 256 * 1024;
    auto* storage = static_cast<os1_log_record*>(kmalloc(bytes, KmallocFlags::Zero));
    if(!storage)
        return;
    ring.initialize(storage, bytes / sizeof(*storage));
#ifndef OS1_HOST_TEST
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    boot_id = (uint64_t(hi) << 32) | lo;
#endif
    enabled = true;
    auto r = origin(nullptr, OS1_LOG_TEXT);
    const char label[] = "memory logging initialized";
    memcpy(r.text, label, sizeof(label));
    r.text_bytes = sizeof(label) - 1;
    append(r);
}
os1_log_record origin(Process* p, uint32_t kind)
{
    os1_log_record r{};
    r.kind = kind;
    r.level = OS1_LOG_INFO_LEVEL;
    r.time_ns = now();
    if(p)
    {
        r.pid = p->pid;
        r.session_id = p->log_session;
        r.command_id = p->log_command;
        r.parent_command_id = p->log_parent_command;
    }
    return r;
}
void append(os1_log_record r)
{
    if(!enabled)
        return;
    IrqGuard irq;
    if(!lock.try_lock())
    {
        __atomic_fetch_add(&dropped, 1ull, __ATOMIC_RELAXED);
        return;
    }
    append_locked(r);
    lock.unlock();
}
void text(char c)
{
    if(!enabled || c == '\r')
        return;
    IrqGuard irq;
    if(!lock.try_lock())
    {
        __atomic_fetch_add(&dropped, 1ull, __ATOMIC_RELAXED);
        return;
    }
    auto r = origin(current(), OS1_LOG_TEXT);
    if(text_record.text_bytes && (r.pid != text_record.pid || cpu_id() != text_record.cpu))
    {
        text_record.flags |= OS1_LOG_TRUNCATED;
        append_locked(text_record);
        text_record = {};
    }
    if(!text_record.text_bytes)
    {
        text_record = r;
        text_record.cpu = cpu_id();
    }
    if(c != '\n')
        text_record.text[text_record.text_bytes++] = c;
    if(c == '\n' || text_record.text_bytes == sizeof(text_record.text))
    {
        if(c != '\n')
            text_record.flags |= OS1_LOG_TRUNCATED;
        append_locked(text_record);
        text_record = {};
    }
    lock.unlock();
}
void event(uint32_t type, uint32_t flags, uint64_t pid, uint64_t a, uint64_t b)
{
    // High-frequency scheduler/IRQ records stay in the existing observation ring.
    if(type != OS1_KERNEL_EVENT_TRAP && type != OS1_KERNEL_EVENT_PCI_BIND &&
       type != OS1_KERNEL_EVENT_KMEM_CORRUPTION && type != OS1_KERNEL_EVENT_USER_COPY_FAILURE &&
       type != OS1_KERNEL_EVENT_TIMER_SOURCE && !(flags & OS1_KERNEL_EVENT_FLAG_FAILURE))
        return;
    auto r = origin(current(), OS1_LOG_EVENT);
    r.pid = pid;
    r.arg0 = a;
    r.arg1 = b;
    r.interface_id = type;
    if(type == OS1_KERNEL_EVENT_TRAP || type == OS1_KERNEL_EVENT_KMEM_CORRUPTION ||
       type == OS1_KERNEL_EVENT_USER_COPY_FAILURE || (flags & OS1_KERNEL_EVENT_FLAG_FAILURE))
        r.level = OS1_LOG_ERROR;
    append(r);
}
void inherit(Process* child, const Process* parent)
{
    if(parent)
    {
        child->log_session = parent->log_session;
        child->log_command = parent->log_command;
        child->log_parent_command = parent->log_parent_command;
    }
    append(origin(child, OS1_LOG_PROCESS_START));
}
void cleanup(Process* p)
{
    if(!p || !p->log_history)
        return;
    auto* h = p->log_history;
    p->log_history = nullptr;
    kfree(h);
    IrqSpinGuard guard(lock);
    --histories;
}
void process_exec(Process* p)
{
    if(!p || !p->log_command_active)
        return;
    if(p->log_history)
    {
        auto& h = *static_cast<History*>(p->log_history);
        for(auto& row : h.records)
            if(row.record.command_id == p->log_command)
            {
                row.record.flags = (row.record.flags & ~OS1_LOG_PENDING) | OS1_LOG_REPLACED;
                row.completed_ns = now();
            }
    }
    auto r = origin(p, OS1_LOG_COMMAND);
    r.flags |= OS1_LOG_REPLACED;
    r.arg0 = 2;
    append(r);
    // The replacement inherits the exec origin but owns no old active scope.
    p->log_command_active = false;
}
void process_exit(Process* p, int status)
{
    auto r = origin(p, OS1_LOG_PROCESS_EXIT);
    r.status = status;
    if(status)
        r.level = OS1_LOG_WARNING;
    append(r);
}
void syscall_begin(Thread* t, uint64_t number, uint64_t operation)
{
    // IPC Operations have their own retained origin. Queries never trace themselves.
    if(number == os1_sys_log)
        return;
    t->log_pending = true;
    t->log_trace = origin(t->process, OS1_LOG_SYSCALL_BEGIN);
    t->log_trace.request_id = new_id();
    t->log_trace.arg0 = number;
    t->log_trace.arg1 = operation;
    if(diagnostic && number != os1_sys_ipc)
        append(t->log_trace);
}
void syscall_end(Thread* t, int64_t status)
{
    if(!t || !t->log_pending)
        return;
    t->log_pending = false;
    auto r = t->log_trace;
    r.kind = OS1_LOG_SYSCALL_END;
    r.status = status;
    r.duration_ns = now() - r.time_ns;
    if(r.arg0 == os1_sys_ipc)
    {
        if(status < 0 && status != OS1_ERR_WOULD_BLOCK)
        {
            r.level = OS1_LOG_WARNING;
            append(r);
        }
        return;
    }
    if(status < 0)
        r.level = OS1_LOG_WARNING;
    if(diagnostic || status < 0 || r.arg0 == os1_sys_launch || r.arg0 == os1_sys_spawn ||
       r.arg0 == os1_sys_exec || r.arg0 == os1_sys_exit || r.arg0 == os1_sys_resource)
        append(r);
}
bool trace_ipc(const os1_log_record& r)
{
    if(diagnostic)
        return true;
    if(r.interface_id == OS1_CONSOLE_INTERFACE_ID ||
       r.interface_id == OS1_CONSOLE_INPUT_INTERFACE_ID)
        return false;
    if(r.interface_id == OS1_FILE_INTERFACE_ID)
        return r.arg0 == OS1_FILE_WRITE || r.arg0 == OS1_FILE_APPEND ||
               r.arg0 == OS1_FILE_TRUNCATE || r.arg0 == OS1_FILE_SYNC ||
               r.arg0 == OS1_FILE_SET_TIMES;
    if(r.interface_id == OS1_DIRECTORY_INTERFACE_ID)
        return r.arg0 == OS1_DIRECTORY_CREATE || r.arg0 == OS1_DIRECTORY_REMOVE ||
               r.arg0 == OS1_DIRECTORY_RENAME || r.arg0 == OS1_DIRECTORY_REMOVE_TYPED ||
               r.arg0 == OS1_DIRECTORY_SYNC || r.arg0 == OS1_DIRECTORY_SET_TIMES;
    return true;  // Unknown service methods can mutate resources.
}
void ipc_begin(os1_log_record& r, Process* p, uint64_t interface_id, uint32_t member)
{
    r = origin(p, OS1_LOG_IPC_BEGIN);
    if(interface_id == OS1_LOG_INTERFACE_ID)
    {
        r.kind = 0;
        return;
    }
    r.request_id = new_id();
    r.interface_id = interface_id;
    r.arg0 = member;
    if(trace_ipc(r))
        append(r);
}
void ipc_end(const os1_log_record& origin_record, int64_t status)
{
    if(!origin_record.kind)
        return;
    if(!diagnostic && origin_record.interface_id == OS1_CONSOLE_INPUT_INTERFACE_ID &&
       status == OS1_ERR_WOULD_BLOCK)
        return;  // An empty key queue is normal while an interactive shell waits.
    auto r = origin_record;
    r.kind = OS1_LOG_IPC_END;
    r.status = status;
    r.duration_ns = now() - r.time_ns;
    if(status < 0)
        r.level = OS1_LOG_WARNING;
    if(status < 0 || trace_ipc(r))
        append(r);
}
long read(const os1_log_query& q, os1_log_record& out)
{
    if(!os1::log::valid(q))
        return OS1_ERR_INVALID;
    uint64_t after = q.after, through;
    {
        IrqSpinGuard guard(lock);
        through = q.through ? q.through : ring.latest();
    }
    // At most the ring capacity can exist in this frozen sequence range.
    for(unsigned i = 0; i < 4096; ++i)
    {
        {
            IrqSpinGuard guard(lock);
            if(!ring.next(after, through, out))
                return 0;
        }
        after = out.sequence;
        if(os1::log::matches(out, q))
            return 1;
    }
    return 0;
}
os1_log_info info()
{
    os1_log_info r{};
    r.version = 1;
    r.size = sizeof(r);
    {
        IrqSpinGuard guard(lock);
        r.latest = ring.latest();
        r.overwritten = ring.overwritten();
        r.capacity_bytes = ring.bytes();
        r.history_slots = histories;
    }
    r.boot_id = boot_id;
    r.dropped = __atomic_load_n(&dropped, __ATOMIC_RELAXED);
    r.clock_resolution_ns = 1000000;
    r.diagnostic = diagnostic;
    wall_clock_now(r.wall_time_ns);
    return r;
}
long context(PageFrameContainer& frames, Thread* t, uint64_t op, uint64_t pointer)
{
    auto* p = t->process;
    if(!p)
        return OS1_ERR_INVALID;
    if(op == OS1_LOG_CONTEXT_OPEN)
    {
        if(p->log_history)
            return 0;
        {
            IrqSpinGuard guard(lock);
            if(histories == 4)
                return OS1_ERR_LIMIT;
            ++histories;
        }
        auto* h = static_cast<History*>(kmalloc(sizeof(History), KmallocFlags::Zero));
        if(!h)
        {
            IrqSpinGuard guard(lock);
            --histories;
            return OS1_ERR_NO_MEMORY;
        }
        h->next = 1;
        p->log_history = h;
        return 0;
    }
    if(op == OS1_LOG_CONTEXT_INFO)
    {
        auto r = info();
        r.dropped = 0;
        if(p->log_history)
        {
            auto& h = *static_cast<History*>(p->log_history);
            r.latest = h.next - 1;
            r.overwritten = h.overwritten;
            r.capacity_bytes = OS1_LOG_HISTORY_BYTES;
        }
        else
        {
            r.latest = r.overwritten = r.capacity_bytes = 0;
        }
        return copy_to_user(frames, t, pointer, &r, sizeof(r)) ? 0 : OS1_ERR_FAULT;
    }
    if(op == OS1_LOG_COMMAND_BEGIN || op == OS1_LOG_COMMAND_END)
    {
        os1_log_command command{};
        if(!copy_from_user(frames, t, pointer, &command, sizeof(command)))
            return OS1_ERR_FAULT;
        if(command.version != 1 || command.size != sizeof(command) || command.reserved ||
           (command.flags & ~OS1_LOG_SUPPRESSED))
            return OS1_ERR_INVALID;
        if(op == OS1_LOG_COMMAND_BEGIN)
        {
            if(command.command_id || p->log_command_active)
                return OS1_ERR_INVALID;
            // Validate output before committing command identity.
            command.command_id = new_id();
            if(!copy_to_user(frames, t, pointer, &command, sizeof(command)))
                return OS1_ERR_FAULT;
            p->log_saved_session = p->log_session;
            p->log_saved_command = p->log_command;
            p->log_saved_parent = p->log_parent_command;
            p->log_command_active = true;
            p->log_parent_command = p->log_command;
            p->log_session = p->pid;
            p->log_command = command.command_id;
            if(p->log_history)
            {
                auto& h = *static_cast<History*>(p->log_history);
                auto& row = h.records[(h.next - 1) % 32];
                row = {};
                row.record = origin(p, OS1_LOG_COMMAND);
                row.record.sequence = h.next++;
                row.record.time_ns = now();
                row.record.flags = OS1_LOG_COARSE | OS1_LOG_PENDING | command.flags;
                if(!(command.flags & OS1_LOG_SUPPRESSED))
                {
                    copy_text(row.text,
                              sizeof(row.text),
                              command.text,
                              sizeof(command.text),
                              row.record.flags);
                    copy_text(row.cwd,
                              sizeof(row.cwd),
                              command.cwd,
                              sizeof(command.cwd),
                              row.record.flags);
                }
                if(h.next > 33)
                    ++h.overwritten;
            }
            append(origin(p, OS1_LOG_COMMAND));
            return 0;
        }
        if(!p->log_command_active || !command.command_id || command.command_id != p->log_command)
            return OS1_ERR_INVALID;
        if(p->log_history)
        {
            auto& h = *static_cast<History*>(p->log_history);
            for(auto& row : h.records)
                if(row.record.command_id == command.command_id)
                {
                    row.record.flags &= ~OS1_LOG_PENDING;
                    row.record.status = command.status;
                    row.record.level = command.status ? OS1_LOG_WARNING : OS1_LOG_INFO_LEVEL;
                    row.completed_ns = now();
                }
        }
        auto r = origin(p, OS1_LOG_COMMAND);
        r.status = command.status;
        r.arg0 = 1;
        append(r);
        p->log_session = p->log_saved_session;
        p->log_command = p->log_saved_command;
        p->log_parent_command = p->log_saved_parent;
        p->log_command_active = false;
        return 0;
    }
    if(op == OS1_LOG_HISTORY_READ)
    {
        os1_log_history_query result{};
        if(!copy_from_user(frames, t, pointer, &result.query, sizeof(result.query)))
            return OS1_ERR_FAULT;
        if(!os1::log::valid(result.query))
            return OS1_ERR_INVALID;
        if(!p->log_history)
            return 0;
        auto& h = *static_cast<History*>(p->log_history);
        auto& q = result.query;
        uint64_t end = q.through && q.through < h.next ? q.through : h.next - 1;
        uint64_t first = h.next > 32 ? h.next - 32 : 1;
        for(uint64_t seq = first; seq <= end; ++seq)
        {
            auto& row = h.records[(seq - 1) % 32];
            if(seq <= q.after || !os1::log::matches(row.record, q))
                continue;
            result.result = row;
            return copy_to_user(frames, t, pointer, &result, sizeof(result)) ? 1 : OS1_ERR_FAULT;
        }
        return 0;
    }
    return OS1_ERR_UNSUPPORTED;
}
}  // namespace diagnostic_log
