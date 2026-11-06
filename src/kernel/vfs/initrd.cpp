#include "vfs/initrd.hpp"

#include "fs/cpio_newc.hpp"
#include "mm/kmem.hpp"
#include "util/memory.h"
#include "vfs/filesystem.hpp"

namespace
{
struct Import
{
    VfsNode* root;
    uint32_t nodes;
    long error;
};
void destroy_node(NativeObject* object)
{
    auto* node = static_cast<VfsNode*>(object);
    auto* child = node->children;
    while(child)
    {
        auto* next = child->next;
        object_release(child);
        child = next;
    }
    kfree(node);
}
VfsNode* allocate_node(Import& import, const char* name, size_t length, bool directory)
{
    if(import.nodes == 2048)
    {
        import.error = OS1_ERR_LIMIT;
        return nullptr;
    }
    auto* node = static_cast<VfsNode*>(kmalloc(sizeof(VfsNode), KmallocFlags::Zero));
    if(!node)
    {
        import.error = OS1_ERR_NO_MEMORY;
        return nullptr;
    }
    ++import.nodes;
    node->references = 1;
    node->kind = directory ? OS1_OBJECT_DIRECTORY : OS1_OBJECT_FILE;
    node->interfaces =
        directory ? OS1_INTERFACE_DIRECTORY : OS1_INTERFACE_BYTE_INPUT | OS1_INTERFACE_SEEK;
    node->allowed_rights = directory ? kDirectoryRights : kFileRights;
    node->destroy = destroy_node;
    node->name_size = length;
    if(length)
        memcpy(node->name, name, length);
    return node;
}
VfsNode* lookup(VfsNode* directory, const char* name, size_t size)
{
    for(auto* child = directory->children; child; child = child->next)
    {
        if(child->name_size != size)
            continue;
        size_t i = 0;
        while(i < size && child->name[i] == name[i])
            ++i;
        if(i == size)
            return child;
    }
    return nullptr;
}
long validate_path(const char* path, size_t size)
{
    if(!path || !size || size > OS1_PATH_MAX || path[0] == '/')
        return OS1_ERR_INVALID;
    size_t start = 0;
    for(size_t i = 0; i <= size; ++i)
    {
        if(i < size && !path[i])
            return OS1_ERR_INVALID;
        if(i < size && path[i] != '/')
            continue;
        const size_t length = i - start;
        if(length > OS1_NAME_MAX || (length == 2 && path[start] == '.' && path[start + 1] == '.'))
            return OS1_ERR_INVALID;
        start = i + 1;
    }
    return 0;
}
bool import_entry(const char* path,
                  size_t size,
                  uint32_t kind,
                  const uint8_t* bytes,
                  uint64_t length,
                  void* context)
{
    auto& import = *static_cast<Import*>(context);
    if(validate_path(path, size) < 0)
        return false;
    auto* node = import.root;
    size_t start = 0;
    unsigned depth = 0;
    for(size_t i = 0; i <= size; ++i)
    {
        if(i < size && path[i] != '/')
            continue;
        const size_t count = i - start;
        if(count && !(count == 1 && path[start] == '.'))
        {
            if(node->kind != OS1_OBJECT_DIRECTORY)
                return false;
            if(++depth > 64)
            {
                import.error = OS1_ERR_LIMIT;
                return false;
            }
            bool more = false;
            for(size_t j = i; j < size; ++j)
                if(path[j] != '/')
                {
                    more = true;
                    break;
                }
            auto* child = lookup(node, path + start, count);
            if(!child)
            {
                child = allocate_node(import, path + start, count, more || kind == 0040000u);
                if(!child)
                    return false;
                child->next = node->children;
                node->children = child;
            }
            node = child;
        }
        else if(count && node->kind != OS1_OBJECT_DIRECTORY)
            return false;
        start = i + 1;
    }
    const bool directory = kind == 0040000u;
    if(node->explicit_entry || node->kind != (directory ? OS1_OBJECT_DIRECTORY : OS1_OBJECT_FILE) ||
       (!directory && path[size - 1] == '/'))
        return false;
    node->explicit_entry = true;
    node->bytes = bytes;
    node->byte_size = length;
    return true;
}
}  // namespace
long vfs_import_initrd(const uint8_t* bytes, uint64_t size, NativeObject*& root)
{
    root = nullptr;
    if(size > 64ull * 1024 * 1024)
        return OS1_ERR_LIMIT;
    Import import{nullptr, 0, OS1_ERR_BAD_IMAGE};
    import.root = allocate_node(import, nullptr, 0, true);
    if(!import.root)
        return import.error;
    if(!for_each_cpio_newc_entry(bytes, size, import_entry, &import))
    {
        object_release(import.root);
        return import.error;
    }
    root = import.root;
    return 0;
}
long vfs_resolve(NativeObject* directory, const char* path, size_t size, NativeObject*& result)
{
    result = nullptr;
    const long validation = validate_path(path, size);
    if(validation < 0)
        return validation;
    if(!directory || directory->kind != OS1_OBJECT_DIRECTORY)
        return OS1_ERR_NOT_DIRECTORY;
    object_retain(directory);
    NativeObject* current = directory;
    size_t start = 0;
    long status = 0;
    for(size_t i = 0; i <= size; ++i)
    {
        if(i < size && path[i] != '/')
            continue;
        const size_t count = i - start;
        if(count)
        {
            if(current->kind != OS1_OBJECT_DIRECTORY)
            {
                status = OS1_ERR_NOT_DIRECTORY;
                break;
            }
            if(!(count == 1 && path[start] == '.'))
            {
                NativeObject* child = nullptr;
                auto* node = static_cast<VfsNode*>(current);
                if(node->operations)
                    status = node->operations->lookup(node, path + start, count, child);
                else
                {
                    child = lookup(node, path + start, count);
                    if(child)
                        object_retain(child);
                    else
                        status = OS1_ERR_NOT_FOUND;
                }
                if(status < 0)
                    break;
                object_release(current);
                current = child;
            }
        }
        start = i + 1;
    }
    if(status == 0 && path[size - 1] == '/' && current->kind != OS1_OBJECT_DIRECTORY)
        status = OS1_ERR_NOT_DIRECTORY;
    if(status < 0)
        object_release(current);
    else
        result = current;
    return status;
}
long vfs_open(DescriptorTable& table,
              int directory,
              const char* path,
              size_t size,
              uint32_t rights,
              uint32_t flags,
              uint32_t ceiling,
              OpenInstance*& result)
{
    result = nullptr;
    if((rights | ceiling) & ~OS1_RIGHT_ALL || flags & ~OS1_DESCRIPTOR_CLOEXEC)
        return OS1_ERR_INVALID;
    DescriptorEntry source{};
    long status = descriptor_acquire_entry(table, directory, OS1_RIGHT_LOOKUP, source);
    if(status < 0)
        return status;
    InstanceRef pin(source.instance);
    const uint32_t allowed =
        source.descendant_rights &
        ((source.rights & OS1_RIGHT_DELEGATE) ? OS1_RIGHT_ALL : ~OS1_RIGHT_DELEGATE);
    if((rights | ceiling) & ~allowed)
        return OS1_ERR_ACCESS;
    NativeObject* object = nullptr;
    status = vfs_resolve(source.instance->object, path, size, object);
    if(status < 0)
        return status;
    if(rights & ~object->allowed_rights)
        status = OS1_ERR_ACCESS;
    else if(ceiling && object->kind != OS1_OBJECT_DIRECTORY)
        status = OS1_ERR_INVALID;
    else
    {
        result = open_instance(object, &status);
    }
    object_release(object);
    return status;
}
long vfs_open_descriptor(DescriptorTable& table,
                         int directory,
                         const char* path,
                         size_t size,
                         uint32_t rights,
                         uint32_t flags,
                         uint32_t ceiling)
{
    OpenInstance* instance = nullptr;
    const long status = vfs_open(table, directory, path, size, rights, flags, ceiling, instance);
    if(status < 0)
        return status;
    InstanceRef pin(instance);
    return descriptor_install(table, instance, rights, flags, -1, ceiling);
}
