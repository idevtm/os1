#include "proc/startup.hpp"

#include "buffer/buffer.hpp"
#include "fs/initrd.hpp"
#include "handoff/memory_layout.h"
#include "ipc/channel.hpp"
#include "mm/kmem.hpp"
#include "mm/user_copy.hpp"
#include "proc/user_program.hpp"
#include "util/fixed_string.hpp"
#include "util/memory.h"
#include "vfs/initrd.hpp"
#include "vfs/tmpfs.hpp"

long validate_startup(const os1_startup_record& record)
{
    if(record.version != OS1_STARTUP_VERSION || record.size != sizeof(record) || record.reserved ||
       record.argc > OS1_ARGUMENT_LIMIT || record.envc > OS1_ENVIRONMENT_LIMIT ||
       record.string_bytes > sizeof(record.strings) || record.root_fd < -1 || record.cwd_fd < -1 ||
       record.root_fd >= static_cast<int>(OS1_DESCRIPTOR_LIMIT) ||
       record.cwd_fd >= static_cast<int>(OS1_DESCRIPTOR_LIMIT))
        return OS1_ERR_INVALID;
    const auto& args = record.constructor;
    if(args.size > OS1_CONSTRUCTOR_BYTES || ((args.schema_id == 0) != (args.version == 0)) ||
       (!args.schema_id && args.size))
        return OS1_ERR_INVALID;
    for(uint32_t i = args.size; i < OS1_CONSTRUCTOR_BYTES; ++i)
        if(args.payload[i])
            return OS1_ERR_INVALID;
    uint32_t cursor = 0;
    for(unsigned group = 0; group < 2; ++group)
    {
        const uint32_t count = group ? record.envc : record.argc;
        const auto* offsets = group ? record.envp : record.argv;
        for(uint32_t i = 0; i < OS1_ARGUMENT_LIMIT; ++i)
        {
            if(i >= count)
            {
                if(offsets[i])
                    return OS1_ERR_INVALID;
                continue;
            }
            if(offsets[i] != cursor || cursor >= record.string_bytes)
                return OS1_ERR_INVALID;
            while(cursor < record.string_bytes && record.strings[cursor])
                ++cursor;
            if(cursor == record.string_bytes)
                return OS1_ERR_INVALID;
            ++cursor;
        }
    }
    if(cursor != record.string_bytes)
        return OS1_ERR_INVALID;
    for(; cursor < sizeof(record.strings); ++cursor)
        if(record.strings[cursor])
            return OS1_ERR_INVALID;
    return 0;
}
long validate_namespace_roles(DescriptorTable& table, int root, int cwd, bool exec)
{
    const int roles[] = {root, cwd};
    for(int fd : roles)
    {
        if(fd == -1)
            continue;
        DescriptorEntry entry{};
        long result = descriptor_acquire_entry(table, fd, 0, entry);
        if(result < 0)
            return result;
        InstanceRef pin(entry.instance);
        if(entry.instance->object->kind != OS1_OBJECT_DIRECTORY)
            return OS1_ERR_NOT_DIRECTORY;
        if(exec && (entry.flags & OS1_DESCRIPTOR_CLOEXEC))
            return OS1_ERR_INVALID;
    }
    return 0;
}
long install_child_grants(DescriptorTable& parent,
                          DescriptorTable& child,
                          const os1_launch_request& request)
{
    if(request.grant_count > OS1_CHILD_GRANT_LIMIT)
        return OS1_ERR_INVALID;
    DescriptorEntry pins[OS1_CHILD_GRANT_LIMIT]{};
    uint64_t targets = 0;
    long result = 0;
    for(uint32_t i = 0; i < request.grant_count; ++i)
    {
        const auto& grant = request.grants[i];
        if(grant.reserved || grant.target < 0 ||
           grant.target >= static_cast<int>(OS1_DESCRIPTOR_LIMIT) ||
           ((grant.rights | grant.descendant_rights) & ~OS1_RIGHT_ALL) ||
           (grant.descriptor_flags & ~OS1_DESCRIPTOR_CLOEXEC) || (targets & (1ull << grant.target)))
        {
            result = OS1_ERR_INVALID;
            break;
        }
        targets |= 1ull << grant.target;
        result = descriptor_acquire_entry(parent, grant.source, OS1_RIGHT_DELEGATE, pins[i]);
        if(result < 0)
            break;
        if((grant.rights & ~pins[i].rights) ||
           (grant.descendant_rights & ~pins[i].descendant_rights))
        {
            result = OS1_ERR_ACCESS;
            break;
        }
    }
    if(result >= 0)
    {
        for(uint32_t i = 0; i < request.grant_count; ++i)
        {
            const auto& grant = request.grants[i];
            result = descriptor_install(child,
                                        pins[i].instance,
                                        grant.rights,
                                        grant.descriptor_flags,
                                        grant.target,
                                        grant.descendant_rights);
            if(result < 0)
                break;
        }
    }
    for(auto& entry : pins)
        instance_release(entry.instance);
    if(result >= 0)
        result =
            validate_namespace_roles(child, request.startup.root_fd, request.startup.cwd_fd, false);
    if(result < 0)
        descriptor_close_all(child);
    return result;
}
long inherit_process_namespace(Process* parent, Process* child)
{
    if(!parent)
    {
        auto* root = initrd_namespace_root();
        if(!root)
            return OS1_ERR_BAD_IMAGE;
        const int roles[] = {OS1_BOOT_ROOT_FD, OS1_BOOT_CWD_FD};
        for(int fd : roles)
        {
            InstanceRef instance(open_instance(root));
            if(!instance.get())
                return OS1_ERR_NO_MEMORY;
            const long status = descriptor_install(
                child->descriptors, instance.get(), kDirectoryRights, 0, fd, kNamespaceCeiling);
            if(status < 0)
                return status;
        }
        child->root_fd = OS1_BOOT_ROOT_FD;
        child->cwd_fd = OS1_BOOT_CWD_FD;
        return 0;
    }
    const int roles[] = {parent->root_fd, parent->cwd_fd};
    for(int fd : roles)
    {
        if(fd == -1)
            continue;
        DescriptorEntry entry{};
        long status = descriptor_acquire_entry(parent->descriptors, fd, OS1_RIGHT_DELEGATE, entry);
        if(status < 0)
            return status;
        InstanceRef pin(entry.instance);
        if(entry.instance->object->kind != OS1_OBJECT_DIRECTORY)
            return OS1_ERR_NOT_DIRECTORY;
        status = descriptor_install(
            child->descriptors, entry.instance, entry.rights, 0, fd, entry.descendant_rights);
        if(status < 0)
            return status;
    }
    child->root_fd = parent->root_fd;
    child->cwd_fd = parent->cwd_fd;
    return 0;
}
long resolve_process_image(Process* process, const char* path, OpenInstance*& image)
{
    image = nullptr;
    size_t size = 0;
    while(size <= OS1_PATH_MAX && path[size])
        ++size;
    if(!size || size > OS1_PATH_MAX)
        return OS1_ERR_INVALID;
    const bool absolute = path[0] == '/';
    if(absolute)
    {
        while(size && *path == '/')
        {
            ++path;
            --size;
        }
    }
    if(!size)
    {
        path = ".";
        size = 1;
    }
    if(process)
        return vfs_open(process->descriptors,
                        absolute ? process->root_fd : process->cwd_fd,
                        path,
                        size,
                        OS1_RIGHT_EXECUTE,
                        OS1_DESCRIPTOR_CLOEXEC,
                        0,
                        image);
    // Only trusted bootstrap may use the mount anchor directly.
    NativeObject* object = nullptr;
    long result = vfs_resolve(initrd_namespace_root(), path, size, object);
    if(result < 0)
        return result;
    if(object->kind != OS1_OBJECT_FILE)
        result = OS1_ERR_UNSUPPORTED;
    else
    {
        image = open_instance(object);
        if(!image)
            result = OS1_ERR_NO_MEMORY;
    }
    object_release(object);
    return result;
}
bool install_startup_record(PageFrameContainer& frames,
                            uint64_t cr3,
                            const os1_startup_record& record,
                            uint64_t& rsp,
                            uint64_t& argument)
{
    argument = (kUserStackTop - sizeof(record)) & ~uint64_t{15};
    VirtualMemory vm(frames, cr3);
    if(!copy_into_address_space(
           vm, argument, reinterpret_cast<const uint8_t*>(&record), sizeof(record)))
        return false;
    rsp = argument - 8;  // SysV entry alignment; payload lies above the active stack.
    return true;
}
long replace_process_image(const ProcessSyscallContext& context,
                           Thread* thread,
                           OpenInstance* image,
                           const os1_startup_record& startup,
                           const char* name)
{
    long result = validate_startup(startup);
    if(result < 0)
        return result;
    result = validate_namespace_roles(
        thread->process->descriptors, startup.root_fd, startup.cwd_fd, true);
    if(result < 0)
        return result;
    uint64_t cr3 = 0, entry = 0, rsp = 0, argument = 0;
    result =
        load_user_object_image(*context.frames, context.kernel_root_cr3, image, cr3, entry, rsp);
    if(result < 0)
        return result;
    if(!install_startup_record(*context.frames, cr3, startup, rsp, argument))
    {
        destroy_user_address_space(*context.frames, cr3);
        return OS1_ERR_FAULT;
    }
    const uint64_t old = thread->address_space_cr3;
    buffer_process_exec(*context.frames, thread->process);
    descriptor_close_exec(thread->process->descriptors);
    thread->address_space_cr3 = cr3;
    thread->process->address_space.cr3 = cr3;
    thread->process->root_fd = startup.root_fd;
    thread->process->cwd_fd = startup.cwd_fd;
    thread->process->state = ProcessState::Running;
    copy_fixed_string(thread->process->name, sizeof(thread->process->name), name);
    prepare_user_thread_entry(thread, entry, rsp);
    thread->frame.rdi = argument;
    context.write_cr3(cr3);
    if(old && old != cr3)
        destroy_user_address_space(*context.frames, old);
    return 0;
}
long sys_launch(const ProcessSyscallContext& context,
                uint64_t executable,
                uint64_t mode,
                uint64_t pointer)
{
    Thread* thread = current_thread();
    if(!thread || !thread->process || !thread->user_mode || !context.frames ||
       (mode == OS1_LAUNCH_EXEC && !context.write_cr3))
        return OS1_ERR_INVALID;
    if(executable >= OS1_DESCRIPTOR_LIMIT || mode > OS1_LAUNCH_PRIVATE_PROVIDER)
        return OS1_ERR_INVALID;
    auto* request = static_cast<os1_launch_request*>(kmalloc(sizeof(os1_launch_request)));
    if(!request)
        return OS1_ERR_NO_MEMORY;
    struct RequestGuard
    {
        void* pointer;
        ~RequestGuard()
        {
            kfree(pointer);
        }
    } guard{request};
    if(!copy_from_user(*context.frames, thread, pointer, request, sizeof(*request)))
        return OS1_ERR_FAULT;
    if(request->version != OS1_STARTUP_VERSION || request->size != sizeof(*request) ||
       request->reserved || request->grant_count > OS1_CHILD_GRANT_LIMIT ||
       (mode == OS1_LAUNCH_EXEC && request->grant_count))
        return OS1_ERR_INVALID;
    for(uint32_t i = request->grant_count; i < OS1_CHILD_GRANT_LIMIT; ++i)
    {
        const auto& g = request->grants[i];
        if(g.source || g.target || g.rights || g.descriptor_flags || g.descendant_rights ||
           g.reserved)
            return OS1_ERR_INVALID;
    }
    if(mode == OS1_LAUNCH_PRIVATE_PROVIDER &&
       ((request->grant_count != 1 && request->grant_count != 2) ||
        request->grants[0].target != 3 ||
        request->grants[0].rights != (OS1_RIGHT_READ | OS1_RIGHT_WRITE) ||
        request->grants[0].descriptor_flags || request->grants[0].descendant_rights ||
        request->startup.root_fd != -1 || request->startup.cwd_fd != -1))
        return OS1_ERR_INVALID;
    if(mode == OS1_LAUNCH_PRIVATE_PROVIDER && request->grant_count == 2)
    {
        const auto& g = request->grants[1];
        if(g.target != 4 ||
           g.rights !=
               (OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE) ||
           g.descriptor_flags || g.descendant_rights)
            return OS1_ERR_INVALID;
    }
    long result = validate_startup(request->startup);
    if(result < 0)
        return result;
    OpenInstance* image = nullptr;
    result = descriptor_acquire(thread->process->descriptors, executable, OS1_RIGHT_EXECUTE, image);
    if(result < 0)
        return result;
    InstanceRef image_pin(image);
    if(image->object->kind != OS1_OBJECT_FILE)
        return OS1_ERR_UNSUPPORTED;
    const char* name = request->startup.argc ? request->startup.strings : "native-image";
    if(mode == OS1_LAUNCH_EXEC)
        return replace_process_image(context, thread, image, request->startup, name);
    Process* child = create_user_process(name, 0);
    if(!child)
        return OS1_ERR_NO_MEMORY;
    child->parent = thread->process;
    result = install_child_grants(thread->process->descriptors, child->descriptors, *request);
    if(result < 0)
    {
        reap_process(child, *context.frames);
        return result;
    }
    if(mode == OS1_LAUNCH_PRIVATE_PROVIDER)
    {
        if(child->descriptors.entries[3].instance->object->kind != OS1_OBJECT_IPC_PROVIDER ||
           (request->grant_count == 2 &&
            !ipc_matching_endpoints(child->descriptors.entries[3].instance->object,
                                    child->descriptors.entries[4].instance->object)))
        {
            reap_process(child, *context.frames);
            return OS1_ERR_UNSUPPORTED;
        }
        child->private_provider = true;
    }
    uint64_t cr3 = 0, entry = 0, rsp = 0, argument = 0;
    result =
        load_user_object_image(*context.frames, context.kernel_root_cr3, image, cr3, entry, rsp);
    if(result < 0)
    {
        reap_process(child, *context.frames);
        return result;
    }
    child->address_space.cr3 = cr3;
    if(!install_startup_record(*context.frames, cr3, request->startup, rsp, argument))
    {
        reap_process(child, *context.frames);
        return OS1_ERR_FAULT;
    }
    child->root_fd = request->startup.root_fd;
    child->cwd_fd = request->startup.cwd_fd;
    Thread* worker = create_user_thread(child, entry, rsp, *context.frames, false);
    if(!worker)
    {
        reap_process(child, *context.frames);
        return OS1_ERR_NO_MEMORY;
    }
    worker->frame.rdi = argument;
    const long pid = child->pid;
    mark_thread_ready(worker, worker->scheduler_cpu);
    return pid;
}
