#include "ipc/channel.hpp"

#include "arch/x86_64/apic/ipi.hpp"
#include "arch/x86_64/cpu/cpu.hpp"
#include "mm/kmem.hpp"
#include "mm/user_copy.hpp"
#include "util/memory.h"
namespace
{
struct Operation;
struct Subscription;
struct Connection
{
    volatile uint32_t references;
    uint32_t live_operations;
    uint64_t generation, next_request;
    bool client_closed, provider_closed, ready;
    uint64_t interface_id;
    uint32_t interface_version;
    Operation* pending[OS1_IPC_OPERATION_LIMIT];
    Subscription* subscriptions[OS1_IPC_SUBSCRIPTION_LIMIT];
    uint64_t next_event;
};
struct Endpoint : NativeObject
{
    Connection* connection;
    bool provider;
};
struct Operation : NativeObject
{
    Connection* connection;
    bool counted, delivered, receiving, complete;
    os1_ipc_message request;
    os1_ipc_result result;
};
struct Subscription : NativeObject
{
    Connection* connection;
    uint32_t member, head, count;
    uint64_t dropped;
    bool active, receiving;
    os1_ipc_event events[OS1_IPC_EVENT_LIMIT];
};
// This lock protects all IPC state, registrations, and the deferred wake list.
// Table operations, user copies, allocation, destruction, and scheduling stay outside.
WaitQueue waiters{"ipc-state"};
WaitQueue signalled{"ipc-signalled"};
uint64_t next_generation = 1;
uint64_t monotonic_ns = 0;
void retain(Connection* c)
{
    (void)atomic_fetch_add(&c->references, 1u);
}
void release(Connection* c)
{
    if(__atomic_fetch_sub(&c->references, 1u, __ATOMIC_ACQ_REL) == 1u)
        kfree(c);
}
bool ready_locked(NativeObject* object)
{
    if(object->kind == OS1_OBJECT_OPERATION)
        return static_cast<Operation*>(object)->complete;
    if(object->kind == OS1_OBJECT_SUBSCRIPTION)
    {
        auto* sub = static_cast<Subscription*>(object);
        return !sub->active || sub->connection->provider_closed || (sub->count && !sub->receiving);
    }
    auto* e = static_cast<Endpoint*>(object);
    auto* c = e->connection;
    if(!e->provider)
        return c->ready || c->provider_closed;
    for(auto* op : c->pending)
        if(op && !op->delivered && !op->receiving)
            return true;
    return c->client_closed;
}
void signal_locked()
{
    Thread** link = &waiters.head;
    while(*link)
    {
        Thread* t = *link;
        if(ipc_stop_requested(t))
            t->wait.ipc.result = OS1_ERR_CANCELLED;
        else if(ready_locked(t->wait.ipc.instance->object))
            t->wait.ipc.result = 0;
        else if(t->wait.ipc.deadline_ns <= monotonic_ns)
            t->wait.ipc.result = OS1_ERR_TIMED_OUT;
        else
        {
            link = &t->wait_link;
            continue;
        }
        *link = t->wait_link;
        t->wait_link = nullptr;
        wait_queue_enqueue_locked(signalled, t);
    }
}
void finish_locked(Operation* op, int32_t transport)
{
    op->result = {};
    op->result.version = OS1_IPC_VERSION;
    op->result.size = sizeof(op->result);
    op->result.generation = op->request.generation;
    op->result.request_id = op->request.request_id;
    op->result.transport_status = transport;
    op->complete = true;
}
void destroy_operation(NativeObject* object)
{
    auto* op = static_cast<Operation*>(object);
    auto* c = op->connection;
    {
        IrqSpinGuard guard(waiters.lock);
        if(op->counted)
            --c->live_operations;
    }
    kfree(op);
    release(c);
}
void destroy_endpoint(NativeObject* object)
{
    auto* e = static_cast<Endpoint*>(object);
    auto* c = e->connection;
    Operation* dropped[OS1_IPC_OPERATION_LIMIT]{};
    {
        IrqSpinGuard guard(waiters.lock);
        if(e->provider)
        {
            c->provider_closed = true;
            for(unsigned i = 0; i < OS1_IPC_OPERATION_LIMIT; ++i)
            {
                auto* op = c->pending[i];
                if(!op)
                    continue;
                finish_locked(op, op->delivered ? OS1_ERR_OUTCOME_UNKNOWN : OS1_ERR_PEER_CLOSED);
                dropped[i] = op;
                c->pending[i] = nullptr;
            }
        }
        else
            c->client_closed = true;
        signal_locked();
    }
    for(auto* op : dropped)
        object_release(op);
    kfree(e);
    release(c);
    ipc_drain_wakeups();
}
bool zero_tail(const uint8_t* bytes, uint32_t count)
{
    if(count > OS1_IPC_PAYLOAD_MAX)
        return false;
    for(; count < OS1_IPC_PAYLOAD_MAX; ++count)
        if(bytes[count])
            return false;
    return true;
}
void unsubscribe_locked(Subscription* sub)
{
    sub->active = false;
    sub->count = 0;
    sub->dropped = 0;
    for(auto& slot : sub->connection->subscriptions)
        if(slot == sub)
            slot = nullptr;
    signal_locked();
}
void destroy_subscription(NativeObject* object)
{
    auto* sub = static_cast<Subscription*>(object);
    auto* c = sub->connection;
    {
        IrqSpinGuard guard(waiters.lock);
        unsubscribe_locked(sub);
    }
    kfree(sub);
    release(c);
    ipc_drain_wakeups();
}
long subscribe(PageFrameContainer& frames, Thread* thread, Endpoint* e, uint64_t pointer)
{
    os1_ipc_subscription request{};
    if(!copy_from_user(frames, thread, pointer, &request, sizeof(request)))
        return OS1_ERR_FAULT;
    if(request.version != 1 || request.size != sizeof(request) || !request.member ||
       request.reserved)
        return OS1_ERR_INVALID;
    auto* sub = static_cast<Subscription*>(kmalloc(sizeof(Subscription), KmallocFlags::Zero));
    if(!sub)
        return OS1_ERR_NO_MEMORY;
    sub->references = 1;
    sub->kind = OS1_OBJECT_SUBSCRIPTION;
    sub->interfaces = OS1_INTERFACE_SUBSCRIPTION;
    sub->allowed_rights = OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE;
    sub->destroy = destroy_subscription;
    sub->connection = e->connection;
    sub->member = request.member;
    retain(sub->connection);
    InstanceRef pin(open_instance(sub));
    object_release(sub);
    if(!pin.get())
        return OS1_ERR_NO_MEMORY;
    long fd = descriptor_install(
        thread->process->descriptors, pin.get(), sub->allowed_rights, OS1_DESCRIPTOR_CLOEXEC);
    if(fd < 0)
        return fd;
    long result = OS1_ERR_LIMIT;
    {
        IrqSpinGuard guard(waiters.lock);
        auto* c = sub->connection;
        if(c->provider_closed)
            result = OS1_ERR_PEER_CLOSED;
        else if(!c->ready)
            result = OS1_ERR_WOULD_BLOCK;
        else
            for(auto& slot : c->subscriptions)
                if(!slot)
                {
                    slot = sub;
                    sub->active = true;
                    result = fd;
                    break;
                }
    }
    if(result < 0)
        descriptor_close(thread->process->descriptors, fd);
    return result;
}
long emit(PageFrameContainer& frames, Thread* thread, Endpoint* e, uint64_t pointer)
{
    os1_ipc_event event{};
    if(!copy_from_user(frames, thread, pointer, &event, sizeof(event)))
        return OS1_ERR_FAULT;
    if(event.version != 1 || event.size != sizeof(event) || !event.member || event.sequence ||
       event.dropped || !zero_tail(event.payload, event.payload_bytes))
        return OS1_ERR_INVALID;
    {
        IrqSpinGuard guard(waiters.lock);
        auto* c = e->connection;
        if(!c->ready)
            return OS1_ERR_WOULD_BLOCK;
        if(!c->next_event)
            return OS1_ERR_LIMIT;
        event.sequence = c->next_event++;
        for(auto* sub : c->subscriptions)
        {
            if(!sub || sub->member != event.member)
                continue;
            if(sub->count == OS1_IPC_EVENT_LIMIT)
            {
                if(sub->dropped != UINT64_MAX)
                    ++sub->dropped;
                continue;
            }
            sub->events[(sub->head + sub->count) % OS1_IPC_EVENT_LIMIT] = event;
            ++sub->count;
        }
        signal_locked();
    }
    ipc_drain_wakeups();
    return 0;
}
long read_event(PageFrameContainer& frames, Thread* thread, Subscription* sub, uint64_t pointer)
{
    os1_ipc_event event{};
    {
        IrqSpinGuard guard(waiters.lock);
        if(!sub->active)
            return OS1_ERR_CLOSED;
        if(sub->receiving)
            return OS1_ERR_WOULD_BLOCK;
        if(!sub->count)
            return sub->connection->provider_closed ? OS1_ERR_PEER_CLOSED : OS1_ERR_WOULD_BLOCK;
        sub->receiving = true;
        event = sub->events[sub->head];
        event.dropped = sub->dropped;
    }
    bool copied = copy_to_user(frames, thread, pointer, &event, sizeof(event));
    long result = copied ? 0 : OS1_ERR_FAULT;
    {
        IrqSpinGuard guard(waiters.lock);
        if(!sub->active)
            result = OS1_ERR_CLOSED;
        else if(copied)
        {
            sub->head = (sub->head + 1) % OS1_IPC_EVENT_LIMIT;
            --sub->count;
            sub->dropped -= event.dropped;
        }
        sub->receiving = false;
        signal_locked();
    }
    ipc_drain_wakeups();
    return result;
}
// BIOS PIT fallback has no AP timer. Interrupt spinning providers explicitly,
// and retry from the BSP tick to cover migration/publication and transient IPI failure.
void kick_stopping_providers()
{
    for(cpu* owner = g_cpu_boot; owner; owner = owner->next)
    {
        if(owner == cpu_cur())
            continue;
        bool stop = false;
        {
            IrqSpinGuard guard(g_thread_registry_lock);
            auto* running = __atomic_load_n(&owner->current_thread, __ATOMIC_ACQUIRE);
            stop = ipc_stop_requested(running);
        }
        if(stop)
            (void)ipi_send_reschedule(owner);
    }
}
long stop_child(Thread* parent, uint64_t pid)
{
    Thread* child = nullptr;
    {
        IrqSpinGuard process_guard(g_process_table_lock);
        IrqSpinGuard guard(g_thread_registry_lock);
        for(auto* t = first_thread(); t; t = next_thread(t))
            if(t->process && t->process->pid == pid)
            {
                if(t->process->parent != parent->process || !t->process->private_provider)
                    return OS1_ERR_ACCESS;
                if(t->state == ThreadState::Dying)
                    return 0;
                child = t;
                (void)atomic_fetch_add(&child->io_pins, 1u);
                __atomic_store_n(&child->stop_requested, true, __ATOMIC_RELEASE);
                break;
            }
    }
    if(!child)
        return OS1_ERR_INVALID;
    {
        IrqSpinGuard guard(waiters.lock);
        signal_locked();
    }
    ipc_drain_wakeups();
    kick_stopping_providers();
    (void)__atomic_fetch_sub(&child->io_pins, 1u, __ATOMIC_RELEASE);
    return 0;
}
long create_pair(PageFrameContainer& frames, Thread* thread, uint64_t pointer)
{
    auto* c = static_cast<Connection*>(kmalloc(sizeof(Connection), KmallocFlags::Zero));
    if(!c)
        return OS1_ERR_NO_MEMORY;
    auto* client = static_cast<Endpoint*>(kmalloc(sizeof(Endpoint), KmallocFlags::Zero));
    auto* provider = static_cast<Endpoint*>(kmalloc(sizeof(Endpoint), KmallocFlags::Zero));
    if(!client || !provider)
    {
        kfree(client);
        kfree(provider);
        kfree(c);
        return OS1_ERR_NO_MEMORY;
    }
    {
        IrqSpinGuard guard(waiters.lock);
        if(!next_generation)
        {
            guard.unlock();
            kfree(client);
            kfree(provider);
            kfree(c);
            return OS1_ERR_LIMIT;
        }
        c->generation = next_generation++;
    }
    c->references = 2;
    c->next_request = 1;
    c->next_event = 1;
    Endpoint* endpoints[] = {client, provider};
    for(auto* e : endpoints)
    {
        e->references = 1;
        e->connection = c;
        e->provider = e == provider;
        e->kind = e->provider ? OS1_OBJECT_IPC_PROVIDER : OS1_OBJECT_IPC_CLIENT;
        e->interfaces = OS1_INTERFACE_IPC;
        e->allowed_rights =
            OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE;
        e->destroy = destroy_endpoint;
    }
    InstanceRef a(open_instance(client)), b(open_instance(provider));
    const uint32_t rights = client->allowed_rights;
    object_release(client);
    object_release(provider);
    if(!a.get() || !b.get())
        return OS1_ERR_NO_MEMORY;
    auto& table = thread->process->descriptors;
    long left = descriptor_install(table, a.get(), rights, OS1_DESCRIPTOR_CLOEXEC);
    if(left < 0)
        return left;
    long right = descriptor_install(table, b.get(), rights, OS1_DESCRIPTOR_CLOEXEC);
    if(right < 0)
    {
        descriptor_close(table, left);
        return right;
    }
    os1_ipc_pair pair{static_cast<int32_t>(left), static_cast<int32_t>(right)};
    if(!copy_to_user(frames, thread, pointer, &pair, sizeof(pair)))
    {
        descriptor_close(table, left);
        descriptor_close(table, right);
        return OS1_ERR_FAULT;
    }
    return 0;
}
long submit(PageFrameContainer& frames, Thread* thread, Endpoint* endpoint, uint64_t pointer)
{
    os1_ipc_message request{};
    if(!copy_from_user(frames, thread, pointer, &request, sizeof(request)))
        return OS1_ERR_FAULT;
    if(request.version != OS1_IPC_VERSION || request.size != sizeof(request) ||
       request.generation || request.request_id || request.reserved || !request.interface_id ||
       !request.interface_version || !request.member ||
       !zero_tail(request.payload, request.payload_bytes))
        return OS1_ERR_INVALID;
    auto* op = static_cast<Operation*>(kmalloc(sizeof(Operation), KmallocFlags::Zero));
    if(!op)
        return OS1_ERR_NO_MEMORY;
    auto* c = endpoint->connection;
    op->references = 1;
    op->kind = OS1_OBJECT_OPERATION;
    op->interfaces = OS1_INTERFACE_OPERATION;
    op->allowed_rights = OS1_RIGHT_READ | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE;
    op->destroy = destroy_operation;
    op->connection = c;
    retain(c);
    op->request = request;
    InstanceRef instance(open_instance(op));
    object_release(op);
    if(!instance.get())
        return OS1_ERR_NO_MEMORY;
    auto& table = thread->process->descriptors;
    long fd = descriptor_install(table, instance.get(), op->allowed_rights, OS1_DESCRIPTOR_CLOEXEC);
    if(fd < 0)
        return fd;
    long error = 0;
    {
        IrqSpinGuard guard(waiters.lock);
        if(c->provider_closed)
            error = OS1_ERR_PEER_CLOSED;
        else if(!c->ready)
            error = OS1_ERR_WOULD_BLOCK;
        else if(request.interface_id != c->interface_id ||
                request.interface_version != c->interface_version)
            error = OS1_ERR_UNSUPPORTED;
        else if(c->live_operations == OS1_IPC_OPERATION_LIMIT || !c->next_request)
            error = OS1_ERR_LIMIT;
        else
        {
            for(auto& slot : c->pending)
                if(!slot)
                {
                    op->request.generation = c->generation;
                    op->request.request_id = c->next_request++;
                    ++c->live_operations;
                    op->counted = true;
                    object_retain(op);
                    slot = op;
                    signal_locked();
                    break;
                }
        }
    }
    if(error < 0)
    {
        descriptor_close(table, fd);
        return error;
    }
    ipc_drain_wakeups();
    return fd;
}
long receive(PageFrameContainer& frames, Thread* thread, Endpoint* endpoint, uint64_t pointer)
{
    auto* c = endpoint->connection;
    Operation* chosen = nullptr;
    {
        IrqSpinGuard guard(waiters.lock);
        for(auto* op : c->pending)
            if(op && !op->delivered && !op->receiving &&
               (!chosen || op->request.request_id < chosen->request.request_id))
                chosen = op;
        if(!chosen)
            return c->client_closed ? OS1_ERR_PEER_CLOSED : OS1_ERR_WOULD_BLOCK;
        chosen->receiving = true;
        object_retain(chosen);
    }
    const bool copied =
        copy_to_user(frames, thread, pointer, &chosen->request, sizeof(chosen->request));
    {
        IrqSpinGuard guard(waiters.lock);
        chosen->receiving = false;
        chosen->delivered = copied;
        signal_locked();
    }
    object_release(chosen);
    ipc_drain_wakeups();
    return copied ? 0 : OS1_ERR_FAULT;
}
long reply(PageFrameContainer& frames, Thread* thread, Endpoint* endpoint, uint64_t pointer)
{
    os1_ipc_result result{};
    if(!copy_from_user(frames, thread, pointer, &result, sizeof(result)))
        return OS1_ERR_FAULT;
    if(result.version != OS1_IPC_VERSION || result.size != sizeof(result) ||
       result.transport_status || result.reserved ||
       !zero_tail(result.payload, result.payload_bytes))
        return OS1_ERR_INVALID;
    auto* c = endpoint->connection;
    Operation* completed = nullptr;
    {
        IrqSpinGuard guard(waiters.lock);
        if(result.generation != c->generation)
            return OS1_ERR_STALE_REPLY;
        for(auto& slot : c->pending)
            if(slot && slot->delivered && slot->request.request_id == result.request_id)
            {
                completed = slot;
                completed->result = result;
                completed->complete = true;
                slot = nullptr;
                break;
            }
        if(!completed)
            return OS1_ERR_STALE_REPLY;
        signal_locked();
    }
    object_release(completed);
    ipc_drain_wakeups();
    return 0;
}
}  // namespace
long sys_ipc(
    PageFrameContainer& frames, Thread* thread, uint64_t operation, uint64_t fd, uint64_t argument)
{
    if(!thread || !thread->process)
        return OS1_ERR_INVALID;
    if(operation == OS1_IPC_CLOCK)
    {
        if(fd || argument)
            return OS1_ERR_INVALID;
        IrqSpinGuard guard(waiters.lock);
        return static_cast<long>(monotonic_ns);
    }
    if(operation == OS1_IPC_STOP_CHILD)
        return fd ? OS1_ERR_INVALID : stop_child(thread, argument);
    if(operation == OS1_IPC_CREATE)
        return fd ? OS1_ERR_INVALID : create_pair(frames, thread, argument);
    if(operation < OS1_IPC_READY || operation > OS1_IPC_UNSUBSCRIBE)
        return OS1_ERR_UNSUPPORTED;
    if(fd >= OS1_DESCRIPTOR_LIMIT)
        return OS1_ERR_BAD_DESCRIPTOR;
    uint32_t right =
        (operation == OS1_IPC_READY || operation == OS1_IPC_SUBMIT || operation == OS1_IPC_REPLY ||
         operation == OS1_IPC_EMIT || operation == OS1_IPC_UNSUBSCRIBE)
            ? OS1_RIGHT_WRITE
            : OS1_RIGHT_READ;
    OpenInstance* raw = nullptr;
    long status = descriptor_acquire(thread->process->descriptors, fd, right, raw);
    if(status < 0)
        return status;
    InstanceRef pin(raw);
    auto* object = raw->object;
    const auto kind = object->kind;
    if(kind < OS1_OBJECT_IPC_CLIENT || kind > OS1_OBJECT_SUBSCRIPTION)
        return OS1_ERR_UNSUPPORTED;
    if(operation == OS1_IPC_WAIT || operation == OS1_IPC_WAIT_UNTIL)
    {
        uint64_t deadline = UINT64_MAX;
        if(operation == OS1_IPC_WAIT_UNTIL)
        {
            os1_ipc_deadline record{};
            if(!copy_from_user(frames, thread, argument, &record, sizeof(record)))
                return OS1_ERR_FAULT;
            if(record.version != 1 || record.size != sizeof(record) ||
               record.nanoseconds > INT64_MAX)
                return OS1_ERR_INVALID;
            deadline = record.nanoseconds;
        }
        else if(argument)
            return OS1_ERR_INVALID;
        IrqSpinGuard process_guard(g_process_table_lock);
        IrqSpinGuard guard(waiters.lock);
        if(ipc_stop_requested(thread))
            return OS1_ERR_CANCELLED;
        if(ready_locked(object))
            return 0;
        if(deadline <= monotonic_ns)
            return OS1_ERR_TIMED_OUT;
        thread->wait = {};
        thread->wait.reason = ThreadWaitReason::Ipc;
        thread->wait.ipc.instance = pin.take();
        thread->wait.ipc.deadline_ns = deadline;
        thread->state = ThreadState::Blocked;
        thread->process->state = ProcessState::Ready;
        wait_queue_enqueue_locked(waiters, thread);
        return kIpcBlocked;
    }
    if(kind == OS1_OBJECT_SUBSCRIPTION)
    {
        auto* sub = static_cast<Subscription*>(object);
        if(operation == OS1_IPC_EVENT_READ)
            return read_event(frames, thread, sub, argument);
        if(operation != OS1_IPC_UNSUBSCRIBE)
            return OS1_ERR_UNSUPPORTED;
        if(argument)
            return OS1_ERR_INVALID;
        {
            IrqSpinGuard guard(waiters.lock);
            unsubscribe_locked(sub);
        }
        ipc_drain_wakeups();
        return 0;
    }
    if(operation == OS1_IPC_EVENT_READ || operation == OS1_IPC_UNSUBSCRIBE)
        return OS1_ERR_UNSUPPORTED;
    if(operation == OS1_IPC_RESULT)
    {
        if(kind != OS1_OBJECT_OPERATION)
            return OS1_ERR_UNSUPPORTED;
        os1_ipc_result result{};
        {
            IrqSpinGuard guard(waiters.lock);
            auto* op = static_cast<Operation*>(object);
            if(!op->complete)
                return OS1_ERR_WOULD_BLOCK;
            result = op->result;
        }
        return copy_to_user(frames, thread, argument, &result, sizeof(result)) ? 0 : OS1_ERR_FAULT;
    }
    if(kind == OS1_OBJECT_OPERATION)
        return OS1_ERR_UNSUPPORTED;
    auto* e = static_cast<Endpoint*>(object);
    auto* c = e->connection;
    if(operation == OS1_IPC_INFO)
    {
        os1_ipc_info info{};
        {
            IrqSpinGuard guard(waiters.lock);
            if(c->provider_closed)
                return OS1_ERR_PEER_CLOSED;
            info = {OS1_IPC_VERSION,
                    sizeof(info),
                    c->generation,
                    c->interface_id,
                    c->interface_version,
                    c->ready ? 1u : 0u};
        }
        return copy_to_user(frames, thread, argument, &info, sizeof(info)) ? 0 : OS1_ERR_FAULT;
    }
    if(operation == OS1_IPC_SUBMIT)
        return e->provider ? OS1_ERR_UNSUPPORTED : submit(frames, thread, e, argument);
    if(operation == OS1_IPC_SUBSCRIBE)
        return e->provider ? OS1_ERR_UNSUPPORTED : subscribe(frames, thread, e, argument);
    if(!e->provider)
        return OS1_ERR_UNSUPPORTED;
    if(operation == OS1_IPC_EMIT)
        return emit(frames, thread, e, argument);
    if(operation == OS1_IPC_RECEIVE)
        return receive(frames, thread, e, argument);
    if(operation == OS1_IPC_REPLY)
        return reply(frames, thread, e, argument);
    os1_ipc_info info{};
    if(!copy_from_user(frames, thread, argument, &info, sizeof(info)))
        return OS1_ERR_FAULT;
    if(info.version != OS1_IPC_VERSION || info.size != sizeof(info) || info.generation ||
       !info.interface_id || !info.interface_version || info.ready != 1)
        return OS1_ERR_INVALID;
    {
        IrqSpinGuard guard(waiters.lock);
        if(c->ready)
            return OS1_ERR_INVALID;
        if(c->client_closed)
            return OS1_ERR_PEER_CLOSED;
        c->ready = true;
        c->interface_id = info.interface_id;
        c->interface_version = info.interface_version;
        signal_locked();
    }
    ipc_drain_wakeups();
    return 0;
}
void cancel_ipc_wait(Thread* thread)
{
    if(!thread)
        return;
    OpenInstance* released = nullptr;
    {
        IrqSpinGuard guard(waiters.lock);
        if(thread->wait.reason != ThreadWaitReason::Ipc)
            return;
        WaitQueue* queues[] = {&waiters, &signalled};
        for(auto* queue : queues)
        {
            Thread** link = &queue->head;
            while(*link && *link != thread)
                link = &(*link)->wait_link;
            if(*link)
            {
                *link = thread->wait_link;
                thread->wait_link = nullptr;
                break;
            }
        }
        released = thread->wait.ipc.instance;
        thread->wait = {};
    }
    instance_release(released);
}
void ipc_drain_wakeups()
{
    for(;;)
    {
        Thread* t = nullptr;
        OpenInstance* released = nullptr;
        {
            IrqSpinGuard guard(waiters.lock);
            Thread** link = &signalled.head;
            while(*link)
            {
                bool current = false;
                for(cpu* owner = g_cpu_boot; owner; owner = owner->next)
                    if(__atomic_load_n(&owner->current_thread, __ATOMIC_ACQUIRE) == *link)
                        current = true;
                if(!current && (*link)->state != ThreadState::Dying)
                    break;
                link = &(*link)->wait_link;
            }
            if(!*link)
                return;
            t = *link;
            *link = t->wait_link;
            t->wait_link = nullptr;
            (void)atomic_fetch_add(&t->io_pins, 1u);
            released = t->wait.ipc.instance;
            t->frame.rax = static_cast<uint64_t>(t->wait.ipc.result);
            t->wait = {};
        }
        instance_release(released);
        wake_blocked_thread(t);
        (void)__atomic_fetch_sub(&t->io_pins, 1u, __ATOMIC_RELEASE);
    }
}

bool ipc_stop_requested(const Thread* thread)
{
    return thread && __atomic_load_n(&thread->stop_requested, __ATOMIC_ACQUIRE);
}
void ipc_timer_tick(uint64_t elapsed_ns)
{
    {
        IrqSpinGuard guard(waiters.lock);
        monotonic_ns =
            elapsed_ns > INT64_MAX - monotonic_ns ? INT64_MAX : monotonic_ns + elapsed_ns;
        signal_locked();
    }
    ipc_drain_wakeups();
    kick_stopping_providers();
}

void ipc_stop_children(Process* parent)
{
    if(!parent)
        return;
    {
        IrqSpinGuard process_guard(g_process_table_lock);
        IrqSpinGuard guard(g_thread_registry_lock);
        for(auto* t = first_thread(); t; t = next_thread(t))
            if(t->process && t->process->parent == parent && t->process->private_provider &&
               t->state != ThreadState::Dying)
                __atomic_store_n(&t->stop_requested, true, __ATOMIC_RELEASE);
    }
    {
        IrqSpinGuard guard(waiters.lock);
        signal_locked();
    }
    ipc_drain_wakeups();
    kick_stopping_providers();
}
