#include "syscall/process.hpp"

#include <os1/observe.h>

#include "debug/debug.hpp"
#include "mm/user_copy.hpp"
#include "proc/thread.hpp"
#include "proc/user_program.hpp"
#include "util/fixed_string.hpp"

long sys_write(const ProcessSyscallContext& context, int fd, uint64_t user_buffer, size_t length)
{
    Thread* thread = current_thread();
    if(!thread || !thread->process || !context.frames)
        return OS1_ERR_INVALID;
    OpenInstance* instance = nullptr;
    const long status = descriptor_acquire(thread->process->descriptors, fd, OS1_RIGHT_WRITE, instance);
    if(status < 0) return status;
    InstanceRef retained(instance);
    if(!(instance->object->interfaces & OS1_INTERFACE_BYTE_OUTPUT) || !instance->object->write_bytes)
        return OS1_ERR_UNSUPPORTED;
    if(length > OS1_IO_MAX_BYTES) return OS1_ERR_INVALID;
    if(length == 0) return 0;
    char buffer[128];
    size_t written = 0;
    while(written < length)
    {
        const size_t chunk = ((length - written) < sizeof(buffer)) ? length - written : sizeof(buffer);
        if(user_buffer > ~uint64_t{0} - written ||
           !copy_from_user(*context.frames, thread, user_buffer + written, buffer, chunk))
            return written ? static_cast<long>(written) : OS1_ERR_FAULT;
        instance->object->write_bytes(buffer, chunk);
        written += chunk;
    }
    return static_cast<long>(written);
}

long sys_spawn(const ProcessSyscallContext& context, uint64_t user_path)
{
    if(nullptr == context.frames)
    {
        return -1;
    }

    Thread* thread = current_thread();
    if((nullptr == thread) || (nullptr == thread->process))
    {
        return -1;
    }

    char path[OS1_OBSERVE_INITRD_PATH_BYTES];
    if(!copy_user_string(*context.frames, thread, user_path, path, sizeof(path)))
    {
        return -1;
    }

    Thread* child =
        load_user_program(*context.frames, context.kernel_root_cr3, path, thread->process, false);
    if((nullptr == child) || (nullptr == child->process))
    {
        return -1;
    }
    // Capture the result before publication: an AP may run and exit the child
    // immediately, allowing the BSP to reclaim its Thread while we return.
    const long pid = static_cast<long>(child->process->pid);
    mark_thread_ready(child, child->scheduler_cpu);
    return pid;
}

long sys_exec(const ProcessSyscallContext& context, uint64_t user_path)
{
    if((nullptr == context.frames) || (nullptr == context.write_cr3))
    {
        return -1;
    }

    Thread* thread = current_thread();
    if((nullptr == thread) || (nullptr == thread->process) || !thread->user_mode)
    {
        return -1;
    }

    char path[OS1_OBSERVE_INITRD_PATH_BYTES];
    if(!copy_user_string(*context.frames, thread, user_path, path, sizeof(path)))
    {
        return -1;
    }

    uint64_t new_cr3 = 0;
    uint64_t entry = 0;
    uint64_t user_rsp = 0;
    if(!load_user_program_image(
           *context.frames, context.kernel_root_cr3, path, new_cr3, entry, user_rsp))
    {
        return -1;
    }

    descriptor_close_exec(thread->process->descriptors);
    const uint64_t old_cr3 = thread->address_space_cr3;
    thread->address_space_cr3 = new_cr3;
    thread->process->address_space.cr3 = new_cr3;
    thread->process->state = ProcessState::Running;
    copy_fixed_string(thread->process->name, sizeof(thread->process->name), path);
    prepare_user_thread_entry(thread, entry, user_rsp);
    context.write_cr3(new_cr3);

    if((0 != old_cr3) && (old_cr3 != new_cr3) &&
       !destroy_user_address_space(*context.frames, old_cr3))
    {
        debug("exec old address-space teardown failed for ")(path)();
    }

    return 0;
}
