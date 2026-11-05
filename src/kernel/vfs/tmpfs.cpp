#include "vfs/tmpfs.hpp"

#include "mm/kmem.hpp"
#include "sync/atomic.hpp"
#include "util/memory.h"

struct Tmpfs
{
    Spinlock lock;
    volatile uint32_t references;
    uint32_t nodes, opens, pages;
    uint64_t next_cookie;
};
#ifdef OS1_HOST_TEST
int host_tmpfs_allocation_budget = -1;
#endif
namespace
{
constexpr size_t page_bytes = 4096;
struct TmpNode : VfsNode
{
    uint8_t* pages[OS1_TMPFS_FILE_MAX / page_bytes];
    uint32_t depth;
};
void* allocate(size_t bytes)
{
#ifdef OS1_HOST_TEST
    if(host_tmpfs_allocation_budget == 0)
        return nullptr;
    if(host_tmpfs_allocation_budget > 0)
        --host_tmpfs_allocation_budget;
#endif
    return kmalloc(bytes, KmallocFlags::Zero);
}
void release_fs(Tmpfs* fs)
{
    if(__atomic_fetch_sub(&fs->references, 1u, __ATOMIC_ACQ_REL) == 1u)
        kfree(fs);
}
long acquire_open(NativeObject* object)
{
    auto* fs = static_cast<VfsNode*>(object)->filesystem;
    IrqSpinGuard guard(fs->lock);
    if(fs->opens == OS1_TMPFS_OPEN_LIMIT)
        return OS1_ERR_LIMIT;
    ++fs->opens;
    return 0;
}
void release_open(NativeObject* object)
{
    auto* fs = static_cast<VfsNode*>(object)->filesystem;
    IrqSpinGuard guard(fs->lock);
    --fs->opens;
}
void destroy_node(NativeObject* object)
{
    auto* node = static_cast<TmpNode*>(object);
    auto* fs = node->filesystem;
    auto* children = node->children;
    {
        IrqSpinGuard guard(fs->lock);
        --fs->nodes;
        for(auto* page : node->pages)
            if(page)
                --fs->pages;
    }
    // Final object release runs outside filesystem/table/IPC locks.
    while(children)
    {
        auto* next = children->next;
        object_release(children);
        children = next;
    }
    for(auto* page : node->pages)
        kfree(page);
    kfree(node);
    release_fs(fs);
}
bool valid_name(const char* name, size_t length)
{
    if(!name || !length || length > OS1_TMPFS_NAME_MAX)
        return false;
    if(name[0] == '.' && (length == 1 || (length == 2 && name[1] == '.')))
        return false;
    for(size_t i = 0; i < length; ++i)
        if(!name[i] || name[i] == '/')
            return false;
    return true;
}
bool same_name(const char* a, const char* b, size_t size)
{
    for(size_t i = 0; i < size; ++i)
        if(a[i] != b[i])
            return false;
    return true;
}
VfsNode* lookup_locked(VfsNode* directory, const char* name, size_t size)
{
    for(auto* n = directory->children; n; n = n->next)
        if(n->name_size == size && same_name(n->name, name, size))
            return n;
    return nullptr;
}
long allocate_node(Tmpfs* fs, uint32_t kind, TmpNode*& node)
{
    node = nullptr;
    {
        IrqSpinGuard guard(fs->lock);
        if(fs->nodes == OS1_TMPFS_NODE_LIMIT)
            return OS1_ERR_LIMIT;
        ++fs->nodes;
    }
    node = static_cast<TmpNode*>(allocate(sizeof(TmpNode)));
    if(!node)
    {
        IrqSpinGuard guard(fs->lock);
        --fs->nodes;
        return OS1_ERR_NO_MEMORY;
    }
    (void)atomic_fetch_add(&fs->references, 1u);
    node->filesystem = fs;
    node->references = 1;
    node->kind = kind;
    node->interfaces =
        kind == OS1_OBJECT_DIRECTORY
            ? OS1_INTERFACE_DIRECTORY
            : OS1_INTERFACE_BYTE_INPUT | OS1_INTERFACE_BYTE_OUTPUT | OS1_INTERFACE_SEEK;
    node->allowed_rights = kind == OS1_OBJECT_DIRECTORY ? kTmpDirectoryRights : kTmpFileRights;
    node->destroy = destroy_node;
    node->acquire_open = acquire_open;
    node->release_open = release_open;
    return 0;
}
}  // namespace
bool tmpfs_node(const NativeObject* object)
{
    return object && (object->kind == OS1_OBJECT_DIRECTORY || object->kind == OS1_OBJECT_FILE) &&
           static_cast<const VfsNode*>(object)->filesystem;
}
long tmpfs_mount(NativeObject* object)
{
    if(!object || object->kind != OS1_OBJECT_DIRECTORY || tmpfs_node(object))
        return OS1_ERR_INVALID;
    auto* root = static_cast<VfsNode*>(object);
    if(lookup_locked(root, "tmp", 3))
        return OS1_ERR_EXISTS;
    auto* fs = static_cast<Tmpfs*>(allocate(sizeof(Tmpfs)));
    if(!fs)
        return OS1_ERR_NO_MEMORY;
    fs->lock.reset("tmpfs");
    fs->references = 1;
    fs->next_cookie = 1;
    TmpNode* mount = nullptr;
    long status = allocate_node(fs, OS1_OBJECT_DIRECTORY, mount);
    release_fs(fs);
    if(status < 0)
        return status;
    mount->name_size = 3;
    memcpy(mount->name, "tmp", 3);
    mount->linked = true;
    mount->next = root->children;
    root->children = mount;
    return 0;
}
long tmpfs_lookup(VfsNode* directory, const char* name, size_t size, NativeObject*& result)
{
    result = nullptr;
    if(!valid_name(name, size))
        return OS1_ERR_INVALID;
    IrqSpinGuard guard(directory->filesystem->lock);
    if(!directory->linked)
        return OS1_ERR_CLOSED;
    auto* node = lookup_locked(directory, name, size);
    if(!node)
        return OS1_ERR_NOT_FOUND;
    object_retain(node);
    result = node;
    return 0;
}
long tmpfs_create(
    VfsNode* directory, const char* name, size_t size, uint32_t kind, OpenInstance*& result)
{
    result = nullptr;
    if(!valid_name(name, size) || (kind != OS1_OBJECT_DIRECTORY && kind != OS1_OBJECT_FILE))
        return OS1_ERR_INVALID;
    if(static_cast<TmpNode*>(directory)->depth >= 32)
        return OS1_ERR_LIMIT;
    auto* fs = directory->filesystem;
    TmpNode* node = nullptr;
    long status = allocate_node(fs, kind, node);
    if(status < 0)
        return status;
    InstanceRef instance(open_instance(node, &status));
    if(instance.get())
    {
        IrqSpinGuard guard(fs->lock);
        if(!directory->linked)
            status = OS1_ERR_CLOSED;
        else if(lookup_locked(directory, name, size))
            status = OS1_ERR_EXISTS;
        else if(fs->next_cookie == UINT64_MAX)
            status = OS1_ERR_LIMIT;
        else
        {
            node->name_size = size;
            memcpy(node->name, name, size);
            node->depth = static_cast<TmpNode*>(directory)->depth + 1;
            node->cookie = fs->next_cookie++;
            node->linked = true;
            node->next = directory->children;
            directory->children = node;
            result = instance.take();
            return 0;
        }
    }
    object_release(node);
    return status;
}
long tmpfs_remove(VfsNode* directory, const char* name, size_t size)
{
    if(!valid_name(name, size))
        return OS1_ERR_INVALID;
    VfsNode* removed = nullptr;
    {
        IrqSpinGuard guard(directory->filesystem->lock);
        if(!directory->linked)
            return OS1_ERR_CLOSED;
        VfsNode** link = &directory->children;
        while(*link && ((*link)->name_size != size || !same_name((*link)->name, name, size)))
            link = &(*link)->next;
        if(!*link)
            return OS1_ERR_NOT_FOUND;
        removed = *link;
        if(removed->kind == OS1_OBJECT_DIRECTORY && removed->children)
            return OS1_ERR_NOT_EMPTY;
        *link = removed->next;
        removed->next = nullptr;
        removed->linked = false;
    }
    object_release(removed);
    return 0;
}
long tmpfs_rename(VfsNode* directory,
                  const char* source,
                  size_t source_size,
                  const char* destination,
                  size_t destination_size)
{
    if(!valid_name(source, source_size) || !valid_name(destination, destination_size))
        return OS1_ERR_INVALID;
    auto* fs = directory->filesystem;
    IrqSpinGuard guard(fs->lock);
    if(!directory->linked)
        return OS1_ERR_CLOSED;
    auto* node = lookup_locked(directory, source, source_size);
    if(!node)
        return OS1_ERR_NOT_FOUND;
    auto* target = lookup_locked(directory, destination, destination_size);
    if(target == node)
        return 0;
    if(target)
        return OS1_ERR_EXISTS;
    if(fs->next_cookie == UINT64_MAX)
        return OS1_ERR_LIMIT;
    memset(node->name, 0, sizeof(node->name));
    memcpy(node->name, destination, destination_size);
    node->name_size = destination_size;
    node->cookie = fs->next_cookie++;
    return 0;
}
long tmpfs_read(VfsNode* node, uint64_t offset, void* output, size_t size)
{
    IrqSpinGuard guard(node->filesystem->lock);
    if(offset >= node->byte_size)
        return 0;
    if(size > node->byte_size - offset)
        size = node->byte_size - offset;
    auto* file = static_cast<TmpNode*>(node);
    auto* bytes = static_cast<uint8_t*>(output);
    for(size_t i = 0; i < size; ++i)
    {
        uint64_t at = offset + i;
        auto* page = file->pages[at / page_bytes];
        bytes[i] = page ? page[at % page_bytes] : 0;
    }
    return size;
}
long tmpfs_write(VfsNode* node, uint64_t offset, const void* input, size_t size)
{
    if(size > 256 || offset > OS1_TMPFS_FILE_MAX || size > OS1_TMPFS_FILE_MAX - offset)
        return OS1_ERR_RANGE;
    if(!size)
        return 0;
    auto* file = static_cast<TmpNode*>(node);
    auto* fs = node->filesystem;
    IrqSpinGuard guard(fs->lock);
    unsigned first = offset / page_bytes, last = (offset + size - 1) / page_bytes;
    uint8_t* fresh[2]{};
    unsigned needed = 0;
    for(unsigned p = first; p <= last; ++p)
        if(!file->pages[p])
            ++needed;
    if(needed > OS1_TMPFS_PAGE_LIMIT - fs->pages)
        return OS1_ERR_LIMIT;
    // Allocation is bounded to two pages. Lock order is filesystem -> allocator;
    // allocator paths never acquire filesystem locks or invoke resource callbacks.
    for(unsigned p = first; p <= last; ++p)
    {
        if(file->pages[p])
            continue;
        fresh[p - first] = static_cast<uint8_t*>(allocate(page_bytes));
        if(!fresh[p - first])
        {
            for(auto* page : fresh)
                kfree(page);
            return OS1_ERR_NO_MEMORY;
        }
    }
    for(unsigned p = first; p <= last; ++p)
        if(fresh[p - first])
            file->pages[p] = fresh[p - first];
    fs->pages += needed;
    auto* bytes = static_cast<const uint8_t*>(input);
    for(size_t i = 0; i < size; ++i)
        file->pages[(offset + i) / page_bytes][(offset + i) % page_bytes] = bytes[i];
    if(offset + size > node->byte_size)
        node->byte_size = offset + size;
    return size;
}
long tmpfs_truncate(VfsNode* node, uint64_t size)
{
    if(size > OS1_TMPFS_FILE_MAX)
        return OS1_ERR_RANGE;
    auto* file = static_cast<TmpNode*>(node);
    auto* fs = node->filesystem;
    IrqSpinGuard guard(fs->lock);
    if(size < node->byte_size)
    {
        for(unsigned p = (size + page_bytes - 1) / page_bytes; p < OS1_TMPFS_FILE_MAX / page_bytes;
            ++p)
        {
            if(file->pages[p])
            {
                kfree(file->pages[p]);
                file->pages[p] = nullptr;
                --fs->pages;
            }
        }
        if(size % page_bytes && file->pages[size / page_bytes])
            memset(file->pages[size / page_bytes] + size % page_bytes,
                   0,
                   page_bytes - size % page_bytes);
    }
    node->byte_size = size;
    return 0;
}
long tmpfs_seek(VfsNode* node, uint64_t cursor, int64_t offset, uint32_t origin)
{
    if(origin > OS1_SEEK_END)
        return OS1_ERR_INVALID;
    IrqSpinGuard guard(node->filesystem->lock);
    uint64_t base = origin == OS1_SEEK_SET ? 0 : origin == OS1_SEEK_CUR ? cursor : node->byte_size;
    if(base > OS1_TMPFS_FILE_MAX)
        return OS1_ERR_RANGE;
    if(offset < 0)
    {
        uint64_t delta = static_cast<uint64_t>(-(offset + 1)) + 1;
        return delta > base ? OS1_ERR_RANGE : static_cast<long>(base - delta);
    }
    return static_cast<uint64_t>(offset) > OS1_TMPFS_FILE_MAX - base
               ? OS1_ERR_RANGE
               : static_cast<long>(base + offset);
}
long tmpfs_next(VfsNode* directory, uint64_t cursor, os1_directory_entry& entry, uint64_t& next)
{
    IrqSpinGuard guard(directory->filesystem->lock);
    VfsNode* chosen = nullptr;
    for(auto* n = directory->children; n; n = n->next)
        if(n->cookie > cursor && (!chosen || n->cookie < chosen->cookie))
            chosen = n;
    if(!chosen)
        return 0;
    entry = {};
    entry.kind = chosen->kind;
    entry.name_bytes = chosen->name_size;
    memcpy(entry.name, chosen->name, chosen->name_size);
    next = chosen->cookie;
    return sizeof(entry);
}
os1_filesystem_info tmpfs_info(VfsNode* node)
{
    auto* fs = node->filesystem;
    IrqSpinGuard guard(fs->lock);
    return {node->byte_size, node->kind, fs->nodes, fs->opens, fs->pages, OS1_TMPFS_PAGE_LIMIT, 0};
}
