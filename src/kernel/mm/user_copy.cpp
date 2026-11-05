// Audited syscall copy boundary. Every user pointer is canonicalized, range
// checked, translated, and permission checked before bytes cross into kernel code.
#include "mm/user_copy.hpp"

#include "arch/x86_64/cpu/cpu.hpp"
#include "debug/event_ring.hpp"
#include "handoff/memory_layout.h"
#include "mm/user_address.hpp"
#include "mm/user_copy_lock.hpp"
#include "util/memory.h"

namespace
{
Spinlock g_copy_gate{"user-copy-retirement"};
#if defined(OS1_HOST_TEST)
thread_local unsigned g_copy_depth = 0;
#endif
[[nodiscard]] uint64_t user_copy_pid(const Thread* thread)
{
    return (thread && thread->process) ? thread->process->pid : 0;
}

[[nodiscard]] uint64_t user_copy_tid(const Thread* thread)
{
    return thread ? thread->tid : 0;
}

void record_user_copy_failure(const Thread* thread,
                              uint32_t flags,
                              uint64_t user_pointer,
                              size_t length,
                              uint64_t reason,
                              uint64_t offset = 0)
{
    kernel_event::record_subject(OS1_KERNEL_EVENT_USER_COPY_FAILURE,
                                 flags,
                                 user_copy_pid(thread),
                                 user_copy_tid(thread),
                                 user_pointer,
                                 static_cast<uint64_t>(length),
                                 reason,
                                 offset);
}
}  // namespace

void user_copy_lock()
{
#if defined(OS1_HOST_TEST)
    auto& depth = g_copy_depth;
#else
    auto& depth = cpu_cur()->user_copy_depth;
#endif
    if(depth++ == 0)
        g_copy_gate.lock();
}
void user_copy_unlock()
{
#if defined(OS1_HOST_TEST)
    auto& depth = g_copy_depth;
#else
    auto& depth = cpu_cur()->user_copy_depth;
#endif
    if(--depth == 0)
        g_copy_gate.unlock();
}

bool copy_into_address_space(VirtualMemory& vm,
                             uint64_t virtual_address,
                             const uint8_t* source,
                             uint64_t length)
{
    UserCopyGuard copy_guard;
    uint64_t copied = 0;
    while(copied < length)
    {
        uint64_t physical = 0;
        uint64_t flags = 0;
        if(!vm.translate(virtual_address + copied, physical, flags))
        {
            return false;
        }

        const uint64_t page_offset = (virtual_address + copied) & (kPageSize - 1);
        const uint64_t chunk = ((length - copied) < (kPageSize - page_offset))
                                   ? (length - copied)
                                   : (kPageSize - page_offset);
        memcpy(kernel_physical_pointer<void>(physical), source + copied, chunk);
        copied += chunk;
    }
    return true;
}

bool copy_to_user(PageFrameContainer& frames,
                  const Thread* thread,
                  uint64_t user_pointer,
                  const void* source,
                  size_t length)
{
    UserCopyGuard copy_guard;
    if((nullptr == thread) || (nullptr == source))
    {
        record_user_copy_failure(thread,
                                 OS1_KERNEL_EVENT_FLAG_TO_USER,
                                 user_pointer,
                                 length,
                                 OS1_KERNEL_EVENT_USER_COPY_NULL_CONTEXT);
        return false;
    }
    if(!user_address::is_user_address_range(user_pointer, length))
    {
        record_user_copy_failure(thread,
                                 OS1_KERNEL_EVENT_FLAG_TO_USER,
                                 user_pointer,
                                 length,
                                 OS1_KERNEL_EVENT_USER_COPY_BAD_RANGE);
        return false;
    }

    VirtualMemory vm(frames, thread->address_space_cr3);
    const uint8_t* src = (const uint8_t*)source;
    size_t copied = 0;
    while(copied < length)
    {
        uint64_t physical = 0;
        uint64_t flags = 0;
        if(!vm.translate(user_pointer + copied, physical, flags))
        {
            record_user_copy_failure(thread,
                                     OS1_KERNEL_EVENT_FLAG_TO_USER,
                                     user_pointer,
                                     length,
                                     OS1_KERNEL_EVENT_USER_COPY_TRANSLATE,
                                     copied);
            return false;
        }
        if(!user_address::has_required_user_flags(flags, true))
        {
            record_user_copy_failure(thread,
                                     OS1_KERNEL_EVENT_FLAG_TO_USER,
                                     user_pointer,
                                     length,
                                     OS1_KERNEL_EVENT_USER_COPY_PERMISSION,
                                     copied);
            return false;
        }
        const size_t page_offset = (user_pointer + copied) & (kPageSize - 1);
        const size_t chunk = ((length - copied) < (kPageSize - page_offset))
                                 ? (length - copied)
                                 : (kPageSize - page_offset);
        memcpy(kernel_physical_pointer<void>(physical), src + copied, chunk);
        copied += chunk;
    }
    return true;
}

