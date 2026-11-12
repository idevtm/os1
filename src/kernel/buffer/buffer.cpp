#include "buffer/buffer.hpp"

#include "arch/x86_64/apic/ipi.hpp"
#include "arch/x86_64/cpu/cpu.hpp"
#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "mm/user_copy.hpp"
#include "mm/user_copy_lock.hpp"
#include "proc/process.hpp"
#include "resource/descriptor.hpp"
#include "util/memory.h"

#if defined(OS1_HOST_TEST)
void (*host_buffer_before_prepare)() = nullptr;
#endif
namespace
{
constexpr uint32_t kRights =
    OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE;
constexpr uint64_t kMapBase = kUserSpaceBase + 0x20000000ull;
constexpr uint32_t kPages = OS1_BUFFER_BYTES_MAX / kPageSize;
// Reservations consume the same bounded mapping records as active aliases.
constexpr uint32_t kMapSlots = OS1_BUFFER_OBJECT_MAP_LIMIT;
struct Budget
{
    Budget* next;
    Process* process;
    uint64_t space_generation;
    uint32_t references, objects, maps, outbound, inbound;
};
struct Mapping
{
    Budget* budget;
    uint64_t space_generation, cr3, address;
    bool writable, present, borrowed;
};
struct Buffer
{
    NativeObject object;
    Buffer* next;
    PageFrameContainer* frames;
    Budget *origin, *owner;
    uint64_t identity, bytes, generation;
    uint32_t page_count;
    bool borrowed;
    uint64_t pages[kPages];
    Mapping maps[kMapSlots];
};
Budget* budgets;
Buffer* objects;
uint64_t next_identity;
uint32_t global_pages;
Spinlock buffer_lock{"buffer-ownership"};
#if defined(OS1_HOST_TEST)
thread_local uint32_t buffer_lock_depth = 0;
#endif

uint32_t& lock_depth()
{
#if defined(OS1_HOST_TEST)
    return buffer_lock_depth;
#else
    return cpu_cur()->buffer_lock_depth;
#endif
}

class BufferGuard
{
public:
    BufferGuard()
    {
        if(lock_depth()++ == 0)
            buffer_lock.lock();
    }
    ~BufferGuard()
    {
        if(--lock_depth() == 0)
            buffer_lock.unlock();
    }

private:
    IrqGuard irq_;
};

void retain(Budget* b)
{
    if(b)
        ++b->references;
}
void release(Budget* b)
{
    if(!b || --b->references)
        return;
    Budget** link = &budgets;
    while(*link != b)
        link = &(*link)->next;
    *link = b->next;
    kfree(b);
}
Budget* budget(Process* process, bool create = true)
{
    for(auto* b = budgets; b; b = b->next)
        if(b->process == process)
            return b;
    if(!create)
        return nullptr;
    auto* b = static_cast<Budget*>(kmalloc(sizeof(Budget), KmallocFlags::Zero));
    if(!b)
        return nullptr;
    b->process = process;
    b->references = 1;
    b->space_generation = 1;
    b->next = budgets;
    budgets = b;
    return b;
}
void set_owner(Buffer* b, Budget* owner)
{
    retain(owner);
    release(b->owner);
    b->owner = owner;
    ++b->generation;
}
void destroy_buffer(NativeObject* object)
{
    BufferGuard guard;
    auto* b = reinterpret_cast<Buffer*>(object);
    Buffer** link = &objects;
    while(*link != b)
        link = &(*link)->next;
    *link = b->next;
    for(uint32_t i = 0; i < b->page_count; ++i)
        b->frames->free(b->pages[i]);
    global_pages -= b->page_count;
    --b->origin->objects;
    release(b->origin);
    release(b->owner);
    kfree(b);
}
void erase_map(Buffer* b, Mapping& m)
{
    if(!m.budget)
        return;
    auto* owner = m.budget;
    --owner->maps;
    m = {};
    release(owner);
    object_release(&b->object);
}
void revoke(Buffer* b, Budget* from, bool discard, bool borrowed_only = false)
{
    {
        UserCopyGuard copy_guard(from->process);
        for(auto& m : b->maps)
            if(m.budget == from && (!borrowed_only || m.borrowed) && m.present)
            {
                VirtualMemory vm(*b->frames, m.cr3);
                vm.detach(m.address, b->page_count);
                m.present = false;
            }
        ipi_retire_tlb();
    }
    if(discard)
        for(auto& m : b->maps)
            if(m.budget == from && (!borrowed_only || m.borrowed))
                erase_map(b, m);
}
bool map_alias(Buffer* b, VirtualMemory& vm, uint64_t address, bool writable, bool reserved)
{
    const auto flags = PageFlags::User | PageFlags::NoExecute | PageFlags::SharedBacking |
                       (writable ? PageFlags::Write : PageFlags::None);
    for(uint32_t i = 0; i < b->page_count; ++i)
    {
        const uint64_t virtual_page = address + i * kPageSize;
        const bool mapped = reserved ? vm.map_reserved_user_page(virtual_page, b->pages[i], flags)
                                     : vm.map_physical(virtual_page, b->pages[i], 1, flags);
        if(!mapped)
        {
            vm.detach(address, i);
            ipi_retire_tlb();
            return false;
        }
    }
    return true;
}
void restore(Buffer* b, Budget* to)
{
    set_owner(b, to);
    UserCopyGuard copy_guard(to->process);
    for(auto& m : b->maps)
        if(m.budget == to && !m.present)
        {
            if(!to->process || m.space_generation != to->space_generation)
            {
                erase_map(b, m);
                continue;
            }
            VirtualMemory vm(*b->frames, m.cr3);
            if(map_alias(b, vm, m.address, m.writable, true))
                m.present = true;
            else
                erase_map(b, m);
        }
}
bool authority(Buffer* b, Budget* owner, const OpenInstance* instance)
{
    return b->owner == owner && owner && owner->process &&
           (instance->cursor ? instance->cursor == b->generation : !b->borrowed);
}
long acquire(Thread* t, int fd, uint32_t rights, OpenInstance*& instance, Buffer*& b)
{
    auto result = descriptor_acquire(t->process->descriptors, fd, rights, instance);
    if(result < 0)
        return result;
    if(instance->object->kind != OS1_OBJECT_BUFFER)
    {
        instance_release(instance);
        instance = nullptr;
        return OS1_ERR_UNSUPPORTED;
    }
    b = reinterpret_cast<Buffer*>(instance->object);
    return 0;
}
bool valid_batch(const os1_ipc_buffers& in)
{
    if(in.count > OS1_BUFFER_ATTACHMENT_LIMIT || in.reserved)
        return false;
    for(uint32_t i = in.count; i < OS1_BUFFER_ATTACHMENT_LIMIT; ++i)
        if(in.entries[i].fd || in.entries[i].disposition)
            return false;
    return true;
}
long install(Buffer* b, Thread* t, bool borrowed)
{
    InstanceRef instance(open_instance(&b->object));
    if(!instance.get())
        return OS1_ERR_NO_MEMORY;
    instance.get()->cursor = borrowed ? b->generation : 0;
    return descriptor_install(t->process->descriptors,
                              instance.get(),
                              borrowed ? (kRights & ~OS1_RIGHT_DELEGATE) : kRights,
                              OS1_DESCRIPTOR_CLOEXEC);
}
}  // namespace
struct BufferLoanBatch
{
    Budget *caller, *borrower;
    uint32_t count;
    bool finished;
    Buffer* items[OS1_BUFFER_ATTACHMENT_LIMIT];
};
struct BufferOutputBatch
{
    Budget *caller, *source;
    uint32_t count;
    bool consumed, claimed;
    Buffer* items[OS1_BUFFER_ATTACHMENT_LIMIT];
    uint32_t disposition[OS1_BUFFER_ATTACHMENT_LIMIT];
};

