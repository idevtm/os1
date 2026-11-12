#include "core/wall_clock.hpp"
#include "mm/kmem.hpp"
#include "sync/atomic.hpp"
#include "util/memory.h"
#include "vfs/filesystem.hpp"
#include "vfs/namespace.hpp"

struct Tmpfs
{
    Spinlock lock;
    volatile uint32_t references;
    uint32_t nodes, opens, pages;
    uint64_t next_cookie, identity;
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
    os1_file_times times;
};
void stamp(VfsNode* base, uint32_t fields)
{
    uint64_t now = 0;
    if(!wall_clock_now(now))
        return;
    auto& times = static_cast<TmpNode*>(base)->times;
    if(fields & OS1_TIME_ACCESS)
        times.access_ns = now;
    if(fields & OS1_TIME_MODIFY)
        times.modify_ns = now;
    times.change_ns = now;
    times.valid |= fields | OS1_TIME_CHANGE;
}
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
    stamp(node, OS1_TIME_ACCESS | OS1_TIME_MODIFY);
    node->object_id = vfs_identity();
    node->filesystem_id = fs->identity;
    node->filesystem = fs;
    node->operations = &tmpfs_operations;
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
    fs->identity = vfs_identity();
    TmpNode* mount = nullptr;
    long status = allocate_node(fs, OS1_OBJECT_DIRECTORY, mount);
    release_fs(fs);
    if(status < 0)
        return status;
    mount->mount_root = true;
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
static long create_locked(
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
            stamp(directory, OS1_TIME_MODIFY);
            vfs_changed(directory, node);
            result = instance.take();
            return 0;
        }
    }
    object_release(node);
    return status;
}
long tmpfs_create(VfsNode* p, const char* n, size_t s, uint32_t k, OpenInstance*& out)
{
    NamespaceGuard guard;
    return guard.held ? create_locked(p, n, s, k, out) : OS1_ERR_WOULD_BLOCK;
}

static long remove_locked(VfsNode* directory, const char* name, size_t size, uint32_t kind)
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
        if(kind && removed->kind != kind)
            return OS1_ERR_INVALID;
        if(removed->kind == OS1_OBJECT_DIRECTORY && removed->children)
            return OS1_ERR_NOT_EMPTY;
        *link = removed->next;
        removed->next = nullptr;
        removed->linked = false;
        stamp(directory, OS1_TIME_MODIFY);
        stamp(removed, 0);
    }
    vfs_changed(directory, removed);
    object_release(removed);
    return 0;
}
long tmpfs_remove(VfsNode* p, const char* n, size_t s, uint32_t k)
{
    NamespaceGuard guard;
    return guard.held ? remove_locked(p, n, s, k) : OS1_ERR_WOULD_BLOCK;
}

long tmpfs_rename(VfsNode* directory,
                  const char* source,
                  size_t source_size,
                  const char* destination,
                  size_t destination_size)
{
    NamespaceGuard namespace_guard;
    if(!namespace_guard.held)
        return OS1_ERR_WOULD_BLOCK;
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
    stamp(directory, OS1_TIME_MODIFY);
    stamp(node, 0);
    vfs_changed(directory, node);
    return 0;
}

