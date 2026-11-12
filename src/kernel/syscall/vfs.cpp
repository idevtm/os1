#include "syscall/vfs.hpp"

#include "arch/x86_64/apic/ipi.hpp"
#include "arch/x86_64/cpu/cpu.hpp"
#include "debug/log_export.hpp"
#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "mm/user_copy.hpp"
#include "util/memory.h"
#include "vfs/filesystem.hpp"
#include "vfs/initrd.hpp"
#include "vfs/namespace.hpp"

namespace
{
WaitQueue waiters{"vfs-cursor"};
bool vfs_wait_pending = false;
Thread* vfs_worker_thread = nullptr;
PageFrameContainer* vfs_worker_frames = nullptr;

void vfs_worker()
{
#ifndef OS1_HOST_TEST
    Thread* thread = current_thread();
    for(;;)
    {
        service_vfs_waiters(*vfs_worker_frames);
        IrqGuard irq;
        thread->wait.reason = ThreadWaitReason::VfsWorker;
        thread->state = ThreadState::Blocked;
        while(thread->state == ThreadState::Blocked)
        {
            (void)ipi_send_reschedule(cpu_cur());
            asm volatile("sti; hlt; cli" ::: "memory");
        }
    }
#endif
}

constexpr long kCursorBusy = -4097;
struct CursorReservation
{
    OpenInstance* instance;
    ~CursorReservation()
    {
        atomic_store_release(&instance->cursor_busy, false);
    }
};
constexpr size_t kUserCopyChunkBytes = 256;
struct IoBuffer
{
    PageFrameContainer& frames;
    uint8_t small[OS1_DIRECTORY_RECORD_MAX]{};
    uint64_t physical = 0;
    uint8_t* large = nullptr;

    IoBuffer(PageFrameContainer& frames, size_t preferred, size_t length) : frames(frames)
    {
        if(preferred > sizeof(small) && preferred <= kPageSize && length > kUserCopyChunkBytes &&
           frames.allocate(physical))
            large = kernel_physical_pointer<uint8_t>(physical);
    }

