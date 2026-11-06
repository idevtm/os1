#include "resource/descriptor.hpp"

#include "console/console.hpp"
#include "console/console_input.hpp"
#include "mm/kmem.hpp"
#include "sync/atomic.hpp"
#include "util/memory.h"
#include "vfs/filesystem.hpp"

namespace
{
bool valid_fd(int fd)
{
    return fd >= 0 && static_cast<uint32_t>(fd) < OS1_DESCRIPTOR_LIMIT;
}
bool valid_grant(OpenInstance* instance, uint32_t rights, uint32_t flags)
{
    return instance && !(rights & ~instance->object->allowed_rights) &&
           !(flags & ~OS1_DESCRIPTOR_CLOEXEC);
}
NativeObject console_input{1,
                           OS1_OBJECT_CONSOLE,
                           OS1_INTERFACE_BYTE_INPUT,
                           OS1_RIGHT_READ | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE,
                           nullptr,
                           console_input_reserve,
                           console_input_finish,
                           nullptr};
NativeObject console_output{1,
                            OS1_OBJECT_CONSOLE,
                            OS1_INTERFACE_BYTE_OUTPUT,
                            OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE,
                            nullptr,
                            nullptr,
                            nullptr,
                            write_console_bytes};
void close_selected(DescriptorTable& table, bool all)
{
    OpenInstance* released[OS1_DESCRIPTOR_LIMIT]{};
    {
        IrqSpinGuard guard(table.lock);
        if(all)
            table.closed = true;
        for(uint32_t i = 0; i < OS1_DESCRIPTOR_LIMIT; ++i)
        {
            if(all || (table.entries[i].flags & OS1_DESCRIPTOR_CLOEXEC))
            {
                released[i] = table.entries[i].instance;
                table.entries[i] = {};
            }
        }
    }
    for(auto* instance : released)
        instance_release(instance);
}
}  // namespace
void object_retain(NativeObject* object)
{
    (void)atomic_fetch_add(&object->references, 1u);
}
void object_release(NativeObject* object)
{
    if(object && __atomic_fetch_sub(&object->references, 1u, __ATOMIC_ACQ_REL) == 1u &&
       object->destroy)
        object->destroy(object);
}
OpenInstance* open_instance(NativeObject* object, long* error)
{
    if(error)
        *error = OS1_ERR_NO_MEMORY;
    if(!object)
        return nullptr;
    if(object->acquire_open)
    {
        long status = object->acquire_open(object);
        if(status < 0)
        {
            if(error)
                *error = status;
            return nullptr;
        }
    }
    auto* instance = static_cast<OpenInstance*>(kmalloc(sizeof(OpenInstance), KmallocFlags::Zero));
    if(instance)
    {
        instance->references = 1;
        instance->object = object;
        object_retain(object);
        if(error)
            *error = 0;
    }
    else if(object->release_open)
        object->release_open(object);
    return instance;
}
void instance_retain(OpenInstance* instance)
{
    (void)atomic_fetch_add(&instance->references, 1u);
}
void instance_release(OpenInstance* instance)
{
    if(instance && __atomic_fetch_sub(&instance->references, 1u, __ATOMIC_ACQ_REL) == 1u)
    {
        if(instance->object->release_open)
            instance->object->release_open(instance->object);
        object_release(instance->object);
        kfree(instance);
    }
}
void descriptor_table_initialize(DescriptorTable& table)
{
    table.lock.reset("descriptors");
    table.closed = false;
    memset(table.entries, 0, sizeof(table.entries));
}
long descriptor_install(DescriptorTable& table,
                        OpenInstance* instance,
                        uint32_t rights,
                        uint32_t flags,
                        int target,
                        uint32_t ceiling)
{
    if(!valid_grant(instance, rights, flags) || (target != -1 && !valid_fd(target)) ||
       (ceiling & ~OS1_RIGHT_ALL) || (ceiling && instance->object->kind != OS1_OBJECT_DIRECTORY))
        return OS1_ERR_INVALID;
    OpenInstance* replaced = nullptr;
    {
        IrqSpinGuard guard(table.lock);
        if(table.closed)
            return OS1_ERR_CLOSED;
        if(target == -1)
        {
            for(uint32_t i = 0; i < OS1_DESCRIPTOR_LIMIT; ++i)
                if(!table.entries[i].instance)
                {
                    target = static_cast<int>(i);
                    break;
                }
            if(target == -1)
                return OS1_ERR_LIMIT;
        }
        instance_retain(instance);
        replaced = table.entries[target].instance;
        table.entries[target] = {instance, rights, flags, ceiling};
    }
    instance_release(replaced);
    return target;
}
long descriptor_acquire(DescriptorTable& table, int fd, uint32_t rights, OpenInstance*& instance)
{
    instance = nullptr;
    IrqSpinGuard guard(table.lock);
    if(table.closed)
        return OS1_ERR_CLOSED;
    if(!valid_fd(fd) || !table.entries[fd].instance)
        return OS1_ERR_BAD_DESCRIPTOR;
    auto& entry = table.entries[fd];
    if(rights & ~entry.rights)
        return OS1_ERR_ACCESS;
    instance_retain(entry.instance);
    instance = entry.instance;
    return 0;
}
long descriptor_close(DescriptorTable& table, int fd)
{
    OpenInstance* instance = nullptr;
    {
        IrqSpinGuard guard(table.lock);
        if(!valid_fd(fd) || !table.entries[fd].instance)
            return OS1_ERR_BAD_DESCRIPTOR;
        instance = table.entries[fd].instance;
        table.entries[fd] = {};
    }
    instance_release(instance);
    return 0;
}
long descriptor_duplicate_scoped(DescriptorTable& table,
                                 int fd,
                                 const os1_scoped_duplicate_request& r)
{
    if(r.version != OS1_VFS_ABI_VERSION || r.size != sizeof(r) || r.reserved[0] || r.reserved[1] ||
       ((r.rights | r.descendant_rights) & ~OS1_RIGHT_ALL) ||
       (r.descriptor_flags & ~OS1_DESCRIPTOR_CLOEXEC) || (r.target != -1 && !valid_fd(r.target)))
        return OS1_ERR_INVALID;
    OpenInstance* replaced = nullptr;
    int target = r.target;
    {
        IrqSpinGuard guard(table.lock);
        if(table.closed)
            return OS1_ERR_CLOSED;
        if(!valid_fd(fd) || !table.entries[fd].instance)
            return OS1_ERR_BAD_DESCRIPTOR;
        const auto source = table.entries[fd];
        if(r.descendant_rights & ~source.descendant_rights)
            return OS1_ERR_ACCESS;
        if(!(source.rights & OS1_RIGHT_DELEGATE) || (r.rights & ~source.rights))
            return OS1_ERR_ACCESS;
        if(target == fd)
            return (r.rights == source.rights && r.descriptor_flags == source.flags &&
                    r.descendant_rights == source.descendant_rights)
                       ? fd
                       : OS1_ERR_INVALID;
        if(target == -1)
        {
            for(uint32_t i = 0; i < OS1_DESCRIPTOR_LIMIT; ++i)
                if(!table.entries[i].instance)
                {
                    target = static_cast<int>(i);
                    break;
                }
            if(target == -1)
                return OS1_ERR_LIMIT;
        }
        instance_retain(source.instance);
        replaced = table.entries[target].instance;
        table.entries[target] = {
            source.instance, r.rights, r.descriptor_flags, r.descendant_rights};
    }
    instance_release(replaced);
    return target;
}
long descriptor_duplicate(DescriptorTable& table, int fd, const os1_duplicate_request& r)
{
    if(r.version != OS1_RESOURCE_ABI_VERSION || r.size != sizeof(r) || r.reserved)
        return OS1_ERR_INVALID;
    // Legacy duplication carries no descendant factory authority.
    const os1_scoped_duplicate_request scoped{
        OS1_VFS_ABI_VERSION, sizeof(scoped), r.target, r.rights, r.descriptor_flags, 0, {0, 0}};
    return descriptor_duplicate_scoped(table, fd, scoped);
}
long descriptor_query(DescriptorTable& table, int fd, os1_resource_info& result)
{
    IrqSpinGuard guard(table.lock);
    if(table.closed)
        return OS1_ERR_CLOSED;
    if(!valid_fd(fd) || !table.entries[fd].instance)
        return OS1_ERR_BAD_DESCRIPTOR;
    const auto& entry = table.entries[fd];
    if(!(entry.rights & OS1_RIGHT_INSPECT))
        return OS1_ERR_ACCESS;
    result = {};
    result.version = OS1_RESOURCE_ABI_VERSION;
    result.size = sizeof(result);
    result.kind = entry.instance->object->kind;
    result.interfaces = entry.instance->object->interfaces;
    result.rights = entry.rights;
    result.descriptor_flags = entry.flags;
    result.status_flags = atomic_load_acquire(&entry.instance->status_flags);
    result.max_io_bytes = OS1_IO_MAX_BYTES;
    result.descriptor_limit = OS1_DESCRIPTOR_LIMIT;
    return 0;
}
long descriptor_set_flags(DescriptorTable& table, int fd, uint32_t flags)
{
    if(flags & ~OS1_DESCRIPTOR_CLOEXEC)
        return OS1_ERR_INVALID;
    IrqSpinGuard guard(table.lock);
    if(table.closed)
        return OS1_ERR_CLOSED;
    if(!valid_fd(fd) || !table.entries[fd].instance)
        return OS1_ERR_BAD_DESCRIPTOR;
    table.entries[fd].flags = flags;
    return 0;
}
long descriptor_set_status(DescriptorTable& table, int fd, uint32_t flags)
{
    if(flags & ~(OS1_STATUS_NONBLOCK | OS1_STATUS_APPEND))
        return OS1_ERR_INVALID;
    DescriptorEntry entry{};
    long status = descriptor_acquire_entry(table, fd, 0, entry);
    if(status < 0)
        return status;
    auto* instance = entry.instance;
    InstanceRef retained(instance);
    uint32_t required;
    if(instance->object->kind == OS1_OBJECT_CONSOLE &&
       (instance->object->interfaces & OS1_INTERFACE_BYTE_INPUT))
    {
        if(flags & ~OS1_STATUS_NONBLOCK)
            return OS1_ERR_UNSUPPORTED;
        required = OS1_RIGHT_READ;
    }
    else if(vfs_backed(instance->object) && instance->object->kind == OS1_OBJECT_FILE)
    {
        if(flags & ~OS1_STATUS_APPEND)
            return OS1_ERR_UNSUPPORTED;
        required = OS1_RIGHT_WRITE;
    }
    else
        return OS1_ERR_UNSUPPORTED;
    if(!(entry.rights & required))
        return OS1_ERR_ACCESS;
    atomic_store_release(&instance->status_flags, flags);
    return 0;
}