bool copy_from_user(PageFrameContainer& frames,
                    const Thread* thread,
                    uint64_t user_pointer,
                    void* destination,
                    size_t length)
{
    UserCopyGuard copy_guard;
    if((nullptr == thread) || (nullptr == destination))
    {
        record_user_copy_failure(thread,
                                 OS1_KERNEL_EVENT_FLAG_FROM_USER,
                                 user_pointer,
                                 length,
                                 OS1_KERNEL_EVENT_USER_COPY_NULL_CONTEXT);
        return false;
    }
    if(!user_address::is_user_address_range(user_pointer, length))
    {
        record_user_copy_failure(thread,
                                 OS1_KERNEL_EVENT_FLAG_FROM_USER,
                                 user_pointer,
                                 length,
                                 OS1_KERNEL_EVENT_USER_COPY_BAD_RANGE);
        return false;
    }

    VirtualMemory vm(frames, thread->address_space_cr3);
    uint8_t* dest = (uint8_t*)destination;
    size_t copied = 0;
    while(copied < length)
    {
        uint64_t physical = 0;
        uint64_t flags = 0;
        if(!vm.translate(user_pointer + copied, physical, flags))
        {
            record_user_copy_failure(thread,
                                     OS1_KERNEL_EVENT_FLAG_FROM_USER,
                                     user_pointer,
                                     length,
                                     OS1_KERNEL_EVENT_USER_COPY_TRANSLATE,
                                     copied);
            return false;
        }
        if(!user_address::has_required_user_flags(flags, false))
        {
            record_user_copy_failure(thread,
                                     OS1_KERNEL_EVENT_FLAG_FROM_USER,
                                     user_pointer,
                                     length,
                                     OS1_KERNEL_EVENT_USER_COPY_PERMISSION,
                                     copied);
            return false;
        }
        const size_t page_offset = (user_pointer + copied) & (kPageSize - 1);
        const size_t chunk = ((length - copied) < (kPageSize - page_offset))
                                 ? (length - copied)
                                 : (kPageSize - page_offset);
        memcpy(dest + copied, kernel_physical_pointer<const void>(physical), chunk);
        copied += chunk;
    }
    return true;
}

bool copy_user_string(PageFrameContainer& frames,
                      const Thread* thread,
                      uint64_t user_pointer,
                      char* destination,
                      size_t destination_size)
{
    if((nullptr == thread) || (nullptr == destination) || (0 == destination_size) ||
       (0 == user_pointer))
    {
        record_user_copy_failure(thread,
                                 OS1_KERNEL_EVENT_FLAG_FROM_USER,
                                 user_pointer,
                                 destination_size,
                                 OS1_KERNEL_EVENT_USER_COPY_NULL_CONTEXT);
        return false;
    }

    for(size_t index = 0; index < destination_size; ++index)
    {
        char ch = 0;
        if(!copy_from_user(frames, thread, user_pointer + index, &ch, sizeof(ch)))
        {
            return false;
        }
        destination[index] = ch;
        if(0 == ch)
        {
            return true;
        }
    }

    destination[destination_size - 1] = 0;
    record_user_copy_failure(thread,
                             OS1_KERNEL_EVENT_FLAG_FROM_USER,
                             user_pointer,
                             destination_size,
                             OS1_KERNEL_EVENT_USER_COPY_STRING_TOO_LONG);
    return false;
}