namespace
{
void update_depth(VfsNode* node, unsigned depth)
{
    static_cast<TmpNode*>(node)->depth = depth;
    for(auto* child = node->children; child; child = child->next)
        update_depth(child, depth + 1);
}
long move(VfsNode* from,
          const char* source,
          size_t a,
          VfsNode* to,
          const char* destination,
          size_t b,
          bool replace,
          uint64_t source_id)
{
    if(!valid_name(source, a) || !valid_name(destination, b))
        return OS1_ERR_INVALID;
    if(from->filesystem != to->filesystem)
        return OS1_ERR_UNSUPPORTED;
    NamespaceGuard namespace_guard;
    if(!namespace_guard.held)
        return OS1_ERR_WOULD_BLOCK;
    VfsNode* target = nullptr;
    VfsNode* node = nullptr;
    {
        auto* fs = from->filesystem;
        IrqSpinGuard guard(fs->lock);
        if(!from->linked || !to->linked)
            return OS1_ERR_CLOSED;
        node = lookup_locked(from, source, a);
        if(source_id && (!node || node->object_id != source_id))
            return OS1_ERR_CHANGED;
        if(!node)
            return OS1_ERR_NOT_FOUND;
        target = lookup_locked(to, destination, b);
        if(node == target)
            return 0;
        if(vfs_contains(node, to))
            return OS1_ERR_INVALID;
        if(target && !replace)
            return OS1_ERR_EXISTS;
        if(target && target->kind != node->kind)
            return OS1_ERR_INVALID;
        if(target && target->children)
            return OS1_ERR_NOT_EMPTY;
        if(static_cast<TmpNode*>(to)->depth + 1 + vfs_height(node) > 32 ||
           fs->next_cookie == UINT64_MAX)
            return OS1_ERR_LIMIT;
        if(target)
        {
            vfs_detach(to, target);
            target->linked = false;
            stamp(target, 0);
        }
        vfs_detach(from, node);
        memset(node->name, 0, sizeof(node->name));
        memcpy(node->name, destination, b);
        node->name_size = b;
        update_depth(node, static_cast<TmpNode*>(to)->depth + 1);
        node->cookie = fs->next_cookie++;
        node->next = to->children;
        to->children = node;
        stamp(from, OS1_TIME_MODIFY);
        stamp(to, OS1_TIME_MODIFY);
        stamp(node, 0);
    }
    vfs_changed(from, to, node, target);
    object_release(target);
    return 0;
}
}  // namespace
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
long tmpfs_write(
    VfsNode* node, uint64_t offset, const void* input, size_t size, bool append, uint64_t* end)
{
    auto* file = static_cast<TmpNode*>(node);
    auto* fs = node->filesystem;
    IrqSpinGuard guard(fs->lock);
    if(append)
        offset = node->byte_size;
    if(size > 256 || offset > OS1_TMPFS_FILE_MAX || size > OS1_TMPFS_FILE_MAX - offset)
        return OS1_ERR_RANGE;
    if(!size)
        return 0;
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
    stamp(node, OS1_TIME_MODIFY);
    if(end)
        *end = offset + size;
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
    stamp(node, OS1_TIME_MODIFY);
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

static long metadata(VfsNode* base, os1_metadata& out)
{
    auto* n = static_cast<TmpNode*>(base);
    IrqSpinGuard guard(n->filesystem->lock);
    out.filesystem_kind = OS1_FILESYSTEM_TMPFS;
    out.valid |= OS1_METADATA_ALLOCATED;
    out.logical_bytes = n->byte_size;
    for(auto* page : n->pages)
        if(page)
            out.allocated_bytes += page_bytes;
    return 0;
}
static long space(VfsNode* n, os1_space& out)
{
    IrqSpinGuard guard(n->filesystem->lock);
    out.filesystem_kind = OS1_FILESYSTEM_TMPFS;
    out.total_bytes = uint64_t(OS1_TMPFS_PAGE_LIMIT) * page_bytes;
    out.free_bytes = out.available_bytes = out.total_bytes - n->filesystem->pages * page_bytes;
    return 0;
}
static long times(VfsNode* base, os1_file_times& out)
{
    IrqSpinGuard guard(base->filesystem->lock);
    out = static_cast<TmpNode*>(base)->times;
    out.version = OS1_VFS_ABI_VERSION;
    out.size = sizeof(out);
    out.resolution_ns = 1000000;
    return 0;
}
static long set_times(VfsNode* base, uint32_t fields)
{
    IrqSpinGuard guard(base->filesystem->lock);
    uint64_t now = 0;
    if(!wall_clock_now(now))
        return OS1_ERR_UNSUPPORTED;
    stamp(base, fields);
    return 0;
}
const VfsOperations tmpfs_operations{tmpfs_lookup,
                                     tmpfs_create,
                                     tmpfs_remove,
                                     tmpfs_rename,
                                     tmpfs_read,
                                     tmpfs_write,
                                     tmpfs_truncate,
                                     tmpfs_seek,
                                     tmpfs_next,
                                     tmpfs_info,
                                     [](VfsNode*) -> long { return 0; },
                                     nullptr,
                                     move,
                                     metadata,
                                     space,
                                     create_locked,
                                     remove_locked,
                                     times,
                                     set_times};
bool vfs_backed(const NativeObject* object)
{
    return object && (object->kind == OS1_OBJECT_FILE || object->kind == OS1_OBJECT_DIRECTORY) &&
           static_cast<const VfsNode*>(object)->operations;
}
