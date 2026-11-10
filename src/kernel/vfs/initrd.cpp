#include "vfs/initrd.hpp"

#include "fs/cpio_newc.hpp"
#include "mm/kmem.hpp"
#include "sync/atomic.hpp"
#include "util/memory.h"
#include "vfs/filesystem.hpp"
#include "vfs/namespace.hpp"

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
    node->object_id = vfs_identity();
    node->filesystem_id = import.root ? import.root->filesystem_id : node->object_id;
    node->mount_root = !import.root;
    node->archive_bytes = import.root ? import.root->archive_bytes : 0;
    node->linked = true;
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
    import.root->archive_bytes = size;
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

namespace
{
volatile bool namespace_busy = false;
// Caller retains root and holds NamespaceGuard. No publication can change child lists.
bool ancestry(VfsNode* root, VfsNode* target, VfsNode** path, unsigned& depth)
{
    VfsNode* next[65]{};
    path[0] = root;
    next[0] = root->children;
    depth = 0;
    unsigned visited = 0;
    for(;;)
    {
        if(path[depth] == target)
            return true;
        if(++visited > 8192)
            return false;
        if(next[depth])
        {
            auto* node = next[depth];
            next[depth] = node->next;
            if(depth == 64)
                continue;
            path[++depth] = node;
            next[depth] = node->children;
        }
        else if(depth)
            --depth;
        else
            return false;
    }
}
}  // namespace
NamespaceGuard::NamespaceGuard()
{
    bool expected = false;
    held = atomic_compare_exchange_strong(&namespace_busy, &expected, true);
}
NamespaceGuard::~NamespaceGuard()
{
    if(held)
        atomic_store_release(&namespace_busy, false);
}
long vfs_describe_path(DescriptorTable& table, int root_fd, int fd, char* out, size_t capacity)
{
    DescriptorEntry root{}, object{};
    long r = descriptor_acquire_entry(table, root_fd, OS1_RIGHT_LOOKUP, root);
    if(r < 0)
        return r;
    InstanceRef root_pin(root.instance);
    r = descriptor_acquire_entry(table, fd, OS1_RIGHT_INSPECT, object);
    if(r < 0)
        return r;
    InstanceRef object_pin(object.instance);
    if(root.instance->object->kind != OS1_OBJECT_DIRECTORY)
        return OS1_ERR_NOT_DIRECTORY;
    if(object.instance->object->kind != OS1_OBJECT_DIRECTORY &&
       object.instance->object->kind != OS1_OBJECT_FILE)
        return OS1_ERR_UNSUPPORTED;
    NamespaceGuard guard;
    if(!guard.held)
        return OS1_ERR_WOULD_BLOCK;
    VfsNode* path[65]{};
    unsigned depth = 0;
    if(!ancestry(static_cast<VfsNode*>(root.instance->object),
                 static_cast<VfsNode*>(object.instance->object),
                 path,
                 depth))
        return OS1_ERR_NOT_FOUND;
    size_t size = 1;
    for(unsigned i = 1; i <= depth; ++i)
        size += path[i]->name_size + (i > 1);
    if(size >= capacity)
        return OS1_ERR_BUFFER_TOO_SMALL;
    size_t at = 0;
    out[at++] = '/';
    for(unsigned i = 1; i <= depth; ++i)
    {
        if(i > 1)
            out[at++] = '/';
        memcpy(out + at, path[i]->name, path[i]->name_size);
        at += path[i]->name_size;
    }
    out[at] = 0;
    return at;
}
long vfs_context_open(DescriptorTable& table,
                      int root_fd,
                      const os1_context_open_request& request,
                      const char* text)
{
    if(!request.path_bytes || request.path_bytes > OS1_PATH_MAX ||
       ((request.rights | request.descendant_rights) & ~OS1_RIGHT_ALL) ||
       request.descriptor_flags & ~OS1_DESCRIPTOR_CLOEXEC)
        return OS1_ERR_INVALID;
    DescriptorEntry root{}, cwd{};
    long r = descriptor_acquire_entry(table, root_fd, OS1_RIGHT_LOOKUP, root);
    if(r < 0)
        return r;
    InstanceRef root_pin(root.instance);
    const bool absolute = text[0] == '/';
    r = descriptor_acquire_entry(table, absolute ? root_fd : request.cwd_fd, OS1_RIGHT_LOOKUP, cwd);
    if(r < 0)
        return r;
    InstanceRef cwd_pin(cwd.instance);
    if(root.instance->object->kind != OS1_OBJECT_DIRECTORY ||
       cwd.instance->object->kind != OS1_OBJECT_DIRECTORY)
        return OS1_ERR_NOT_DIRECTORY;
    const uint32_t allowed =
        root.descendant_rights & cwd.descendant_rights &
        (((root.rights & cwd.rights) & OS1_RIGHT_DELEGATE) ? OS1_RIGHT_ALL : ~OS1_RIGHT_DELEGATE);
    if((request.rights | request.descendant_rights) & ~allowed)
        return OS1_ERR_ACCESS;
    NamespaceGuard guard;
    if(!guard.held)
        return OS1_ERR_WOULD_BLOCK;
    VfsNode* path[65]{};
    unsigned depth = 0;
    if(!ancestry(static_cast<VfsNode*>(root.instance->object),
                 static_cast<VfsNode*>(cwd.instance->object),
                 path,
                 depth))
        return OS1_ERR_NOT_FOUND;
    size_t start = 0;
    for(size_t i = 0; i <= request.path_bytes; ++i)
    {
        if(i < request.path_bytes && !text[i])
            return OS1_ERR_INVALID;
        if(i < request.path_bytes && text[i] != '/')
            continue;
        const size_t n = i - start;
        if(n)
        {
            if(path[depth]->kind != OS1_OBJECT_DIRECTORY)
                return OS1_ERR_NOT_DIRECTORY;
            if(n > OS1_NAME_MAX)
                return OS1_ERR_INVALID;
            if(n == 2 && text[start] == '.' && text[start + 1] == '.')
            {
                if(depth)
                    --depth;
            }
            else if(!(n == 1 && text[start] == '.'))
            {
                auto* child = lookup(path[depth], text + start, n);
                if(!child)
                    return OS1_ERR_NOT_FOUND;
                if(depth == 64)
                    return OS1_ERR_LIMIT;
                path[++depth] = child;
            }
        }
        start = i + 1;
    }
    auto* node = path[depth];
    if(text[request.path_bytes - 1] == '/' && node->kind != OS1_OBJECT_DIRECTORY)
        return OS1_ERR_NOT_DIRECTORY;
    if(request.rights & ~node->allowed_rights)
        return OS1_ERR_ACCESS;
    if(request.descendant_rights && node->kind != OS1_OBJECT_DIRECTORY)
        return OS1_ERR_INVALID;
    InstanceRef instance(open_instance(node, &r));
    if(!instance.get())
        return r;
    return descriptor_install(table,
                              instance.get(),
                              request.rights,
                              request.descriptor_flags,
                              -1,
                              request.descendant_rights);
}
long vfs_move_at(DescriptorTable& table, int source_fd, const os1_move_request& request)
{
    DescriptorEntry source{}, destination{};
    long r =
        descriptor_acquire_entry(table, source_fd, OS1_RIGHT_LOOKUP | OS1_RIGHT_RENAME, source);
    if(r < 0)
        return r;
    InstanceRef s(source.instance);
    const uint32_t rights = OS1_RIGHT_LOOKUP | OS1_RIGHT_CREATE | OS1_RIGHT_RENAME |
                            (request.replace ? OS1_RIGHT_REMOVE : 0);
    r = descriptor_acquire_entry(table, request.destination_fd, rights, destination);
    if(r < 0)
        return r;
    InstanceRef d(destination.instance);
    if(s.get()->object->kind != OS1_OBJECT_DIRECTORY ||
       d.get()->object->kind != OS1_OBJECT_DIRECTORY)
        return OS1_ERR_NOT_DIRECTORY;
    auto* from = static_cast<VfsNode*>(s.get()->object);
    auto* to = static_cast<VfsNode*>(d.get()->object);
    if(!from->operations || !from->operations->move || from->operations != to->operations)
        return OS1_ERR_UNSUPPORTED;
    return from->operations->move(from,
                                  request.source,
                                  request.source_bytes,
                                  to,
                                  request.destination,
                                  request.destination_bytes,
                                  request.replace);
}
long vfs_same_object(DescriptorTable& table, int left, int right)
{
    DescriptorEntry a{}, b{};
    long r = descriptor_acquire_entry(table, left, OS1_RIGHT_INSPECT, a);
    if(r < 0)
        return r;
    InstanceRef pa(a.instance);
    r = descriptor_acquire_entry(table, right, OS1_RIGHT_INSPECT, b);
    if(r < 0)
        return r;
    InstanceRef pb(b.instance);
    return a.instance->object == b.instance->object;
}