long sys_buffer(PageFrameContainer& frames, Thread* t, uint64_t fd, uint64_t op, uint64_t arg)
{
    if(!t || !t->process)
        return OS1_ERR_INVALID;
    BufferGuard guard;
    auto* owner = budget(t->process);
    if(!owner)
        return OS1_ERR_NO_MEMORY;
    if(op == OS1_RESOURCE_BUFFER_CREATE)
    {
        os1_buffer_create r{};
        if(!copy_from_user(frames, t, arg, &r, sizeof(r)))
            return OS1_ERR_FAULT;
        if(fd || r.version != OS1_BUFFER_ABI_VERSION || r.size != sizeof(r) || !r.bytes ||
           r.bytes > OS1_BUFFER_BYTES_MAX || r.mode != OS1_BUFFER_EXCLUSIVE || r.reserved)
            return OS1_ERR_INVALID;
        const uint32_t pages = (r.bytes + kPageSize - 1) / kPageSize;
        if(owner->objects >= OS1_BUFFER_PROCESS_OBJECT_LIMIT ||
           pages > OS1_BUFFER_GLOBAL_PAGE_LIMIT - global_pages)
            return OS1_ERR_LIMIT;
        auto* b = static_cast<Buffer*>(kmalloc(sizeof(Buffer), KmallocFlags::Zero));
        if(!b)
            return OS1_ERR_NO_MEMORY;
        b->object = {1, OS1_OBJECT_BUFFER, OS1_INTERFACE_BUFFER, kRights, destroy_buffer};
        b->frames = &frames;
        b->bytes = r.bytes;
        b->identity = ++next_identity;
        b->generation = 1;
        b->origin = owner;
        retain(owner);
        b->owner = owner;
        retain(owner);
        ++owner->objects;
        b->next = objects;
        objects = b;
        for(uint32_t i = 0; i < pages; ++i)
        {
            if(!frames.allocate(b->pages[i]))
            {
                object_release(&b->object);
                return OS1_ERR_NO_MEMORY;
            }
            ++b->page_count;
            ++global_pages;
            memset(kernel_physical_pointer<void>(b->pages[i]), 0, kPageSize);
        }
        long result = install(b, t, false);
        object_release(&b->object);
        return result;
    }
    OpenInstance* instance = nullptr;
    Buffer* b = nullptr;
    long result = acquire(t,
                          static_cast<int>(fd),
                          op == OS1_RESOURCE_BUFFER_INFO ? OS1_RIGHT_INSPECT : OS1_RIGHT_READ,
                          instance,
                          b);
    if(result < 0)
        return result;
    InstanceRef ref(instance);
    if(op == OS1_RESOURCE_BUFFER_INFO)
    {
        os1_buffer_info in{};
        if(!copy_from_user(frames, t, arg, &in, sizeof(in)))
            return OS1_ERR_FAULT;
        if(in.version != OS1_BUFFER_ABI_VERSION || in.size != sizeof(in))
            return OS1_ERR_INVALID;
        os1_buffer_info info{OS1_BUFFER_ABI_VERSION,
                             sizeof(info),
                             b->identity,
                             b->bytes,
                             b->generation,
                             OS1_BUFFER_EXCLUSIVE,
                             authority(b, owner, instance) ? 1u : 0u};
        return copy_to_user(frames, t, arg, &info, sizeof(info)) ? 0 : OS1_ERR_FAULT;
    }
    os1_buffer_map r{};
    if(!copy_from_user(frames, t, arg, &r, sizeof(r)))
        return OS1_ERR_FAULT;
    if(r.version != OS1_BUFFER_ABI_VERSION || r.size != sizeof(r) || r.reserved || r.writable > 1)
        return OS1_ERR_INVALID;
    if(op == OS1_RESOURCE_BUFFER_UNMAP)
    {
        if(instance->cursor && instance->cursor != b->generation)
            return OS1_ERR_ACCESS;
        if(r.writable || !r.address)
            return OS1_ERR_INVALID;
        for(auto& m : b->maps)
            if(m.budget == owner && m.address == r.address &&
               m.space_generation == owner->space_generation)
            {
                if(m.present)
                {
                    UserCopyGuard copy_guard(owner->process);
                    VirtualMemory vm(frames, m.cr3);
                    vm.detach(m.address, b->page_count);
                    ipi_retire_tlb();
                }
                erase_map(b, m);
                return 0;
            }
        return OS1_ERR_INVALID;
    }
    if(op != OS1_RESOURCE_BUFFER_MAP || r.address)
        return OS1_ERR_INVALID;
    if(!authority(b, owner, instance))
        return OS1_ERR_ACCESS;
    if(r.writable)
    {
        OpenInstance* checked = nullptr;
        result = descriptor_acquire(
            t->process->descriptors, static_cast<int>(fd), OS1_RIGHT_WRITE, checked);
        if(result < 0)
            return result;
        const bool same = checked == instance;
        instance_release(checked);
        if(!same)
            return OS1_ERR_BAD_DESCRIPTOR;
    }
    uint32_t aliases = 0;
    Mapping* slot = nullptr;
    for(auto& m : b->maps)
    {
        if(m.budget == owner)
            ++aliases;
        if(!m.budget)
            slot = &m;
    }
    if(!slot || aliases >= OS1_BUFFER_OBJECT_MAP_LIMIT ||
       owner->maps >= OS1_BUFFER_PROCESS_MAP_LIMIT)
        return OS1_ERR_LIMIT;
    UserCopyGuard copy_guard(t->process);
    VirtualMemory vm(frames, t->address_space_cr3);
    uint64_t address = kMapBase;
    for(; address < kMapBase + OS1_BUFFER_PROCESS_MAP_LIMIT * OS1_BUFFER_BYTES_MAX;
        address += OS1_BUFFER_BYTES_MAX)
    {
        bool used = false;
        for(auto* other = objects; other; other = other->next)
            for(auto& m : other->maps)
                if(m.budget == owner && m.address == address)
                    used = true;
        for(uint32_t i = 0; i < kPages && !used; ++i)
        {
            uint64_t physical = 0, leaf_flags = 0;
            used = vm.translate(address + i * kPageSize, physical, leaf_flags);
        }
        if(!used)
            break;
    }
    if(address >= kMapBase + OS1_BUFFER_PROCESS_MAP_LIMIT * OS1_BUFFER_BYTES_MAX)
        return OS1_ERR_LIMIT;
    if(!map_alias(b, vm, address, r.writable != 0, false))
        return OS1_ERR_NO_MEMORY;
    *slot = {owner,
             owner->space_generation,
             t->address_space_cr3,
             address,
             r.writable != 0,
             true,
             b->borrowed};
    retain(owner);
    ++owner->maps;
    object_retain(&b->object);
    r.address = address;
    if(!copy_to_user(frames, t, arg, &r, sizeof(r)))
    {
        vm.detach(address, b->page_count);
        ipi_retire_tlb();
        erase_map(b, *slot);
        return OS1_ERR_FAULT;
    }
    return 0;
}
long buffer_prepare_loans(PageFrameContainer&,
                          Thread* t,
                          const os1_ipc_buffers& in,
                          BufferLoanBatch*& out)
{
    out = nullptr;
#if defined(OS1_HOST_TEST)
    if(host_buffer_before_prepare)
        host_buffer_before_prepare();
#endif
    if(!t || !t->process || !valid_batch(in))
        return OS1_ERR_INVALID;
    BufferGuard guard;
    auto* caller = budget(t->process);
    if(!caller)
        return OS1_ERR_NO_MEMORY;
    if(in.count > OS1_BUFFER_PROCESS_LOAN_LIMIT - caller->outbound)
        return OS1_ERR_LIMIT;
    auto* batch =
        static_cast<BufferLoanBatch*>(kmalloc(sizeof(BufferLoanBatch), KmallocFlags::Zero));
    if(!batch)
        return OS1_ERR_NO_MEMORY;
    long status = 0;
    for(uint32_t i = 0; i < in.count; ++i)
    {
        OpenInstance* instance = nullptr;
        Buffer* b = nullptr;
        if(in.entries[i].disposition != OS1_BUFFER_LOAN)
        {
            status = OS1_ERR_INVALID;
            break;
        }
        status = acquire(t,
                         in.entries[i].fd,
                         OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_DELEGATE,
                         instance,
                         b);
        if(status < 0)
            break;
        if(!authority(b, caller, instance) || b->borrowed)
            status = OS1_ERR_ACCESS;
        for(uint32_t j = 0; j < i; ++j)
            if(batch->items[j] == b)
                status = OS1_ERR_INVALID;
        if(status >= 0)
        {
            batch->items[batch->count++] = b;
            object_retain(&b->object);
        }
        instance_release(instance);
        if(status < 0)
            break;
    }
    if(status < 0)
    {
        for(uint32_t i = 0; i < batch->count; ++i)
            object_release(&batch->items[i]->object);
        kfree(batch);
        return status;
    }
    batch->caller = caller;
    retain(caller);
    caller->outbound += batch->count;
    for(uint32_t i = 0; i < batch->count; ++i)
    {
        auto* b = batch->items[i];
        revoke(b, caller, false);
        set_owner(b, nullptr);
        b->borrowed = true;
    }
    out = batch;
    return 0;
}
void buffer_finish_loans(BufferLoanBatch* b)
{
    if(!b)
        return;
    BufferGuard guard;
    if(b->finished)
        return;
    for(uint32_t i = 0; i < b->count; ++i)
    {
        auto* item = b->items[i];
        if(b->borrower)
            revoke(item, b->borrower, true, true);
        item->borrowed = false;
        restore(item, b->caller);
    }
    if(b->borrower)
        b->borrower->inbound -= b->count;
    b->caller->outbound -= b->count;
    b->finished = true;
}
void buffer_release_loans(BufferLoanBatch* b)
{
    if(!b)
        return;
    BufferGuard guard;
    // The terminal IPC transition must finish before dropping its batch.
    buffer_finish_loans(b);
    for(uint32_t i = 0; i < b->count; ++i)
        object_release(&b->items[i]->object);
    release(b->borrower);
    release(b->caller);
    kfree(b);
}
void buffer_cancel_loans(BufferLoanBatch* b)
{
    buffer_finish_loans(b);
    buffer_release_loans(b);
}
long buffer_receive_loans(BufferLoanBatch* b, Thread* t, os1_ipc_buffers& out)
{
    out = {};
    if(!b || !t || !t->process)
        return OS1_ERR_INVALID;
    BufferGuard guard;
    if(b->finished || b->borrower)
        return OS1_ERR_INVALID;
    auto* borrower = budget(t->process);
    if(!borrower)
        return OS1_ERR_NO_MEMORY;
    if(b->count > OS1_BUFFER_PROCESS_LOAN_LIMIT - borrower->inbound)
        return OS1_ERR_LIMIT;
    for(uint32_t i = 0; i < b->count; ++i)
    {
        set_owner(b->items[i], borrower);
        const auto fd = install(b->items[i], t, true);
        if(fd < 0)
        {
            for(uint32_t j = 0; j < out.count; ++j)
                descriptor_close(t->process->descriptors, out.entries[j].fd);
            for(uint32_t j = 0; j <= i; ++j)
                set_owner(b->items[j], nullptr);
            out = {};
            return fd;
        }
        out.entries[out.count++] = {static_cast<int32_t>(fd), OS1_BUFFER_LOAN};
    }
    b->borrower = borrower;
    retain(borrower);
    borrower->inbound += b->count;
    return 0;
}
void buffer_rollback_receive(BufferLoanBatch* b, Thread* t, const os1_ipc_buffers& in)
{
    if(!b || !t)
        return;
    BufferGuard guard;
    for(uint32_t i = 0; i < in.count; ++i)
        descriptor_close(t->process->descriptors, in.entries[i].fd);
    for(uint32_t i = 0; i < b->count; ++i)
    {
        revoke(b->items[i], b->borrower, true, true);
        set_owner(b->items[i], nullptr);
    }
    if(b->borrower)
    {
        b->borrower->inbound -= b->count;
        release(b->borrower);
        b->borrower = nullptr;
    }
}
long buffer_prepare_outputs(PageFrameContainer&,
                            Thread* t,
                            BufferLoanBatch* loans,
                            const os1_ipc_buffers& in,
                            BufferOutputBatch*& out)
{
    out = nullptr;
    if(!loans || !t || !t->process || !valid_batch(in))
        return OS1_ERR_INVALID;
    BufferGuard guard;
    auto* source = budget(t->process);
    if(!source)
        return OS1_ERR_NO_MEMORY;
    if(loans->finished || loans->borrower != source)
        return OS1_ERR_ACCESS;
    auto* b =
        static_cast<BufferOutputBatch*>(kmalloc(sizeof(BufferOutputBatch), KmallocFlags::Zero));
    if(!b)
        return OS1_ERR_NO_MEMORY;
    long status = 0;
    for(uint32_t i = 0; i < in.count; ++i)
    {
        const auto disposition = in.entries[i].disposition;
        if(disposition != OS1_BUFFER_RETURN && disposition != OS1_BUFFER_MOVE)
        {
            status = OS1_ERR_INVALID;
            break;
        }
        OpenInstance* instance = nullptr;
        Buffer* item = nullptr;
        status = acquire(t,
                         in.entries[i].fd,
                         OS1_RIGHT_READ | OS1_RIGHT_WRITE |
                             (disposition == OS1_BUFFER_MOVE ? OS1_RIGHT_DELEGATE : 0),
                         instance,
                         item);
        if(status < 0)
            break;
        if(!authority(item, source, instance))
            status = OS1_ERR_ACCESS;
        bool input = false;
        for(uint32_t j = 0; j < loans->count; ++j)
            if(loans->items[j] == item)
                input = true;
        if(disposition == OS1_BUFFER_RETURN ? !input : (input || item->borrowed))
            status = OS1_ERR_ACCESS;
        for(uint32_t j = 0; j < i; ++j)
            if(b->items[j] == item)
                status = OS1_ERR_INVALID;
        if(status >= 0)
        {
            b->items[b->count] = item;
            b->disposition[b->count++] = disposition;
            object_retain(&item->object);
        }
        instance_release(instance);
        if(status < 0)
            break;
    }
    if(status < 0)
    {
        for(uint32_t i = 0; i < b->count; ++i)
            object_release(&b->items[i]->object);
        kfree(b);
        return status;
    }
    b->source = source;
    retain(source);
    b->caller = loans->caller;
    retain(b->caller);
    for(uint32_t i = 0; i < b->count; ++i)
        if(b->disposition[i] == OS1_BUFFER_MOVE)
        {
            revoke(b->items[i], source, false);
            set_owner(b->items[i], nullptr);
        }
    out = b;
    return 0;
}
void buffer_cancel_outputs(BufferOutputBatch* b)
{
    if(!b)
        return;
    BufferGuard guard;
    for(uint32_t i = 0; i < b->count; ++i)
        if(b->disposition[i] == OS1_BUFFER_MOVE)
            restore(b->items[i], b->source);
    b->consumed = true;
    buffer_release_outputs(b);
}
long buffer_claim_outputs(BufferOutputBatch* b, Thread* t, os1_ipc_buffers& out)
{
    out = {};
    if(!b)
        return 0;
    if(!t || !t->process)
        return OS1_ERR_INVALID;
    BufferGuard guard;
    if(b->caller->process != t->process)
        return OS1_ERR_ACCESS;
    if(b->consumed)
        return 0;
    if(b->claimed)
        return OS1_ERR_WOULD_BLOCK;
    for(uint32_t i = 0; i < b->count; ++i)
    {
        if(b->disposition[i] == OS1_BUFFER_MOVE)
            set_owner(b->items[i], b->caller);
        const auto fd = install(b->items[i], t, false);
        if(fd < 0)
        {
            for(uint32_t j = 0; j < out.count; ++j)
                descriptor_close(t->process->descriptors, out.entries[j].fd);
            for(uint32_t j = 0; j <= i; ++j)
                if(b->disposition[j] == OS1_BUFFER_MOVE)
                    set_owner(b->items[j], nullptr);
            out = {};
            return fd;
        }
        out.entries[out.count++] = {static_cast<int32_t>(fd), b->disposition[i]};
    }
    b->claimed = true;
    return 0;
}
void buffer_rollback_claim(BufferOutputBatch* b, Thread* t, const os1_ipc_buffers& in)
{
    if(!b || !t)
        return;
    BufferGuard guard;
    if(!b->claimed || b->consumed)
        return;
    for(uint32_t i = 0; i < in.count; ++i)
        descriptor_close(t->process->descriptors, in.entries[i].fd);
    for(uint32_t i = 0; i < b->count; ++i)
        if(b->disposition[i] == OS1_BUFFER_MOVE)
        {
            revoke(b->items[i], b->caller, true);
            set_owner(b->items[i], nullptr);
        }
    b->claimed = false;
}
void buffer_commit_claim(BufferOutputBatch* b)
{
    if(!b)
        return;
    BufferGuard guard;
    if(!b->claimed || b->consumed)
        return;
    b->consumed = true;
    b->claimed = false;
    for(uint32_t i = 0; i < b->count; ++i)
        if(b->disposition[i] == OS1_BUFFER_MOVE)
            for(auto& m : b->items[i]->maps)
                if(m.budget == b->source && !m.present)
                    erase_map(b->items[i], m);
}
void buffer_release_outputs(BufferOutputBatch* b)
{
    if(!b)
        return;
    BufferGuard guard;
    for(uint32_t i = 0; i < b->count; ++i)
    {
        if(!b->consumed && b->disposition[i] == OS1_BUFFER_MOVE)
            for(auto& m : b->items[i]->maps)
                if(m.budget == b->source && !m.present)
                    erase_map(b->items[i], m);
        object_release(&b->items[i]->object);
    }
    release(b->caller);
    release(b->source);
    kfree(b);
}
void buffer_process_exec(PageFrameContainer&, Process* p)
{
    BufferGuard guard;
    auto* owner = budget(p, false);
    if(!owner)
        return;
    ++owner->space_generation;
    for(auto* b = objects; b;)
    {
        auto* next = b->next;
        object_retain(&b->object);
        revoke(b, owner, true);
        object_release(&b->object);
        b = next;
    }
}
void buffer_process_cleanup(PageFrameContainer& frames, Process* p)
{
    BufferGuard guard;
    auto* owner = budget(p, false);
    if(!owner)
        return;
    buffer_process_exec(frames, p);
    owner->process = nullptr;
    release(owner);
}
