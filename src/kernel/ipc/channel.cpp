#include "ipc/channel.hpp"

#include <os1/console.h>
#include <os1/system.h>
#include <os1/tmpfs.h>

#include "arch/x86_64/apic/ipi.hpp"
#include "arch/x86_64/cpu/cpu.hpp"
#include "buffer/buffer.hpp"
#include "console/console_input.hpp"
#include "debug/log.hpp"
#include "debug/log_export.hpp"
#include "ipc/kernel_provider.hpp"
#include "mm/kmem.hpp"
#include "mm/user_copy.hpp"
#include "platform/platform.hpp"
#include "util/memory.h"
#ifdef OS1_HOST_TEST
void (*host_ipc_before_receive)() = nullptr;
int host_ipc_kernel_allocation_budget = -1;
#endif
namespace
{
struct Operation;
struct Subscription;
// Accounts own only parents, never connections; attachment edges cannot cycle.
// Child reservations partition the root capacity and survive every alias.
struct Account : NativeObject
{
    Account* parent;
    uint32_t limit, live, reserved;
    bool charged;
};
struct Connection
{
    volatile uint32_t references;
    uint32_t live_operations, live_subscriptions;
    Account* account;
    const KernelProvider* kernel;
    OpenInstance* source;
    uint32_t service_rights, service_ceiling;
    bool account_created;
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
struct Capabilities
{
    uint32_t count;
    DescriptorEntry entries[OS1_IPC_CAP_LIMIT];
};
void release_caps(Capabilities& caps)
{
    for(unsigned i = 0; i < caps.count; ++i)
        instance_release(caps.entries[i].instance);
    caps = {};
}
struct Operation : NativeObject
{
    Connection* connection;
    Operation* kernel_next;
    uint64_t kernel_retry_ns;
    bool counted, delivered, receiving, complete, claiming;
    bool submitting, retiring, buffer_mode;
    BufferLoanBatch* loans;
    BufferOutputBatch* outputs;
    Capabilities request_caps, result_caps;
    os1_ipc_message request;
    os1_ipc_result result;
    os1_log_record log_origin;
};
struct Subscription : NativeObject
{
    Connection* connection;
    uint32_t member, head, count;
    uint64_t dropped;
    bool active, receiving, counted;
    os1_ipc_event events[OS1_IPC_EVENT_LIMIT];
};
// This lock protects all IPC state, registrations, and the deferred wake list.
// Table operations, user copies, allocation, destruction, and scheduling stay outside.
WaitQueue waiters{"ipc-state"};
WaitQueue signalled{"ipc-signalled"};
uint64_t next_generation = 1;
uint64_t monotonic_ns = 0;
uint32_t kernel_bindings = 0;
Operation* kernel_head = nullptr;
Operation* kernel_tail = nullptr;

void retain(Connection* c)
{
    (void)atomic_fetch_add(&c->references, 1u);
}
void release(Connection* c)
{
    if(__atomic_fetch_sub(&c->references, 1u, __ATOMIC_ACQ_REL) != 1u)
        return;
    auto* account = c->account;
    if(account)
    {
        IrqSpinGuard guard(waiters.lock);
        --account->live;
    }
    if(c->kernel)
    {
        IrqSpinGuard guard(waiters.lock);
        --kernel_bindings;
    }
    instance_release(c->source);
    kfree(c);
    object_release(account);
}
bool ready_locked(NativeObject* object)
{
    if(object->kind == OS1_OBJECT_CONSOLE_INPUT_SESSION)
        return console_input_ready(object);
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
        if(op && !op->delivered && !op->receiving && !op->submitting && !op->retiring)
            return true;
    return c->client_closed;
}
bool in_kernel_lane(const Operation* op, KernelDispatch lane)
{
    bool exporting = op->connection->interface_id == OS1_SYSTEM_INTERFACE_ID ||
                     op->connection->interface_id == OS1_FILE_INTERFACE_ID ||
                     op->connection->interface_id == OS1_DIRECTORY_INTERFACE_ID ||
                     (op->connection->interface_id == OS1_LOG_INTERFACE_ID &&
                      op->request.member == OS1_LOG_EXPORT);
    return lane == KernelDispatch::Any || exporting == (lane == KernelDispatch::Export);
}
bool kernel_ready_locked(KernelDispatch lane)
{
    for(auto* op = kernel_head; op; op = op->kernel_next)
        if(in_kernel_lane(op, lane) && op->kernel_retry_ns <= monotonic_ns)
            return true;
    return false;
}
long ready_selection_locked(const IpcWaitState& wait)
{
    // Only kernel dispatch workers register an empty selection; user WAIT_MANY
    // validates count >= 1. It shares IPC's post-context-save wakeup discipline.
    if(wait.timer_only)
        return OS1_ERR_WOULD_BLOCK;
    if(!wait.count)
        return kernel_ready_locked(static_cast<KernelDispatch>(wait.kernel_lane))
                   ? 0
                   : OS1_ERR_WOULD_BLOCK;
    for(unsigned i = 0; i < wait.count; ++i)
        if(ready_locked(wait.instances[i]->object))
            return i;
    return OS1_ERR_WOULD_BLOCK;
}
void release_wait(IpcWaitState& wait)
{
    for(unsigned i = 0; i < wait.count; ++i)
        instance_release(wait.instances[i]);
    wait = {};
}
void signal_locked()
{
    Thread** link = &waiters.head;
    while(*link)
    {
        Thread* t = *link;
        long selected;
        if(ipc_stop_requested(t))
            t->wait.ipc.result = OS1_ERR_CANCELLED;
        else if((selected = ready_selection_locked(t->wait.ipc)) >= 0)
            t->wait.ipc.result = selected;
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
    if(transport)
        diagnostic_log::ipc_end(op->log_origin, transport);
}
void destroy_operation(NativeObject* object)
{
    auto* op = static_cast<Operation*>(object);
    auto* c = op->connection;
    if(!op->complete)
        diagnostic_log::ipc_end(op->log_origin, OS1_ERR_CLOSED);
    {
        IrqSpinGuard guard(waiters.lock);
        if(op->counted)
            --c->live_operations;
    }
    release_caps(op->request_caps);
    release_caps(op->result_caps);
    buffer_release_outputs(op->outputs);
    buffer_release_loans(op->loans);
    kfree(op);
    release(c);
}
// The caller owns the detached pending reference. Retirement precedes visibility.
void fail_detached(Operation* op)
{
    buffer_finish_loans(op->loans);
    Capabilities discarded{};
    {
        IrqSpinGuard guard(waiters.lock);
        finish_locked(op, op->delivered ? OS1_ERR_OUTCOME_UNKNOWN : OS1_ERR_PEER_CLOSED);
        op->retiring = false;
        discarded = op->request_caps;
        op->request_caps = {};
        signal_locked();
    }
    release_caps(discarded);
    object_release(op);
}
void detach_locked(Operation* op)
{
    for(auto& slot : op->connection->pending)
        if(slot == op)
        {
            slot = nullptr;
            return;
        }
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
                // The active transition observes provider_closed before publication.
                if(!op || op->submitting || op->receiving || op->retiring)
                    continue;
                op->retiring = true;
                dropped[i] = op;
                c->pending[i] = nullptr;
            }
        }
        else
            c->client_closed = true;
        signal_locked();
    }
    for(auto* op : dropped)
        if(op)
            fail_detached(op);
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
        if(sub->counted)
            --c->live_subscriptions;
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
        else if(c->account && c->live_subscriptions >= OS1_IPC_SESSION_SUBSCRIPTION_LIMIT)
            result = OS1_ERR_LIMIT;
        else
            for(auto& slot : c->subscriptions)
                if(!slot)
                {
                    slot = sub;
                    sub->active = true;
                    sub->counted = true;
                    ++c->live_subscriptions;
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
long create_pair(PageFrameContainer& frames,
                 Thread* thread,
                 uint64_t pointer,
                 Account* account = nullptr)
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
        if(!next_generation || (account && account->live + account->reserved >= account->limit))
        {
            guard.unlock();
            kfree(client);
            kfree(provider);
            kfree(c);
            return OS1_ERR_LIMIT;
        }
        c->generation = next_generation++;
        c->account = account;
        if(account)
        {
            ++account->live;
            object_retain(account);
        }
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
bool kernel_allocation_allowed()
{
#ifdef OS1_HOST_TEST
    if(host_ipc_kernel_allocation_budget == 0)
        return false;
    if(host_ipc_kernel_allocation_budget > 0)
        --host_ipc_kernel_allocation_budget;
#endif
    return true;
}
long bind_kernel(PageFrameContainer& frames, Thread* thread, uint64_t fd, uint64_t pointer)
{
    if(fd >= OS1_DESCRIPTOR_LIMIT)
        return OS1_ERR_BAD_DESCRIPTOR;
    os1_ipc_kernel_binding request{};
    if(!copy_from_user(frames, thread, pointer, &request, sizeof(request)))
        return OS1_ERR_FAULT;
    if(request.version != 1 || request.size != sizeof(request) || !request.rights ||
       (request.rights & ~OS1_RIGHT_ALL))
        return OS1_ERR_INVALID;
    auto* provider = kernel_provider(request.interface_id, request.interface_version);
    if(!provider)
        return OS1_ERR_UNSUPPORTED;
    if(request.rights & ~provider->allowed_rights)
        return OS1_ERR_INVALID;
    DescriptorEntry grant{};
    long status = descriptor_acquire_entry(thread->process->descriptors, fd, 0, grant);
    if(status < 0)
        return status;
    InstanceRef source(grant.instance);
    if((request.rights & ~grant.rights))
        return OS1_ERR_ACCESS;
    if(source.get()->object->kind != provider->object_kind ||
       (provider->accepts && !provider->accepts(source.get()->object)))
        return OS1_ERR_UNSUPPORTED;
    auto* c = kernel_allocation_allowed()
                  ? static_cast<Connection*>(kmalloc(sizeof(Connection), KmallocFlags::Zero))
                  : nullptr;
    auto* client = kernel_allocation_allowed()
                       ? static_cast<Endpoint*>(kmalloc(sizeof(Endpoint), KmallocFlags::Zero))
                       : nullptr;
    if(!c || !client)
    {
        kfree(c);
        kfree(client);
        return OS1_ERR_NO_MEMORY;
    }
    {
        IrqSpinGuard guard(waiters.lock);
        if(!next_generation || kernel_bindings >= OS1_IPC_KERNEL_BINDING_LIMIT)
        {
            guard.unlock();
            kfree(c);
            kfree(client);
            return OS1_ERR_LIMIT;
        }
        ++kernel_bindings;
        c->generation = next_generation++;
    }
    c->references = 1;
    c->kernel = provider;
    c->source = source.take();
    c->service_rights = request.rights;
    c->service_ceiling =
        grant.descendant_rights &
        ((grant.rights & OS1_RIGHT_DELEGATE) ? OS1_RIGHT_ALL : ~OS1_RIGHT_DELEGATE);
    c->next_request = 1;
    c->ready = true;
    c->interface_id = provider->interface_id;
    c->interface_version = provider->interface_version;
    client->references = 1;
    client->connection = c;
    client->kind = OS1_OBJECT_IPC_CLIENT;
    client->interfaces = OS1_INTERFACE_IPC;
    client->allowed_rights =
        OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT | (grant.rights & OS1_RIGHT_DELEGATE);
    client->destroy = destroy_endpoint;
    InstanceRef instance(kernel_allocation_allowed() ? open_instance(client) : nullptr);
    object_release(client);
    if(!instance.get())
        return OS1_ERR_NO_MEMORY;
    return descriptor_install(thread->process->descriptors,
                              instance.get(),
                              instance.get()->object->allowed_rights,
                              OS1_DESCRIPTOR_CLOEXEC);
}
void destroy_account(NativeObject* object)
{
    auto* account = static_cast<Account*>(object);
    auto* parent = account->parent;
    if(parent && account->charged)
    {
        IrqSpinGuard guard(waiters.lock);
        parent->reserved -= account->limit;
    }
    kfree(account);
    object_release(parent);
}
long create_account(PageFrameContainer& frames,
                    Thread* thread,
                    NativeObject* source,
                    uint64_t pointer)
{
    os1_ipc_account request{};
    if(!copy_from_user(frames, thread, pointer, &request, sizeof(request)))
        return OS1_ERR_FAULT;
    if(request.version != 1 || request.size != sizeof(request) || request.reserved ||
       !request.sessions || request.sessions > OS1_IPC_SESSION_LIMIT)
        return OS1_ERR_INVALID;
    Account* parent = nullptr;
    Connection* control = nullptr;
    if(source->kind == OS1_OBJECT_IPC_ACCOUNT)
    {
        parent = static_cast<Account*>(source);
        if(parent->parent)
            return OS1_ERR_ACCESS;
    }
    else if(source->kind == OS1_OBJECT_IPC_PROVIDER)
    {
        control = static_cast<Endpoint*>(source)->connection;
        if(control->account)
            return OS1_ERR_ACCESS;
    }
    else
        return OS1_ERR_UNSUPPORTED;
    auto* account = static_cast<Account*>(kmalloc(sizeof(Account), KmallocFlags::Zero));
    if(!account)
        return OS1_ERR_NO_MEMORY;
    account->references = 1;
    account->kind = OS1_OBJECT_IPC_ACCOUNT;
    account->interfaces = OS1_INTERFACE_IPC_ACCOUNT;
    account->allowed_rights =
        OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE;
    account->destroy = destroy_account;
    account->parent = parent;
    if(parent)
        object_retain(parent);
    account->limit = request.sessions;
    InstanceRef pin(open_instance(account));
    object_release(account);
    if(!pin.get())
        return OS1_ERR_NO_MEMORY;
    long fd = descriptor_install(
        thread->process->descriptors, pin.get(), account->allowed_rights, OS1_DESCRIPTOR_CLOEXEC);
    if(fd < 0)
        return fd;
    long status = 0;
    {
        IrqSpinGuard guard(waiters.lock);
        if(control && control->client_closed)
            status = OS1_ERR_PEER_CLOSED;
        else if((control && control->account_created) ||
                (parent && request.sessions > parent->limit - parent->live - parent->reserved))
            status = OS1_ERR_LIMIT;
        else if(parent)
        {
            parent->reserved += request.sessions;
            account->charged = true;
        }
        else
            control->account_created = true;
    }
    if(status < 0)
    {
        descriptor_close(thread->process->descriptors, fd);
        return status;
    }
    return fd;
}
long wait_many(PageFrameContainer& frames, Thread* thread, uint64_t pointer)
{
    os1_ipc_wait_many request{};
    if(!copy_from_user(frames, thread, pointer, &request, sizeof(request)))
        return OS1_ERR_FAULT;
    if(request.version != 1 || request.size != sizeof(request) || request.reserved ||
       !request.count || request.count > OS1_IPC_WAIT_LIMIT ||
       (request.nanoseconds != UINT64_MAX && request.nanoseconds > INT64_MAX))
        return OS1_ERR_INVALID;
    struct Pins
    {
        IpcWaitState value{};
        ~Pins()
        {
            release_wait(value);
        }
    } pins;
    pins.value.deadline_ns = request.nanoseconds;
    for(unsigned i = 0; i < OS1_IPC_WAIT_LIMIT; ++i)
    {
        if(i >= request.count)
        {
            if(request.fds[i])
                return OS1_ERR_INVALID;
            continue;
        }
        OpenInstance* raw = nullptr;
        long status =
            descriptor_acquire(thread->process->descriptors, request.fds[i], OS1_RIGHT_READ, raw);
        if(status < 0)
            return status;
        pins.value.instances[pins.value.count++] = raw;
        if(raw->object->kind != OS1_OBJECT_CONSOLE_INPUT_SESSION &&
           (raw->object->kind < OS1_OBJECT_IPC_CLIENT ||
            raw->object->kind > OS1_OBJECT_SUBSCRIPTION))
            return OS1_ERR_UNSUPPORTED;
        for(unsigned j = 0; j < i; ++j)
            if(pins.value.instances[j]->object == raw->object)
                return OS1_ERR_INVALID;
    }
    IrqSpinGuard process_guard(g_process_table_lock);
    IrqSpinGuard guard(waiters.lock);
    if(ipc_stop_requested(thread))
        return OS1_ERR_CANCELLED;
    long selected = ready_selection_locked(pins.value);
    if(selected >= 0)
        return selected;
    if(request.nanoseconds <= monotonic_ns)
        return OS1_ERR_TIMED_OUT;
    thread->wait = {};
    thread->wait.reason = ThreadWaitReason::Ipc;
    thread->wait.ipc = pins.value;
    pins.value = {};
    thread->state = ThreadState::Blocked;
    thread->process->state = ProcessState::Ready;
    wait_queue_enqueue_locked(waiters, thread);
    return kIpcBlocked;
}
// The pending-request graph is acyclic: requests may only reference newer
// client connections. Completed replies are no longer owned by a connection.
long capture_caps(DescriptorTable& table,
                  const os1_ipc_capabilities& input,
                  Capabilities& out,
                  uint64_t request_generation)
{
    if(input.count > OS1_IPC_CAP_LIMIT || input.reserved)
        return OS1_ERR_INVALID;
    for(unsigned i = 0; i < OS1_IPC_CAP_LIMIT; ++i)
    {
        const auto& cap = input.entries[i];
        if(cap.reserved || ((cap.rights | cap.descendant_rights) & ~OS1_RIGHT_ALL) ||
           (i >= input.count && (cap.fd || cap.rights || cap.descendant_rights)))
            return OS1_ERR_INVALID;
    }
    for(unsigned i = 0; i < input.count; ++i)
    {
        const auto& cap = input.entries[i];
        DescriptorEntry source{};
        long status = descriptor_acquire_entry(table, cap.fd, OS1_RIGHT_DELEGATE, source);
        if(status < 0)
            return status;
        InstanceRef pin(source.instance);
        if((cap.rights & ~source.rights) || (cap.descendant_rights & ~source.descendant_rights))
            return OS1_ERR_ACCESS;
        auto* object = source.instance->object;
        if(object->kind == OS1_OBJECT_IPC_CLIENT)
        {
            if(request_generation &&
               static_cast<Endpoint*>(object)->connection->generation <= request_generation)
                return OS1_ERR_ACCESS;
        }
        else if(object->kind != OS1_OBJECT_CONSOLE && object->kind != OS1_OBJECT_FILE &&
                object->kind != OS1_OBJECT_DIRECTORY && object->kind != OS1_OBJECT_IPC_ACCOUNT)
            return OS1_ERR_UNSUPPORTED;
        out.entries[out.count++] = {
            pin.take(), cap.rights, OS1_DESCRIPTOR_CLOEXEC, cap.descendant_rights};
    }
    return 0;
}
struct CapGuard
{
    Capabilities value{};
    ~CapGuard()
    {
        release_caps(value);
    }
};
// Current public execution has one user thread per process. Roll back every
// installed entry on table exhaustion or output fault; never consume on failure.
long install_caps(DescriptorTable& table, const Capabilities& caps, os1_ipc_capabilities& output)
{
    output = {};
    for(unsigned i = 0; i < caps.count; ++i)
    {
        const auto& cap = caps.entries[i];
        long fd = descriptor_install(
            table, cap.instance, cap.rights, OS1_DESCRIPTOR_CLOEXEC, -1, cap.descendant_rights);
        if(fd < 0)
        {
            for(unsigned j = 0; j < output.count; ++j)
                descriptor_close(table, output.entries[j].fd);
            output = {};
            return fd;
        }
        output.entries[output.count++] = {
            static_cast<int32_t>(fd), cap.rights, cap.descendant_rights, 0};
    }
    return 0;
}
void rollback_caps(DescriptorTable& table, const os1_ipc_capabilities& output)
{
    for(unsigned i = 0; i < output.count; ++i)
        descriptor_close(table, output.entries[i].fd);
}
enum class Envelope
{
    Bytes,
    Caps,
    Buffers
};
long submit(
    PageFrameContainer& frames, Thread* thread, Endpoint* endpoint, uint64_t pointer, Envelope mode)
{
    if(endpoint->connection->kernel && mode != Envelope::Bytes)
        return OS1_ERR_UNSUPPORTED;
    os1_ipc_message_caps envelope{};
    os1_ipc_message_buffers buffers{};
    const bool buffer_mode = mode == Envelope::Buffers;
    if(buffer_mode)
    {
        if(!copy_from_user(frames, thread, pointer, &buffers, sizeof(buffers)))
            return OS1_ERR_FAULT;
        envelope.message = buffers.message;
    }
    else if(!copy_from_user(frames,
                            thread,
                            pointer,
                            &envelope,
                            mode == Envelope::Caps ? sizeof(envelope) : sizeof(envelope.message)))
        return OS1_ERR_FAULT;
    auto& request = envelope.message;
    if(request.version != OS1_IPC_VERSION || request.size != sizeof(request) ||
       request.generation || request.request_id || request.reserved || !request.interface_id ||
       !request.interface_version || !request.member ||
       !zero_tail(request.payload, request.payload_bytes))
        return OS1_ERR_INVALID;
    CapGuard attached;
    long captured = capture_caps(thread->process->descriptors,
                                 envelope.capabilities,
                                 attached.value,
                                 endpoint->connection->generation);
    if(captured < 0)
        return captured;
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
    op->buffer_mode = buffer_mode;
    retain(c);
    op->request = request;
    op->request_caps = attached.value;
    attached.value = {};
    InstanceRef instance(open_instance(op));
    object_release(op);
    if(!instance.get())
        return OS1_ERR_NO_MEMORY;
    auto& table = thread->process->descriptors;
    long fd = descriptor_install(table, instance.get(), op->allowed_rights, OS1_DESCRIPTOR_CLOEXEC);
    if(fd < 0)
        return fd;
    long error = 0;
    bool reserved = false;
    {
        IrqSpinGuard guard(waiters.lock);
        if(c->provider_closed)
            error = OS1_ERR_PEER_CLOSED;
        else if(!c->ready)
            error = OS1_ERR_WOULD_BLOCK;
        else if(request.interface_id != c->interface_id ||
                request.interface_version != c->interface_version)
            error = OS1_ERR_UNSUPPORTED;
        else if(c->live_operations >= (c->kernel    ? OS1_IPC_KERNEL_OPERATION_LIMIT
                                       : c->account ? OS1_IPC_SESSION_OPERATION_LIMIT
                                                    : OS1_IPC_OPERATION_LIMIT) ||
                !c->next_request)
            error = OS1_ERR_LIMIT;
        else
        {
            error = OS1_ERR_LIMIT;
            for(auto& slot : c->pending)
                if(!slot)
                {
                    op->request.generation = c->generation;
                    op->request.request_id = c->next_request++;
                    ++c->live_operations;
                    op->counted = true;
                    op->submitting = true;
                    object_retain(op);
                    slot = op;
                    reserved = true;
                    error = 0;
                    break;
                }
        }
    }
    // Admission owns a hidden queue slot. No state lock covers retirement.
    if(error == 0 && buffer_mode)
        error = buffer_prepare_loans(frames, thread, buffers.buffers, op->loans);
    if(reserved)
    {
        IrqSpinGuard guard(waiters.lock);
        if(error == 0 && c->provider_closed)
            error = OS1_ERR_PEER_CLOSED;
        if(error < 0)
            detach_locked(op);
        else
        {
            diagnostic_log::ipc_begin(
                op->log_origin, thread->process, request.interface_id, request.member);
            op->submitting = false;
            if(c->kernel)
            {
                if(kernel_tail)
                    kernel_tail->kernel_next = op;
                else
                    kernel_head = op;
                kernel_tail = op;
            }
            signal_locked();
        }
    }
    if(error < 0)
    {
        buffer_cancel_loans(op->loans);
        op->loans = nullptr;
        if(reserved)
            object_release(op);
        descriptor_close(table, fd);
        return error;
    }
    ipc_drain_wakeups();
    return fd;
}
long receive(
    PageFrameContainer& frames, Thread* thread, Endpoint* endpoint, uint64_t pointer, Envelope mode)
{
    auto* c = endpoint->connection;
    Operation* chosen = nullptr;
    {
        IrqSpinGuard guard(waiters.lock);
        for(auto* op : c->pending)
            if(op && !op->delivered && !op->receiving && !op->submitting && !op->retiring &&
               (!chosen || op->request.request_id < chosen->request.request_id))
                chosen = op;
        if(!chosen)
            return c->client_closed ? OS1_ERR_PEER_CLOSED : OS1_ERR_WOULD_BLOCK;
        if(chosen->buffer_mode != (mode == Envelope::Buffers) ||
           (mode == Envelope::Bytes && chosen->request_caps.count))
            return OS1_ERR_UNSUPPORTED;
        chosen->receiving = true;
        object_retain(chosen);
    }
#ifdef OS1_HOST_TEST
    if(host_ipc_before_receive)
        host_ipc_before_receive();
#endif
    os1_ipc_message_caps envelope{};
    os1_ipc_message_buffers buffers{};
    envelope.message = buffers.message = chosen->request;
    auto& table = thread->process->descriptors;
    long status;
    if(chosen->buffer_mode)
    {
        status = buffer_receive_loans(chosen->loans, thread, buffers.buffers);
        if(status == 0 && !copy_to_user(frames, thread, pointer, &buffers, sizeof(buffers)))
        {
            buffer_rollback_receive(chosen->loans, thread, buffers.buffers);
            status = OS1_ERR_FAULT;
        }
    }
    else
    {
        status = install_caps(table, chosen->request_caps, envelope.capabilities);
        if(status == 0 &&
           !copy_to_user(frames,
                         thread,
                         pointer,
                         &envelope,
                         mode == Envelope::Caps ? sizeof(envelope) : sizeof(envelope.message)))
        {
            rollback_caps(table, envelope.capabilities);
            status = OS1_ERR_FAULT;
        }
    }
    CapGuard consumed;
    bool failed = false;
    {
        IrqSpinGuard guard(waiters.lock);
        chosen->receiving = false;
        chosen->delivered = status == 0;
        if(status == 0)
        {
            consumed.value = chosen->request_caps;
            chosen->request_caps = {};
        }
        if(c->provider_closed)
        {
            chosen->retiring = true;
            detach_locked(chosen);
            failed = true;
        }
        signal_locked();
    }
    if(failed)
        fail_detached(chosen);
    object_release(chosen);
    ipc_drain_wakeups();
    return status;
}
long reply(
    PageFrameContainer& frames, Thread* thread, Endpoint* endpoint, uint64_t pointer, Envelope mode)
{
    os1_ipc_result_caps envelope{};
    os1_ipc_result_buffers buffers{};
    if(mode == Envelope::Buffers)
    {
        if(!copy_from_user(frames, thread, pointer, &buffers, sizeof(buffers)))
            return OS1_ERR_FAULT;
        envelope.result = buffers.result;
    }
    else if(!copy_from_user(frames,
                            thread,
                            pointer,
                            &envelope,
                            mode == Envelope::Caps ? sizeof(envelope) : sizeof(envelope.result)))
        return OS1_ERR_FAULT;
    auto& result = envelope.result;
    if(result.version != OS1_IPC_VERSION || result.size != sizeof(result) ||
       result.transport_status || result.reserved ||
       !zero_tail(result.payload, result.payload_bytes))
        return OS1_ERR_INVALID;
    CapGuard attached;
    long status =
        capture_caps(thread->process->descriptors, envelope.capabilities, attached.value, 0);
    if(status < 0)
        return status;
    auto* c = endpoint->connection;
    Operation* completed = nullptr;
    {
        IrqSpinGuard guard(waiters.lock);
        if(result.generation != c->generation)
            return OS1_ERR_STALE_REPLY;
        for(auto* op : c->pending)
            if(op && op->delivered && !op->retiring && op->request.request_id == result.request_id)
            {
                if(op->buffer_mode != (mode == Envelope::Buffers))
                    return OS1_ERR_UNSUPPORTED;
                completed = op;
                op->retiring = true;
                break;
            }
        if(!completed)
            return OS1_ERR_STALE_REPLY;
    }
    BufferOutputBatch* outputs = nullptr;
    if(completed->buffer_mode)
        status = buffer_prepare_outputs(frames, thread, completed->loans, buffers.buffers, outputs);
    if(status < 0)
    {
        bool failed;
        {
            IrqSpinGuard guard(waiters.lock);
            failed = c->provider_closed;
            if(failed)
                detach_locked(completed);
            else
                completed->retiring = false;
        }
        if(failed)
            fail_detached(completed);
        ipc_drain_wakeups();
        return status;
    }
    buffer_finish_loans(completed->loans);
    {
        IrqSpinGuard guard(waiters.lock);
        completed->result = result;
        completed->result_caps = attached.value;
        attached.value = {};
        completed->outputs = outputs;
        completed->retiring = false;
        completed->complete = true;
        diagnostic_log::ipc_end(completed->log_origin, result.application_status);
        detach_locked(completed);
        signal_locked();
    }
    object_release(completed);
    ipc_drain_wakeups();
    return 0;
}
long claim_result(
    PageFrameContainer& frames, Thread* thread, Operation* op, uint64_t pointer, bool buffer_mode)
{
    os1_ipc_result_caps envelope{};
    os1_ipc_result_buffers buffers{};
    {
        IrqSpinGuard guard(waiters.lock);
        if(op->buffer_mode != buffer_mode)
            return OS1_ERR_UNSUPPORTED;
        if(!op->complete || op->claiming)
            return OS1_ERR_WOULD_BLOCK;
        op->claiming = true;
        envelope.result = buffers.result = op->result;
    }
    auto& table = thread->process->descriptors;
    long status;
    if(buffer_mode)
    {
        status = op->outputs ? buffer_claim_outputs(op->outputs, thread, buffers.buffers) : 0;
        if(status == 0)
        {
            if(copy_to_user(frames, thread, pointer, &buffers, sizeof(buffers)))
            {
                if(op->outputs)
                    buffer_commit_claim(op->outputs);
            }
            else
            {
                if(op->outputs)
                    buffer_rollback_claim(op->outputs, thread, buffers.buffers);
                status = OS1_ERR_FAULT;
            }
        }
    }
    else
    {
        status = install_caps(table, op->result_caps, envelope.capabilities);
        if(status == 0 && !copy_to_user(frames, thread, pointer, &envelope, sizeof(envelope)))
        {
            rollback_caps(table, envelope.capabilities);
            status = OS1_ERR_FAULT;
        }
    }
    CapGuard consumed;
    {
        IrqSpinGuard guard(waiters.lock);
        if(status == 0 && !buffer_mode)
        {
            consumed.value = op->result_caps;
            op->result_caps = {};
        }
        op->claiming = false;
    }
    return status;
}
}  // namespace
long sys_ipc(
    PageFrameContainer& frames, Thread* thread, uint64_t operation, uint64_t fd, uint64_t argument)
{
    if(!thread || !thread->process)
        return OS1_ERR_INVALID;
    if(operation == OS1_IPC_BIND_KERNEL)
        return bind_kernel(frames, thread, fd, argument);
    if(operation == OS1_IPC_MEASURE)
    {
        if(fd)
            return OS1_ERR_INVALID;
        os1_measurement sample{};
        sample.version = 1;
        sample.size = sizeof(sample);
        if(platform_measurement_sample(sample.nanoseconds, sample.resolution_ns))
            sample.source = OS1_MEASUREMENT_HPET;
        return copy_to_user(frames, thread, argument, &sample, sizeof(sample)) ? 0 : OS1_ERR_FAULT;
    }
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
    if(operation == OS1_IPC_WAIT_MANY)
        return fd ? OS1_ERR_INVALID : wait_many(frames, thread, argument);
    if(operation < OS1_IPC_READY || operation > OS1_IPC_RESULT_BUFFERS)
        return OS1_ERR_UNSUPPORTED;
    if(fd >= OS1_DESCRIPTOR_LIMIT)
        return OS1_ERR_BAD_DESCRIPTOR;
    uint32_t right =
        (operation == OS1_IPC_READY || operation == OS1_IPC_SUBMIT || operation == OS1_IPC_REPLY ||
         operation == OS1_IPC_EMIT || operation == OS1_IPC_UNSUBSCRIBE ||
         operation == OS1_IPC_SUBMIT_CAPS || operation == OS1_IPC_REPLY_CAPS ||
         operation == OS1_IPC_SUBMIT_BUFFERS || operation == OS1_IPC_REPLY_BUFFERS ||
         operation == OS1_IPC_ACCOUNT_CREATE || operation == OS1_IPC_SESSION_CREATE)
            ? OS1_RIGHT_WRITE
            : OS1_RIGHT_READ;
    OpenInstance* raw = nullptr;
    long status = descriptor_acquire(thread->process->descriptors, fd, right, raw);
    if(status < 0)
        return status;
    InstanceRef pin(raw);
    auto* object = raw->object;
    const auto kind = object->kind;
    if(operation == OS1_IPC_ACCOUNT_CREATE)
        return create_account(frames, thread, object, argument);
    if(operation == OS1_IPC_SESSION_CREATE)
        return kind == OS1_OBJECT_IPC_ACCOUNT
                   ? create_pair(frames, thread, argument, static_cast<Account*>(object))
                   : OS1_ERR_UNSUPPORTED;
    if(kind != OS1_OBJECT_CONSOLE_INPUT_SESSION &&
       (kind < OS1_OBJECT_IPC_CLIENT || kind > OS1_OBJECT_SUBSCRIPTION))
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
        thread->wait.ipc = {};
        thread->wait.ipc.instances[0] = pin.take();
        thread->wait.ipc.count = 1;
        thread->wait.ipc.deadline_ns = deadline;
        thread->state = ThreadState::Blocked;
        thread->process->state = ProcessState::Ready;
        wait_queue_enqueue_locked(waiters, thread);
        return kIpcBlocked;
    }
    if(kind == OS1_OBJECT_CONSOLE_INPUT_SESSION)
        return OS1_ERR_UNSUPPORTED;
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
    if(operation == OS1_IPC_RESULT_CAPS || operation == OS1_IPC_RESULT_BUFFERS)
        return kind == OS1_OBJECT_OPERATION ? claim_result(frames,
                                                           thread,
                                                           static_cast<Operation*>(object),
                                                           argument,
                                                           operation == OS1_IPC_RESULT_BUFFERS)
                                            : OS1_ERR_UNSUPPORTED;
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
    if(operation == OS1_IPC_SUBMIT || operation == OS1_IPC_SUBMIT_CAPS ||
       operation == OS1_IPC_SUBMIT_BUFFERS)
        return e->provider ? OS1_ERR_UNSUPPORTED
                           : submit(frames,
                                    thread,
                                    e,
                                    argument,
                                    operation == OS1_IPC_SUBMIT_BUFFERS ? Envelope::Buffers
                                    : operation == OS1_IPC_SUBMIT_CAPS  ? Envelope::Caps
                                                                        : Envelope::Bytes);
    if(operation == OS1_IPC_SUBSCRIBE)
        return e->provider || c->kernel ? OS1_ERR_UNSUPPORTED
                                        : subscribe(frames, thread, e, argument);
    if(!e->provider)
        return OS1_ERR_UNSUPPORTED;
    if(operation == OS1_IPC_EMIT)
        return emit(frames, thread, e, argument);
    if(operation == OS1_IPC_RECEIVE || operation == OS1_IPC_RECEIVE_CAPS ||
       operation == OS1_IPC_RECEIVE_BUFFERS)
        return receive(frames,
                       thread,
                       e,
                       argument,
                       operation == OS1_IPC_RECEIVE_BUFFERS ? Envelope::Buffers
                       : operation == OS1_IPC_RECEIVE_CAPS  ? Envelope::Caps
                                                            : Envelope::Bytes);
    if(operation == OS1_IPC_REPLY || operation == OS1_IPC_REPLY_CAPS ||
       operation == OS1_IPC_REPLY_BUFFERS)
        return reply(frames,
                     thread,
                     e,
                     argument,
                     operation == OS1_IPC_REPLY_BUFFERS ? Envelope::Buffers
                     : operation == OS1_IPC_REPLY_CAPS  ? Envelope::Caps
                                                        : Envelope::Bytes);
    if(operation != OS1_IPC_READY)
        return OS1_ERR_UNSUPPORTED;
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
    IpcWaitState released{};
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
        released = thread->wait.ipc;
        thread->wait = {};
    }
    release_wait(released);
}
void ipc_drain_wakeups()
{
    for(;;)
    {
        Thread* t = nullptr;
        IpcWaitState released{};
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
            released = t->wait.ipc;
            // Only a user syscall consumes this return register. A kernel worker
            // can be preempted with any live RAX value while parking.
            if(t->user_mode)
                t->frame.rax = static_cast<uint64_t>(t->wait.ipc.result);
            t->wait = {};
        }
        release_wait(released);
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
    diagnostic_log::tick(elapsed_ns);
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

bool ipc_matching_endpoints(NativeObject* provider, NativeObject* client)
{
    return provider && client && provider->kind == OS1_OBJECT_IPC_PROVIDER &&
           client->kind == OS1_OBJECT_IPC_CLIENT &&
           static_cast<Endpoint*>(provider)->connection ==
               static_cast<Endpoint*>(client)->connection;
}

bool ipc_run_kernel_request(KernelDispatch lane)
{
    Operation* op;
    {
        IrqSpinGuard guard(waiters.lock);
        Operation* previous = nullptr;
        op = kernel_head;
        while(op && (!in_kernel_lane(op, lane) || op->kernel_retry_ns > monotonic_ns))
        {
            previous = op;
            op = op->kernel_next;
        }
        if(!op)
            return false;
        if(previous)
            previous->kernel_next = op->kernel_next;
        else
            kernel_head = op->kernel_next;
        if(kernel_tail == op)
            kernel_tail = previous;
        op->kernel_next = nullptr;
    }
    // The pending reference owns this operation through dispatch. Its connection
    // retains the resource even if all client and result descriptors are closed.
    auto* c = op->connection;
    os1_ipc_result response{};
    DescriptorEntry output{};
    log_export::ForegroundGuard foreground(c->interface_id == OS1_FILE_INTERFACE_ID ||
                                           c->interface_id == OS1_DIRECTORY_INTERFACE_ID);
    if(!foreground.held || c->kernel->try_invoke)
    {
        if(!foreground.held ||
           !c->kernel->try_invoke(
               *c->source, c->service_rights, c->service_ceiling, op->request, response, output))
        {
            IrqSpinGuard guard(waiters.lock);
            // Keep the pending reference. Retry on a later tick, not in a busy loop.
            op->kernel_retry_ns = monotonic_ns + 1'000'000;
            if(kernel_tail)
                kernel_tail->kernel_next = op;
            else
                kernel_head = op;
            kernel_tail = op;
            return false;
        }
    }
    else
        c->kernel->invoke(
            *c->source, c->service_rights, c->service_ceiling, op->request, response, output);
    {
        IrqSpinGuard guard(waiters.lock);
        op->delivered = true;
        detach_locked(op);
        finish_locked(op, 0);
        op->result.application_status = response.application_status;
        if(response.payload_bytes <= OS1_IPC_PAYLOAD_MAX)
        {
            op->result.payload_bytes = response.payload_bytes;
            memcpy(op->result.payload, response.payload, response.payload_bytes);
        }
        else
            op->result.application_status = OS1_ERR_INVALID;
        diagnostic_log::ipc_end(op->log_origin, op->result.application_status);
        if(!op->result.application_status && output.instance)
        {
            op->result_caps.count = 1;
            op->result_caps.entries[0] = output;
            output = {};
        }
        signal_locked();
    }
    instance_release(output.instance);
    object_release(op);
    ipc_drain_wakeups();
    return true;
}
bool ipc_wait_kernel_work(Thread* thread, KernelDispatch lane)
{
    if(!thread || thread->user_mode)
        return false;
    IrqSpinGuard process_guard(g_process_table_lock);
    IrqSpinGuard guard(waiters.lock);
    if(kernel_ready_locked(lane))
        return false;
    thread->wait = {};
    thread->wait.reason = ThreadWaitReason::Ipc;
    thread->wait.ipc = {};
    thread->wait.ipc.kernel_lane = static_cast<uint8_t>(lane);
    thread->wait.ipc.deadline_ns = monotonic_ns + 250000000;
    for(auto* op = kernel_head; op; op = op->kernel_next)
        if(in_kernel_lane(op, lane) && op->kernel_retry_ns < thread->wait.ipc.deadline_ns)
            thread->wait.ipc.deadline_ns = op->kernel_retry_ns;
    thread->state = ThreadState::Blocked;
    thread->process->state = ProcessState::Ready;
    wait_queue_enqueue_locked(waiters, thread);
    return true;
}
void kernel_sleep_for(Thread* thread, uint64_t delay_ns)
{
    {
        IrqSpinGuard process_guard(g_process_table_lock);
        IrqSpinGuard guard(waiters.lock);
        thread->wait = {};
        thread->wait.reason = ThreadWaitReason::Ipc;
        thread->wait.ipc = {};
        thread->wait.ipc.timer_only = true;
        thread->wait.ipc.deadline_ns =
            delay_ns > uint64_t(INT64_MAX) - monotonic_ns ? INT64_MAX : monotonic_ns + delay_ns;
        thread->state = ThreadState::Blocked;
        thread->process->state = ProcessState::Ready;
        wait_queue_enqueue_locked(waiters, thread);
    }
#ifndef OS1_HOST_TEST
    while(thread->wait.reason == ThreadWaitReason::Ipc)
    {
        IrqGuard irq;
        if(ipi_send_reschedule(cpu_cur()))
            asm volatile("sti; hlt" ::: "memory");
        else
            asm volatile("pause" ::: "memory");
    }
#endif
}
namespace
{
#ifndef OS1_HOST_TEST
void kernel_worker(KernelDispatch lane)
{
    for(;;)
    {
        if(lane == KernelDispatch::Export)
            log_export::poll();
        if(ipc_run_kernel_request(lane) || !ipc_wait_kernel_work(current_thread(), lane))
            continue;
        while(current_thread()->wait.reason == ThreadWaitReason::Ipc)
        {
            // Save the worker's frame even on APs without a periodic timer.
            // Deferred IPC wakeups cannot enqueue it until it leaves this CPU.
            IrqGuard irq;
            if(ipi_send_reschedule(cpu_cur()))
                asm volatile("sti; hlt" ::: "memory");
            else
                asm volatile("pause" ::: "memory");
        }
    }
}
void kernel_provider_worker()
{
    kernel_worker(KernelDispatch::Service);
}
void kernel_export_worker()
{
    kernel_worker(KernelDispatch::Export);
}
#endif
}  // namespace
bool ipc_start_kernel_worker(Process* process, PageFrameContainer& frames)
{
#ifdef OS1_HOST_TEST
    (void)process;
    (void)frames;
    return true;
#else
    return create_kernel_thread(process, kernel_provider_worker, frames) != nullptr &&
           create_kernel_thread(process, kernel_export_worker, frames) != nullptr;
#endif
}