namespace
{
volatile uint64_t identity_sequence = 0;
uint64_t namespace_generation = 1;
}  // namespace
uint64_t vfs_identity()
{
    return __atomic_add_fetch(&identity_sequence, 1ull, __ATOMIC_RELAXED);
}
uint64_t vfs_generation()
{
    return namespace_generation;
}
void vfs_changed(VfsNode* a, VfsNode* b, VfsNode* c, VfsNode* d)
{
    ++namespace_generation;
    VfsNode* nodes[] = {a, b, c, d};
    for(auto* node : nodes)
        if(node)
            node->namespace_generation = namespace_generation;
}
static uint64_t tree_generation(VfsNode* node, bool scoped)
{
    return scoped ? (node->namespace_generation ? node->namespace_generation : 1)
                  : vfs_generation();
}
long vfs_metadata(VfsNode* node, os1_metadata& result)
{
    result = {OS1_VFS_ABI_VERSION,
              sizeof(result),
              node->kind,
              OS1_METADATA_LOGICAL,
              OS1_FILESYSTEM_INITRD,
              node->mount_root ? OS1_METADATA_MOUNT : 0u,
              node->filesystem_id,
              node->object_id,
              node->operations ? 0 : node->byte_size,
              0,
              vfs_generation()};
    return node->operations ? node->operations->metadata(node, result) : 0;
}
long vfs_space(VfsNode* node, os1_space& result)
{
    result = {OS1_VFS_ABI_VERSION,
              sizeof(result),
              OS1_FILESYSTEM_INITRD,
              1,
              node->filesystem_id,
              node->archive_bytes,
              0,
              0};
    return node->operations ? node->operations->space(node, result) : 0;
}