    ~IoBuffer()
    {
        if(large)
            (void)frames.free(physical);
    }
};
long perform(PageFrameContainer& frames, Thread* thread, const VfsIoWaitState& request)
{
    auto* instance = request.instance;
    bool expected = false;
    if(!atomic_compare_exchange_strong(&instance->cursor_busy, &expected, true))
        return kCursorBusy;
    CursorReservation guard{instance};
    auto* node = static_cast<VfsNode*>(instance->object);
    if(node->operations)
    {
        if(request.operation == OS1_RESOURCE_SEEK)
        {
            long result =
                node->operations->seek(node, instance->cursor, request.offset, request.origin);
            if(result >= 0)
                instance->cursor = result;
            return result;
        }
        const bool file_io = request.operation == kVfsRead || request.operation == kVfsWrite;
        const size_t preferred =
            file_io ? node->operations->preferred_io_bytes : kUserCopyChunkBytes;
        IoBuffer buffer(frames, preferred, request.length);
        auto* bytes = buffer.large ? buffer.large : buffer.small;
        size_t delivered = 0;
        while(delivered < request.length)
        {
            size_t limit = kUserCopyChunkBytes;
            const bool append = request.operation == kVfsWrite &&
                                (atomic_load_acquire(&instance->status_flags) & OS1_STATUS_APPEND);
            if(buffer.large)
            {
                // Append chooses the actual offset under the backend guard.
                const size_t remaining =
                    append ? preferred : preferred - instance->cursor % preferred;
                // Keep the user-copy commit boundaries when the file offset is unaligned.
                const size_t aligned = remaining / kUserCopyChunkBytes * kUserCopyChunkBytes;
                if(aligned > limit)
                    limit = aligned;
            }
            size_t count = request.length - delivered;
            if(count > limit)
                count = limit;
            uint64_t next = instance->cursor;
            if(request.operation == kVfsRead)
            {
                long result = node->operations->read(node, instance->cursor, bytes, count);
                if(result < 0)
                    return delivered ? static_cast<long>(delivered) : result;
                if(!result)
                    break;
                const size_t read = static_cast<size_t>(result);
                for(size_t copied = 0; copied < read;)
                {
                    const size_t remaining = read - copied;
                    const size_t chunk =
                        remaining > kUserCopyChunkBytes ? kUserCopyChunkBytes : remaining;
                    if(request.buffer > UINT64_MAX - delivered ||
                       !copy_to_user(
                           frames, thread, request.buffer + delivered, bytes + copied, chunk))
                        return delivered ? static_cast<long>(delivered) : OS1_ERR_FAULT;
                    instance->cursor += chunk;
                    delivered += chunk;
                    copied += chunk;
                }
                continue;
            }
            else if(request.operation == kVfsWrite)
            {
                if(request.buffer > UINT64_MAX - delivered)
                    return delivered ? static_cast<long>(delivered) : OS1_ERR_FAULT;
                const auto address = request.buffer + delivered;
                if(!copy_from_user(frames, thread, address, bytes, count))
                {
                    if(count <= kUserCopyChunkBytes ||
                       !copy_from_user(frames, thread, address, bytes, kUserCopyChunkBytes))
                        return delivered ? static_cast<long>(delivered) : OS1_ERR_FAULT;
                    count = kUserCopyChunkBytes;
                }
                uint64_t end = instance->cursor;
                long result =
                    node->operations->write(node, instance->cursor, bytes, count, append, &end);
                if(count > kUserCopyChunkBytes &&
                   (result == OS1_ERR_LIMIT || result == OS1_ERR_RANGE))
                    result = node->operations->write(
                        node, instance->cursor, bytes, kUserCopyChunkBytes, append, &end);
                if(result < 0)
                    return delivered ? static_cast<long>(delivered) : result;
                if(!result)
                    break;
                instance->cursor = end;
                delivered += result;
                continue;
            }
            else
            {
                os1_directory_entry entry{};
                long result = node->operations->next(node, instance->cursor, entry, next);
                if(result < 0)
                    return delivered ? static_cast<long>(delivered) : result;
                if(!result)
                    break;
                uint32_t required = (sizeof(os1_directory_record) + entry.name_bytes + 7) & ~7u;
                if(required > request.length - delivered)
                {
                    if(delivered)
                        return delivered;
                    if(!copy_to_user(frames, thread, request.required, &required, sizeof(required)))
                        return OS1_ERR_FAULT;
                    return OS1_ERR_BUFFER_TOO_SMALL;
                }
                memset(bytes, 0, required);
                os1_directory_record header{OS1_VFS_ABI_VERSION,
                                            required,
                                            static_cast<uint16_t>(entry.name_bytes),
                                            static_cast<uint16_t>(entry.kind),
                                            0};
                memcpy(bytes, &header, sizeof(header));
                memcpy(bytes + sizeof(header), entry.name, entry.name_bytes);
                count = required;
            }
            if(request.buffer > UINT64_MAX - delivered ||
               !copy_to_user(frames, thread, request.buffer + delivered, bytes, count))
                return delivered ? static_cast<long>(delivered) : OS1_ERR_FAULT;
            instance->cursor = next;
            delivered += count;
        }
        return delivered;
    }
    if(request.operation == OS1_RESOURCE_SEEK)
    {
        uint64_t base = request.origin == OS1_SEEK_CUR   ? instance->cursor
                        : request.origin == OS1_SEEK_END ? node->byte_size
                                                         : 0;
        uint64_t position = 0;
        if(request.offset < 0)
        {
            uint64_t delta = static_cast<uint64_t>(-(request.offset + 1)) + 1;
            if(delta > base)
                return OS1_ERR_RANGE;
            position = base - delta;
        }
        else
        {
            if(static_cast<uint64_t>(request.offset) > INT64_MAX - base)
                return OS1_ERR_RANGE;
            position = base + request.offset;
        }
        instance->cursor = position;
        return position;
    }
    if(!request.length)
        return 0;
    size_t delivered = 0;
    if(request.operation == kVfsRead)
    {
        while(delivered < request.length && instance->cursor < node->byte_size)
        {
            size_t count = request.length - delivered;
            if(count > 256)
                count = 256;
            if(count > node->byte_size - instance->cursor)
                count = node->byte_size - instance->cursor;
            if(request.buffer > UINT64_MAX - delivered ||
               !copy_to_user(frames,
                             thread,
                             request.buffer + delivered,
                             node->bytes + instance->cursor,
                             count))
                return delivered ? static_cast<long>(delivered) : OS1_ERR_FAULT;
            delivered += count;
            instance->cursor += count;
        }
        return delivered;
    }
    auto* child = node->children;
    for(uint64_t i = 0; child && i < instance->cursor; ++i)
        child = child->next;
    while(child)
    {
        const uint32_t bytes = (sizeof(os1_directory_record) + child->name_size + 7) & ~7u;
        if(bytes > request.length - delivered)
        {
            if(delivered)
                return delivered;
            if(!copy_to_user(frames, thread, request.required, &bytes, sizeof(bytes)))
                return OS1_ERR_FAULT;
            return OS1_ERR_BUFFER_TOO_SMALL;
        }
        alignas(8) uint8_t record[OS1_DIRECTORY_RECORD_MAX]{};
        auto* header = reinterpret_cast<os1_directory_record*>(record);
        *header = {
            OS1_VFS_ABI_VERSION, bytes, child->name_size, static_cast<uint16_t>(child->kind), 0};
        memcpy(record + sizeof(*header), child->name, child->name_size);
        if(request.buffer > UINT64_MAX - delivered ||
           !copy_to_user(frames, thread, request.buffer + delivered, record, bytes))
            return delivered ? static_cast<long>(delivered) : OS1_ERR_FAULT;
        delivered += bytes;
        ++instance->cursor;
        child = child->next;
    }
    return delivered;
}
long perform_or_block(PageFrameContainer& frames, Thread* thread, const VfsIoWaitState& request)
{
    const long result = perform(frames, thread, request);
    if(result != kCursorBusy)
        return result;
    // The BSP tick wakes the VFS worker while cursor operations remain queued.
    // Registration after a cursor release still receives a retry.
    IrqSpinGuard process_guard(g_process_table_lock);
    IrqSpinGuard queue_guard(waiters.lock);
    instance_retain(request.instance);
    thread->console_cancelled = false;
    thread->wait = {};
    thread->wait.reason = ThreadWaitReason::VfsIo;
    thread->wait.vfs = request;
    thread->state = ThreadState::Blocked;
    thread->process->state = ProcessState::Ready;
    wait_queue_enqueue_locked(waiters, thread);
    atomic_store_release(&vfs_wait_pending, true);
    return kVfsBlocked;
}
}  // namespace
long vfs_read_or_block(PageFrameContainer& frames,
                       Thread* thread,
                       OpenInstance* instance,
                       uint64_t buffer,
                       size_t length)
{
    if(length > OS1_IO_MAX_BYTES)
        return OS1_ERR_INVALID;
    return perform_or_block(frames, thread, {instance, buffer, length, 0, 0, kVfsRead, 0});
}
long vfs_write_or_block(PageFrameContainer& frames,
                        Thread* thread,
                        OpenInstance* instance,
                        uint64_t buffer,
                        size_t length)
{
    if(length > OS1_IO_MAX_BYTES)
        return OS1_ERR_INVALID;
    if(!vfs_backed(instance->object) || instance->object->kind != OS1_OBJECT_FILE)
        return OS1_ERR_UNSUPPORTED;
    return perform_or_block(frames, thread, {instance, buffer, length, 0, 0, kVfsWrite, 0});
}
long sys_vfs(
    PageFrameContainer& frames, Thread* thread, int fd, uint64_t operation, uint64_t argument)
{
    auto& table = thread->process->descriptors;
    if(operation == OS1_RESOURCE_TREE_STEP)
    {
        os1_tree_step step{};
        if(!copy_from_user(frames, thread, argument, &step, sizeof(step)))
            return OS1_ERR_FAULT;
        long r = vfs_tree_step(table, fd, step);
        if(!copy_to_user(frames, thread, argument, &step, sizeof(step)))
        {
            if(r >= 0 && (step.action == OS1_TREE_OPEN || step.action == OS1_TREE_CREATE))
                descriptor_close(table, r);
            return OS1_ERR_FAULT;
        }
        return r;
    }
    if(operation == OS1_RESOURCE_TIMES || operation == OS1_RESOURCE_SET_TIMES)
    {
        const bool set = operation == OS1_RESOURCE_SET_TIMES;
        OpenInstance* instance = nullptr;
        long r =
            descriptor_acquire(table, fd, set ? OS1_RIGHT_SET_TIMES : OS1_RIGHT_INSPECT, instance);
        if(r < 0)
            return r;
        InstanceRef pin(instance);
        if(instance->object->kind != OS1_OBJECT_FILE &&
           instance->object->kind != OS1_OBJECT_DIRECTORY)
            return OS1_ERR_UNSUPPORTED;
        os1_set_times request{};
        if(set && !copy_from_user(frames, thread, argument, &request, sizeof(request)))
            return OS1_ERR_FAULT;
        NamespaceGuard guard;
        if(!guard.held)
            return OS1_ERR_WOULD_BLOCK;
        auto* node = static_cast<VfsNode*>(instance->object);
        if(set)
            return vfs_set_times(node, request);
        os1_file_times result{};
        r = vfs_times(node, result);
        return r < 0                                                             ? r
               : copy_to_user(frames, thread, argument, &result, sizeof(result)) ? 0
                                                                                 : OS1_ERR_FAULT;
    }
    if(operation == OS1_RESOURCE_METADATA || operation == OS1_RESOURCE_SPACE)
    {
        OpenInstance* instance = nullptr;
        long r = descriptor_acquire(table, fd, OS1_RIGHT_INSPECT, instance);
        if(r < 0)
            return r;
        InstanceRef pin(instance);
        if(instance->object->kind != OS1_OBJECT_FILE &&
           instance->object->kind != OS1_OBJECT_DIRECTORY)
            return OS1_ERR_UNSUPPORTED;
        NamespaceGuard guard;
        if(!guard.held)
            return OS1_ERR_WOULD_BLOCK;
        auto* node = static_cast<VfsNode*>(instance->object);
        if(operation == OS1_RESOURCE_METADATA)
        {
            os1_metadata result{};
            r = vfs_metadata(node, result);
            return r < 0 ? r
                   : copy_to_user(frames, thread, argument, &result, sizeof(result))
                       ? 0
                       : OS1_ERR_FAULT;
        }
        os1_space result{};
        r = vfs_space(node, result);
        return r < 0                                                             ? r
               : copy_to_user(frames, thread, argument, &result, sizeof(result)) ? 0
                                                                                 : OS1_ERR_FAULT;
    }
    if(operation == OS1_RESOURCE_SAME_OBJECT)
        return argument < OS1_DESCRIPTOR_LIMIT ? vfs_same_object(table, fd, argument)
                                               : OS1_ERR_BAD_DESCRIPTOR;
    if(operation == OS1_RESOURCE_MOVE_AT)
    {
        os1_move_request request{};
        if(!copy_from_user(frames, thread, argument, &request, sizeof(request)))
            return OS1_ERR_FAULT;
        uint64_t source_id = 0;
        if(request.version == OS1_MOVE_GUARDED_VERSION)
        {
            if(request.size != sizeof(os1_guarded_move_request))
                return OS1_ERR_INVALID;
            if(!copy_from_user(
                   frames, thread, argument + sizeof(request), &source_id, sizeof(source_id)))
                return OS1_ERR_FAULT;
            if(!source_id)
                return OS1_ERR_INVALID;
        }
        else if(request.version != OS1_VFS_ABI_VERSION || request.size != sizeof(request))
            return OS1_ERR_INVALID;
        if(request.replace > 1 || !request.source_bytes || request.source_bytes > 63 ||
           !request.destination_bytes || request.destination_bytes > 63)
            return OS1_ERR_INVALID;
        for(unsigned i = request.source_bytes; i < sizeof(request.source); ++i)
            if(request.source[i])
                return OS1_ERR_INVALID;
        for(unsigned i = request.destination_bytes; i < sizeof(request.destination); ++i)
            if(request.destination[i])
                return OS1_ERR_INVALID;
        return vfs_move_at(table, fd, request, source_id);
    }
    if(operation == OS1_RESOURCE_CONTEXT_OPEN)
    {
        os1_context_open_request request{};
        if(!copy_from_user(frames, thread, argument, &request, sizeof(request)))
            return OS1_ERR_FAULT;
        if(request.version != OS1_VFS_ABI_VERSION || request.size != sizeof(request) ||
           request.reserved || !request.path_bytes || request.path_bytes > OS1_PATH_MAX)
            return OS1_ERR_INVALID;
        auto* path = static_cast<char*>(kmalloc(request.path_bytes));
        if(!path)
            return OS1_ERR_NO_MEMORY;
        long r = copy_from_user(frames, thread, request.path, path, request.path_bytes)
                     ? vfs_context_open(table, fd, request, path)
                     : OS1_ERR_FAULT;
        kfree(path);
        return r;
    }
    if(operation == OS1_RESOURCE_DESCRIBE_PATH)
    {
        os1_path_request request{};
        if(!copy_from_user(frames, thread, argument, &request, sizeof(request)))
            return OS1_ERR_FAULT;
        if(request.version != OS1_VFS_ABI_VERSION || request.size != sizeof(request) ||
           !request.capacity || request.capacity > OS1_PATH_MAX + 1)
            return OS1_ERR_INVALID;
        auto* path = static_cast<char*>(kmalloc(request.capacity));
        if(!path)
            return OS1_ERR_NO_MEMORY;
        long r = vfs_describe_path(table, fd, request.object_fd, path, request.capacity);
        if(r >= 0 && !copy_to_user(frames, thread, request.buffer, path, r + 1))
            r = OS1_ERR_FAULT;
        kfree(path);
        return r;
    }
    if(operation == OS1_RESOURCE_DUPLICATE_SCOPED)
    {
        os1_scoped_duplicate_request request{};
        if(!copy_from_user(frames, thread, argument, &request, sizeof(request)))
            return OS1_ERR_FAULT;
        return descriptor_duplicate_scoped(table, fd, request);
    }
    if(operation == OS1_RESOURCE_OPEN_AT)
    {
        os1_open_request request{};
        if(!copy_from_user(frames, thread, argument, &request, sizeof(request)))
            return OS1_ERR_FAULT;
        if(request.version != OS1_VFS_ABI_VERSION || request.size != sizeof(request) ||
           !request.path_bytes || request.path_bytes > OS1_PATH_MAX)
            return OS1_ERR_INVALID;
        auto* path = static_cast<char*>(kmalloc(request.path_bytes));
        if(!path)
            return OS1_ERR_NO_MEMORY;
        const long result = copy_from_user(frames, thread, request.path, path, request.path_bytes)
                                ? vfs_open_descriptor(table,
                                                      fd,
                                                      path,
                                                      request.path_bytes,
                                                      request.rights,
                                                      request.descriptor_flags,
                                                      request.descendant_rights)
                                : OS1_ERR_FAULT;
        kfree(path);
        return result;
    }
    uint32_t right = operation == OS1_RESOURCE_READ_DIRECTORY ? OS1_RIGHT_ENUMERATE
                     : operation == OS1_RESOURCE_SEEK         ? OS1_RIGHT_SEEK
                                                              : OS1_RIGHT_INSPECT;
    DescriptorEntry entry{};
    long result = descriptor_acquire_entry(table, fd, right, entry);
    if(result < 0)
        return result;
    InstanceRef pin(entry.instance);
    const auto kind = entry.instance->object->kind;
    if(operation == OS1_RESOURCE_FILE_INFO)
    {
        if(kind != OS1_OBJECT_FILE && kind != OS1_OBJECT_DIRECTORY)
            return OS1_ERR_UNSUPPORTED;
        const auto* node = static_cast<VfsNode*>(entry.instance->object);
        os1_file_info info{OS1_VFS_ABI_VERSION,
                           sizeof(info),
                           kind,
                           entry.descendant_rights,
                           node->operations
                               ? node->operations->info(const_cast<VfsNode*>(node)).byte_size
                               : node->byte_size,
                           OS1_DIRECTORY_RECORD_MAX,
                           0};
        return copy_to_user(frames, thread, argument, &info, sizeof(info)) ? 0 : OS1_ERR_FAULT;
    }
    if(operation == OS1_RESOURCE_READ_DIRECTORY)
    {
        if(kind != OS1_OBJECT_DIRECTORY)
            return OS1_ERR_NOT_DIRECTORY;
        os1_directory_request request{};
        if(!copy_from_user(frames, thread, argument, &request, sizeof(request)))
            return OS1_ERR_FAULT;
        if(request.version != OS1_VFS_ABI_VERSION || request.size != sizeof(request) ||
           request.capacity > OS1_IO_MAX_BYTES)
            return OS1_ERR_INVALID;
        return perform_or_block(frames,
                                thread,
                                {entry.instance,
                                 request.buffer,
                                 request.capacity,
                                 argument + offsetof(os1_directory_request, required_bytes),
                                 0,
                                 OS1_RESOURCE_READ_DIRECTORY,
                                 0});
    }
    if(kind != OS1_OBJECT_FILE)
        return OS1_ERR_UNSUPPORTED;
    os1_seek_request request{};
    if(!copy_from_user(frames, thread, argument, &request, sizeof(request)))
        return OS1_ERR_FAULT;
    if(request.version != OS1_VFS_ABI_VERSION || request.size != sizeof(request) ||
       request.origin > OS1_SEEK_END || request.reserved)
        return OS1_ERR_INVALID;
    return perform_or_block(
        frames,
        thread,
        {entry.instance, 0, 0, 0, request.offset, OS1_RESOURCE_SEEK, request.origin});
}
bool start_vfs_worker(Process* process, PageFrameContainer& frames)
{
    vfs_worker_frames = &frames;
    vfs_worker_thread = create_kernel_thread(process, vfs_worker, frames, g_cpu_boot, false);
    if(!vfs_worker_thread)
        return false;
    vfs_worker_thread->wait.reason = ThreadWaitReason::VfsWorker;
    return true;
}