void descriptor_close_all(DescriptorTable& table)
{
    close_selected(table, true);
}
void descriptor_close_exec(DescriptorTable& table)
{
    close_selected(table, false);
}
long descriptor_inherit_standard(DescriptorTable& parent, DescriptorTable& child)
{
    DescriptorEntry grants[3]{};
    {
        IrqSpinGuard guard(parent.lock);
        if(parent.closed)
            return OS1_ERR_CLOSED;
        for(unsigned i = 0; i < 3; ++i)
            if(parent.entries[i].instance && !(parent.entries[i].rights & OS1_RIGHT_DELEGATE))
                return OS1_ERR_ACCESS;
        for(unsigned i = 0; i < 3; ++i)
        {
            grants[i] = parent.entries[i];
            if(grants[i].instance)
                instance_retain(grants[i].instance);
        }
    }
    long status = 0;
    for(unsigned i = 0; i < 3; ++i)
    {
        if(grants[i].instance && status >= 0)
            status = descriptor_install(child, grants[i].instance, grants[i].rights, 0, i);
        instance_release(grants[i].instance);
    }
    if(status < 0)
        descriptor_close_all(child);
    return status < 0 ? status : 0;
}
long descriptor_bootstrap_console(DescriptorTable& table)
{
    for(int i = 0; i < 3; ++i)
    {
        auto* object = i == 0 ? &console_input : &console_output;
        InstanceRef instance(open_instance(object));
        if(!instance.get())
        {
            descriptor_close_all(table);
            return OS1_ERR_NO_MEMORY;
        }
        const long result = descriptor_install(table, instance.get(), object->allowed_rights, 0, i);
        if(result < 0)
        {
            descriptor_close_all(table);
            return result;
        }
    }
    return 0;
}

long descriptor_acquire_entry(DescriptorTable& table,
                              int fd,
                              uint32_t rights,
                              DescriptorEntry& result)
{
    result = {};
    IrqSpinGuard guard(table.lock);
    if(table.closed)
        return OS1_ERR_CLOSED;
    if(!valid_fd(fd) || !table.entries[fd].instance)
        return OS1_ERR_BAD_DESCRIPTOR;
    if(rights & ~table.entries[fd].rights)
        return OS1_ERR_ACCESS;
    result = table.entries[fd];
    instance_retain(result.instance);
    return 0;
}