long vfs_tree_step(DescriptorTable& table, int fd, os1_tree_step& request)
{
    const bool scoped = request.version == OS1_TREE_SCOPED_VERSION;
    if((request.version != OS1_VFS_ABI_VERSION && !scoped) || request.size != sizeof(request) ||
       request.action < OS1_TREE_NEXT || request.action > OS1_TREE_CHECK ||
       request.name_bytes > OS1_NAME_MAX || request.excluded_fd < -1 ||
       ((request.rights | request.descendant_rights) & ~OS1_RIGHT_ALL))
        return OS1_ERR_INVALID;
    const bool named = request.action != OS1_TREE_NEXT && request.action != OS1_TREE_CHECK;
    if(named)
    {
        if(!request.name_bytes || (request.name_bytes == 1 && request.name[0] == '.') ||
           (request.name_bytes == 2 && request.name[0] == '.' && request.name[1] == '.'))
            return OS1_ERR_INVALID;
        for(unsigned i = 0; i < sizeof(request.name); ++i)
            if(i < request.name_bytes ? (!request.name[i] || request.name[i] == '/')
                                      : request.name[i])
                return OS1_ERR_INVALID;
    }
    uint32_t required = OS1_RIGHT_INSPECT;
    if(request.action == OS1_TREE_NEXT)
        required |= OS1_RIGHT_ENUMERATE | OS1_RIGHT_LOOKUP;
    if(named)
        required |= OS1_RIGHT_LOOKUP;
    if(request.action == OS1_TREE_CREATE)
        required |= OS1_RIGHT_CREATE;
    if(request.action == OS1_TREE_REMOVE)
        required |= OS1_RIGHT_REMOVE;
    DescriptorEntry parent{};
    long status = descriptor_acquire_entry(table, fd, required, parent);
    if(status < 0)
        return status;
    InstanceRef pin(parent.instance);
    auto* object = parent.instance->object;
    if(object->kind != OS1_OBJECT_DIRECTORY &&
       !(request.action == OS1_TREE_CHECK && object->kind == OS1_OBJECT_FILE))
        return OS1_ERR_NOT_DIRECTORY;
    auto* node = static_cast<VfsNode*>(object);
    OpenInstance* excluded = nullptr;
    if(request.excluded_fd >= 0)
    {
        status = descriptor_acquire(table, request.excluded_fd, OS1_RIGHT_INSPECT, excluded);
        if(status < 0)
            return status;
    }
    InstanceRef exclusion(excluded);
    if(excluded && excluded->object->kind != OS1_OBJECT_DIRECTORY &&
       excluded->object->kind != OS1_OBJECT_FILE)
        return OS1_ERR_INVALID;
    NamespaceGuard guard;
    if(!guard.held)
        return OS1_ERR_WOULD_BLOCK;
    if(request.generation != tree_generation(node, scoped) &&
       !(request.action == OS1_TREE_CHECK && !request.generation))
        return OS1_ERR_CHANGED;
    if(!node->linked)
        return OS1_ERR_CLOSED;
    if(request.filesystem_id && request.filesystem_id != node->filesystem_id)
        return OS1_ERR_ACCESS;
    if(excluded && vfs_contains(static_cast<VfsNode*>(excluded->object), node))
        return OS1_ERR_INVALID;
    if(request.action == OS1_TREE_CHECK)
    {
        status = vfs_metadata(node, request.metadata);
        if(scoped)
            request.metadata.generation = tree_generation(node, true);
        request.generation = tree_generation(node, scoped);
        return status;
    }
    VfsNode* child = nullptr;
    if(request.action == OS1_TREE_NEXT)
    {
        if(!(parent.descendant_rights & OS1_RIGHT_INSPECT))
            return OS1_ERR_ACCESS;
        for(auto* n = node->children; n; n = n->next)
            if(n->filesystem_id == node->filesystem_id && n->object_id > request.cursor &&
               (!child || n->object_id < child->object_id))
                child = n;
        if(!child)
            return 0;
        status = vfs_metadata(child, request.metadata);
        if(scoped)
            request.metadata.generation = tree_generation(child, true);
        if(status < 0)
            return status;
        memset(request.name, 0, sizeof(request.name));
        memcpy(request.name, child->name, child->name_size);
        request.name_bytes = child->name_size;
        request.object_id = request.cursor = child->object_id;
        request.kind = child->kind;
        return 1;
    }
    child = lookup(node, request.name, request.name_bytes);
    if(child && excluded && vfs_contains(static_cast<VfsNode*>(excluded->object), child))
        return OS1_ERR_INVALID;
    if(child && child->filesystem_id != node->filesystem_id)
        return OS1_ERR_ACCESS;
    if(request.object_id && (!child || child->object_id != request.object_id))
        return OS1_ERR_CHANGED;
    if(request.action == OS1_TREE_REMOVE)
    {
        if(!child)
            return OS1_ERR_NOT_FOUND;
        if(!request.object_id || child->mount_root || !node->operations)
            return OS1_ERR_ACCESS;
        status =
            node->operations->remove_locked(node, request.name, request.name_bytes, request.kind);
        request.generation = tree_generation(node, scoped);
        return status;
    }
    const uint32_t permitted =
        parent.descendant_rights &
        ((parent.rights & OS1_RIGHT_DELEGATE) ? OS1_RIGHT_ALL : ~OS1_RIGHT_DELEGATE);
    if((request.rights | request.descendant_rights) & ~permitted)
        return OS1_ERR_ACCESS;
    OpenInstance* opened = nullptr;
    if(request.action == OS1_TREE_CREATE)
    {
        if(child)
            return OS1_ERR_EXISTS;
        if(!node->operations)
            return OS1_ERR_ACCESS;
        if(request.kind != OS1_OBJECT_FILE && request.kind != OS1_OBJECT_DIRECTORY)
            return OS1_ERR_INVALID;
        if(request.rights &
           ~(request.kind == OS1_OBJECT_FILE ? kTmpFileRights : kTmpDirectoryRights))
            return OS1_ERR_ACCESS;
        if(request.kind == OS1_OBJECT_FILE && request.descendant_rights)
            return OS1_ERR_INVALID;
        status = node->operations->create_locked(
            node, request.name, request.name_bytes, request.kind, opened);
        request.generation = tree_generation(node, scoped);
        if(status < 0)
            return status;
        child = static_cast<VfsNode*>(opened->object);
    }
    else
    {
        if(!child)
            return OS1_ERR_NOT_FOUND;
        if(request.kind && child->kind != request.kind)
            return OS1_ERR_INVALID;
        if(request.rights & ~child->allowed_rights)
            return OS1_ERR_ACCESS;
        if(child->kind != OS1_OBJECT_DIRECTORY && request.descendant_rights)
            return OS1_ERR_INVALID;
        opened = open_instance(child, &status);
        if(!opened)
            return status;
    }
    InstanceRef result(opened);
    status = vfs_metadata(child, request.metadata);
    if(scoped)
        request.metadata.generation = tree_generation(child, true);
    if(status < 0)
        return status;
    return descriptor_install(
        table, opened, request.rights, OS1_DESCRIPTOR_CLOEXEC, -1, request.descendant_rights);
}

long vfs_times(VfsNode* node, os1_file_times& out)
{
    out = {OS1_VFS_ABI_VERSION, sizeof(out), 0, 0, 0, 0, 0};
    return node->operations && node->operations->times ? node->operations->times(node, out) : 0;
}
long vfs_set_times(VfsNode* node, const os1_set_times& request)
{
    if(request.version != OS1_VFS_ABI_VERSION || request.size != sizeof(request) ||
       request.reserved || !request.fields || request.fields & ~(OS1_TIME_ACCESS | OS1_TIME_MODIFY))
        return OS1_ERR_INVALID;
    return node->operations && node->operations->set_times
               ? node->operations->set_times(node, request.fields)
               : OS1_ERR_UNSUPPORTED;
}