void wake_vfs_worker_on_tick()
{
    if(atomic_load_acquire(&vfs_wait_pending) && vfs_worker_thread &&
       vfs_worker_thread->state == ThreadState::Blocked &&
       vfs_worker_thread->wait.reason == ThreadWaitReason::VfsWorker)
        wake_blocked_thread(vfs_worker_thread, g_cpu_boot);
}

void cancel_vfs_wait(Thread* thread)
{
    if(!thread)
        return;
    OpenInstance* released = nullptr;
    {
        IrqSpinGuard guard(waiters.lock);
        if(thread->wait.reason != ThreadWaitReason::VfsIo)
            return;
        thread->console_cancelled = true;
        Thread** link = &waiters.head;
        while(*link && *link != thread)
            link = &(*link)->wait_link;
        if(*link)
        {
            *link = thread->wait_link;
            thread->wait_link = nullptr;
            if(!waiters.head)
                atomic_store_release(&vfs_wait_pending, false);
        }
        if(!atomic_load_acquire(&thread->io_pins))
        {
            released = thread->wait.vfs.instance;
            thread->wait = {};
        }
    }
    instance_release(released);
}
void service_vfs_waiters(PageFrameContainer& frames)
{
    log_export::ForegroundGuard foreground;
    if(!foreground.held)
        return;
    size_t count = wait_queue_count(waiters);
    if(count > 64)
        count = 64;
    while(count--)
    {
        Thread* thread = nullptr;
        VfsIoWaitState request{};
        {
            IrqSpinGuard guard(waiters.lock);
            thread = waiters.head;
            if(!thread)
                return;
            bool current = false;
            for(cpu* owner = g_cpu_boot; owner; owner = owner->next)
                if(__atomic_load_n(&owner->current_thread, __ATOMIC_ACQUIRE) == thread)
                    current = true;
            if(current || thread->state == ThreadState::Dying)
                return;
            (void)wait_queue_dequeue_locked(waiters);
            if(!waiters.head)
                atomic_store_release(&vfs_wait_pending, false);
            (void)atomic_fetch_add(&thread->io_pins, 1u);
            request = thread->wait.vfs;
        }
        const long result = perform(frames, thread, request);
        OpenInstance* released = nullptr;
        bool wake = false;
        {
            IrqSpinGuard guard(waiters.lock);
            if(thread->console_cancelled || thread->state == ThreadState::Dying ||
               result != kCursorBusy)
            {
                wake = !thread->console_cancelled && thread->state != ThreadState::Dying;
                released = thread->wait.vfs.instance;
                thread->wait = {};
                if(wake)
                    thread->frame.rax = static_cast<uint64_t>(result);
            }
            else
            {
                wait_queue_enqueue_locked(waiters, thread);
                atomic_store_release(&vfs_wait_pending, true);
            }
        }
        instance_release(released);
        if(wake)
            wake_blocked_thread(thread);
        (void)__atomic_fetch_sub(&thread->io_pins, 1u, __ATOMIC_RELEASE);
    }
}
