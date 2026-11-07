#include "vfs/ext2.hpp"

#include "fs/ext2.hpp"
#include "mm/kmem.hpp"
#include "storage/block_device.hpp"
#include "sync/atomic.hpp"
#include "util/memory.h"
#include "vfs/filesystem.hpp"
#include "vfs/namespace.hpp"
namespace
{
struct DiskFs
{
    ext2::Volume volume;
    BlockDevice* device;
    uint64_t start, sectors;
    volatile bool busy;
    uint32_t references, nodes;
    uint64_t cookie;
    bool seen[ext2::max_inodes + 1];
};
struct DiskNode : VfsNode
{
    DiskFs* disk;
    ext2::Inode inode;
    uint32_t number, depth;
};
struct Guard
{
    DiskFs& fs;
    bool held;
    explicit Guard(DiskFs& f) : fs(f)
    {
        bool expected = false;
        held = atomic_compare_exchange_strong(&fs.busy, &expected, true);
    }
    ~Guard()
    {
        if(held)
            atomic_store_release(&fs.busy, false);
    }
};
bool read_block(void* context, uint32_t b, void* out)
{
    auto& fs = *static_cast<DiskFs*>(context);
    return uint64_t(b) * 8 + 8 <= fs.sectors &&
           block_read_sync(*fs.device, fs.start + uint64_t(b) * 8, out, 8);
}
bool write_block(void* context, uint32_t b, const void* in)
{
    auto& fs = *static_cast<DiskFs*>(context);
    return uint64_t(b) * 8 + 8 <= fs.sectors &&
           block_write_sync(*fs.device, fs.start + uint64_t(b) * 8, in, 8);
}
bool flush(void* context)
{
    return block_flush_sync(*static_cast<DiskFs*>(context)->device);
}
bool same(const char* a, const char* b, size_t size)
{
    for(size_t i = 0; i < size; ++i)
        if(a[i] != b[i])
            return false;
    return true;
}
bool valid(const char* name, size_t size)
{
    if(!name || !size || size > 63 ||
       (name[0] == '.' && (size == 1 || (size == 2 && name[1] == '.'))))
        return false;
    for(size_t i = 0; i < size; ++i)
        if(!name[i] || name[i] == '/')
            return false;
    return true;
}
VfsNode* find(VfsNode* parent, const char* name, size_t size)
{
    for(auto* n = parent->children; n; n = n->next)
        if(n->name_size == size && same(n->name, name, size))
            return n;
    return nullptr;
}
void destroy(NativeObject* object)
{
    auto* n = static_cast<DiskNode*>(object);
    auto* fs = n->disk;
    while(n->children)
    {
        auto* child = n->children;
        n->children = child->next;
        object_release(child);
    }
    kfree(n);
    if(__atomic_sub_fetch(&fs->references, 1u, __ATOMIC_ACQ_REL) == 0)
        kfree(fs);
}
extern const VfsOperations operations;
DiskNode* node(DiskFs& fs,
               uint32_t number,
               const ext2::Inode& inode,
               const char* name,
               size_t length,
               uint32_t depth)
{
    auto* n = static_cast<DiskNode*>(kmalloc(sizeof(DiskNode), KmallocFlags::Zero));
    if(!n)
        return nullptr;
    ++fs.references;
    __atomic_add_fetch(&fs.nodes, 1u, __ATOMIC_RELAXED);
    n->disk = &fs;
    n->number = number;
    n->inode = inode;
    n->depth = depth;
    n->references = 1;
    n->operations = &operations;
    n->linked = true;
    n->cookie = ++fs.cookie;
    n->kind = ext2::directory(inode) ? OS1_OBJECT_DIRECTORY : OS1_OBJECT_FILE;
    n->allowed_rights = n->kind == OS1_OBJECT_DIRECTORY ? kTmpDirectoryRights : kTmpFileRights;
    // Executable files are immutable in this profile. New writable files cannot gain EXECUTE.
    if(n->kind == OS1_OBJECT_FILE && (ext2::u16(inode.bytes) & 0111))
        n->allowed_rights = (kFileRights);
    n->interfaces =
        n->kind == OS1_OBJECT_DIRECTORY
            ? OS1_INTERFACE_DIRECTORY
            : OS1_INTERFACE_BYTE_INPUT | OS1_INTERFACE_SEEK |
                  ((n->allowed_rights & OS1_RIGHT_WRITE) ? OS1_INTERFACE_BYTE_OUTPUT : 0);
    n->byte_size = n->kind == OS1_OBJECT_FILE ? ext2::size(inode) : 0;
    n->name_size = length;
    memcpy(n->name, name, length);
    n->destroy = destroy;
    return n;
}
long import(DiskNode* parent, uint32_t parent_number)
{
    auto& fs = *parent->disk;
    if(parent->depth >= 32)
        return OS1_ERR_LIMIT;
    uint64_t offset = 0, next = 0;
    ext2::Entry e{};
    bool dot = false, dotdot = false;
    uint32_t subdirectories = 0;
    for(;;)
    {
        long r = fs.volume.next(parent->inode, offset, e, next);
        if(r < 0)
            return r;
        if(!r)
            return dot && dotdot && ext2::u16(parent->inode.bytes + 26) == 2 + subdirectories
                       ? 0
                       : OS1_ERR_BAD_IMAGE;
        offset = next;
        if(e.name[0] == '.' && e.length == 1)
        {
            if(dot || e.inode != parent->number || e.type != 2)
                return OS1_ERR_BAD_IMAGE;
            dot = true;
            continue;
        }
        if(e.length == 2 && e.name[0] == '.' && e.name[1] == '.')
        {
            if(dotdot || e.inode != parent_number || e.type != 2)
                return OS1_ERR_BAD_IMAGE;
            dotdot = true;
            continue;
        }
        if(e.inode < 11 || fs.seen[e.inode] || find(parent, e.name, e.length))
            return OS1_ERR_BAD_IMAGE;
        fs.seen[e.inode] = true;
        ext2::Inode inode{};
        r = fs.volume.inode_read(e.inode, inode);
        if(r < 0)
            return r;
        if((e.type == 2) != ext2::directory(inode))
            return OS1_ERR_BAD_IMAGE;
        r = fs.volume.validate_inode(e.inode, inode);
        if(r < 0)
            return r;
        auto* child = node(fs, e.inode, inode, e.name, e.length, parent->depth + 1);
        if(!child)
            return OS1_ERR_NO_MEMORY;
        child->next = parent->children;
        parent->children = child;
        if(ext2::directory(inode))
        {
            ++subdirectories;
            r = import(child, parent->number);
            if(r < 0)
                return r;
        }
    }
}
long lookup(VfsNode* base, const char* name, size_t size, NativeObject*& out)
{
    out = nullptr;
    auto* n = static_cast<DiskNode*>(base);
    Guard g(*n->disk);
    if(!g.held)
        return OS1_ERR_WOULD_BLOCK;
    if(!valid(name, size))
        return OS1_ERR_INVALID;
    if(!n->linked)
        return OS1_ERR_CLOSED;
    auto* child = find(n, name, size);
    if(!child)
        return OS1_ERR_NOT_FOUND;
    object_retain(child);
    out = child;
    return 0;
}
long create(VfsNode* base, const char* name, size_t size, uint32_t kind, OpenInstance*& out)
{
    NamespaceGuard namespace_guard;
    if(!namespace_guard.held)
        return OS1_ERR_WOULD_BLOCK;
    out = nullptr;
    auto* p = static_cast<DiskNode*>(base);
    auto& fs = *p->disk;
    Guard g(fs);
    if(!g.held)
        return OS1_ERR_WOULD_BLOCK;
    if(!valid(name, size) || (kind != OS1_OBJECT_FILE && kind != OS1_OBJECT_DIRECTORY))
        return OS1_ERR_INVALID;
    if(!p->linked)
        return OS1_ERR_CLOSED;
    if(p->depth >= 31)
        return OS1_ERR_LIMIT;
    if(find(p, name, size))
        return OS1_ERR_EXISTS;
    // Reserve object and open-instance memory before mutating disk metadata.
    ext2::Inode initial{};
    ext2::put16(initial.bytes, kind == OS1_OBJECT_DIRECTORY ? 0x41ed : 0x81a4);
    auto* child = node(fs, 0, initial, name, size, p->depth + 1);
    if(!child)
        return OS1_ERR_NO_MEMORY;
    long r = 0;
    InstanceRef opened(open_instance(child, &r));
    if(!opened.get())
    {
        __atomic_sub_fetch(&fs.nodes, 1u, __ATOMIC_RELAXED);
        object_release(child);
        return r;
    }
    r = fs.volume.create(
        p->number, p->inode, name, size, kind == OS1_OBJECT_DIRECTORY, child->number, child->inode);
    if(r < 0)
    {
        __atomic_sub_fetch(&fs.nodes, 1u, __ATOMIC_RELAXED);
        object_release(child);
        return r;
    }
    child->next = p->children;
    p->children = child;
    out = opened.take();
    return 0;
}
long remove(VfsNode* base, const char* name, size_t size, uint32_t kind)
{
    NamespaceGuard namespace_guard;
    if(!namespace_guard.held)
        return OS1_ERR_WOULD_BLOCK;
    auto* p = static_cast<DiskNode*>(base);
    auto& fs = *p->disk;
    Guard g(fs);
    if(!g.held)
        return OS1_ERR_WOULD_BLOCK;
    if(!valid(name, size))
        return OS1_ERR_INVALID;
    if(!p->linked)
        return OS1_ERR_CLOSED;
    auto* child = static_cast<DiskNode*>(find(p, name, size));
    if(!child)
        return OS1_ERR_NOT_FOUND;
    if(kind && child->kind != kind)
        return OS1_ERR_INVALID;
    if(child->children)
        return OS1_ERR_NOT_EMPTY;
    // Ext2 has no orphan journal. Do not unlink an object retained by a caller.
    if(atomic_load_acquire(&child->references) != 1)
        return OS1_ERR_WOULD_BLOCK;
    long r = fs.volume.remove(p->number, p->inode, name, size, child->number, child->inode);
    if(r < 0)
        return r;
    VfsNode** at = &p->children;
    while(*at != child)
        at = &(*at)->next;
    *at = child->next;
    child->next = nullptr;
    child->linked = false;
    __atomic_sub_fetch(&fs.nodes, 1u, __ATOMIC_RELAXED);
    object_release(child);
    return 0;
}
long rename(VfsNode* base, const char* from, size_t a, const char* to, size_t b)
{
    NamespaceGuard namespace_guard;
    if(!namespace_guard.held)
        return OS1_ERR_WOULD_BLOCK;
    auto* p = static_cast<DiskNode*>(base);
    auto& fs = *p->disk;
    Guard g(fs);
    if(!g.held)
        return OS1_ERR_WOULD_BLOCK;
    if(!valid(from, a) || !valid(to, b))
        return OS1_ERR_INVALID;
    if(!p->linked)
        return OS1_ERR_CLOSED;
    auto* child = find(p, from, a);
    if(!child)
        return OS1_ERR_NOT_FOUND;
    auto* target = find(p, to, b);
    if(target == child)
        return 0;
    if(target)
        return OS1_ERR_EXISTS;
    long r = fs.volume.rename(p->inode, from, a, to, b);
    if(r < 0)
        return r;
    memset(child->name, 0, sizeof(child->name));
    memcpy(child->name, to, b);
    child->name_size = b;
    child->cookie = ++fs.cookie;
    return 0;
}

void set_depth(VfsNode* node, unsigned depth)
{
    static_cast<DiskNode*>(node)->depth = depth;
    for(auto* child = node->children; child; child = child->next)
        set_depth(child, depth + 1);
}
long move(VfsNode* from,
          const char* source,
          size_t a,
          VfsNode* to,
          const char* destination,
          size_t b,
          bool replace)
{
    NamespaceGuard namespace_guard;
    if(!namespace_guard.held)
        return OS1_ERR_WOULD_BLOCK;
    auto* p = static_cast<DiskNode*>(from);
    auto* q = static_cast<DiskNode*>(to);
    if(p->disk != q->disk)
        return OS1_ERR_UNSUPPORTED;
    auto& fs = *p->disk;
    Guard guard(fs);
    if(!guard.held)
        return OS1_ERR_WOULD_BLOCK;
    if(!valid(source, a) || !valid(destination, b))
        return OS1_ERR_INVALID;
    if(!p->linked || !q->linked)
        return OS1_ERR_CLOSED;
    auto* child = static_cast<DiskNode*>(find(p, source, a));
    auto* target = static_cast<DiskNode*>(find(q, destination, b));
    if(!child)
        return OS1_ERR_NOT_FOUND;
    if(child == target)
        return 0;
    if(vfs_contains(child, q))
        return OS1_ERR_INVALID;
    if(target && !replace)
        return OS1_ERR_EXISTS;
    if(target && target->kind != child->kind)
        return OS1_ERR_INVALID;
    if(target && target->children)
        return OS1_ERR_NOT_EMPTY;
    if(target && atomic_load_acquire(&target->references) != 1)
        return OS1_ERR_WOULD_BLOCK;
    if(q->depth + 1 + vfs_height(child) >= 32 || fs.cookie == UINT64_MAX)
        return OS1_ERR_LIMIT;
    auto* scratch = static_cast<uint8_t*>(kmalloc(2 * ext2::block_bytes));
    if(!scratch)
        return OS1_ERR_NO_MEMORY;
    long r = fs.volume.move(p->number,
                            p->inode,
                            source,
                            a,
                            q->number,
                            q->inode,
                            destination,
                            b,
                            child->number,
                            child->inode,
                            target ? target->number : 0,
                            target ? &target->inode : nullptr,
                            scratch);
    kfree(scratch);
    if(r < 0)
        return r;
    if(target)
    {
        vfs_detach(q, target);
        target->linked = false;
        __atomic_sub_fetch(&fs.nodes, 1u, __ATOMIC_RELAXED);
        object_release(target);
    }
    vfs_detach(p, child);
    memset(child->name, 0, sizeof(child->name));
    memcpy(child->name, destination, b);
    child->name_size = b;
    set_depth(child, q->depth + 1);
    child->cookie = ++fs.cookie;
    child->next = q->children;
    q->children = child;
    return 0;
}
long read(VfsNode* base, uint64_t offset, void* out, size_t size)
{
    auto* n = static_cast<DiskNode*>(base);
    Guard g(*n->disk);
    if(!g.held)
        return OS1_ERR_WOULD_BLOCK;
    return n->disk->volume.read(n->inode, offset, out, size);
}
long write(VfsNode* base, uint64_t offset, const void* in, size_t size, bool append, uint64_t* end)
{
    auto* n = static_cast<DiskNode*>(base);
    Guard g(*n->disk);
    if(!g.held)
        return OS1_ERR_WOULD_BLOCK;
    if(!(n->allowed_rights & OS1_RIGHT_WRITE))
        return OS1_ERR_ACCESS;
    if(size > 256)
        return OS1_ERR_RANGE;
    if(append)
        offset = n->byte_size;
    long r = n->disk->volume.write(n->number, n->inode, offset, in, size);
    if(r >= 0)
    {
        __atomic_store_n(&n->byte_size, uint64_t(ext2::size(n->inode)), __ATOMIC_RELAXED);
        if(end && size)
            *end = offset + r;
    }
    return r;
}
long truncate(VfsNode* base, uint64_t size)
{
    auto* n = static_cast<DiskNode*>(base);
    Guard g(*n->disk);
    if(!g.held)
        return OS1_ERR_WOULD_BLOCK;
    if(!(n->allowed_rights & OS1_RIGHT_WRITE))
        return OS1_ERR_ACCESS;
    long r = n->disk->volume.truncate(n->number, n->inode, size);
    if(r == 0)
        __atomic_store_n(&n->byte_size, size, __ATOMIC_RELAXED);
    return r;
}
long seek(VfsNode* base, uint64_t cursor, int64_t offset, uint32_t origin)
{
    auto* n = static_cast<DiskNode*>(base);
    Guard g(*n->disk);
    if(!g.held)
        return OS1_ERR_WOULD_BLOCK;
    if(origin > OS1_SEEK_END)
        return OS1_ERR_INVALID;
    uint64_t start = origin == OS1_SEEK_SET ? 0 : origin == OS1_SEEK_CUR ? cursor : n->byte_size;
    if(start > ext2::max_file_bytes)
        return OS1_ERR_RANGE;
    if(offset < 0)
    {
        uint64_t d = uint64_t(-(offset + 1)) + 1;
        return d > start ? OS1_ERR_RANGE : static_cast<long>(start - d);
    }
    return uint64_t(offset) > ext2::max_file_bytes - start ? OS1_ERR_RANGE
                                                           : static_cast<long>(start + offset);
}
long next(VfsNode* base, uint64_t cursor, os1_directory_entry& entry, uint64_t& after)
{
    auto* p = static_cast<DiskNode*>(base);
    Guard g(*p->disk);
    if(!g.held)
        return OS1_ERR_WOULD_BLOCK;
    VfsNode* choice = nullptr;
    for(auto* c = p->children; c; c = c->next)
        if(c->cookie > cursor && (!choice || c->cookie < choice->cookie))
            choice = c;
    if(!choice)
        return 0;
    entry = {};
    entry.kind = choice->kind;
    entry.name_bytes = choice->name_size;
    memcpy(entry.name, choice->name, choice->name_size);
    after = choice->cookie;
    return sizeof(entry);
}
os1_filesystem_info info(VfsNode* base)
{
    auto* n = static_cast<DiskNode*>(base);
    // Individual atomic observations; these diagnostic counts are not a transaction snapshot.
    return {__atomic_load_n(&n->byte_size, __ATOMIC_RELAXED),
            n->kind,
            __atomic_load_n(&n->disk->nodes, __ATOMIC_RELAXED),
            0,
            0,
            n->disk->volume.blocks,
            0};
}
long sync(VfsNode* base)
{
    auto* n = static_cast<DiskNode*>(base);
    Guard g(*n->disk);
    if(!g.held)
        return OS1_ERR_WOULD_BLOCK;
    return n->disk->volume.sync();
}
long snapshot(VfsNode* base, uint8_t*& out, uint64_t& length)
{
    out = nullptr;
    length = 0;
    auto* n = static_cast<DiskNode*>(base);
    Guard g(*n->disk);
    if(!g.held)
        return OS1_ERR_WOULD_BLOCK;
    if(!(n->allowed_rights & OS1_RIGHT_EXECUTE) || !n->byte_size)
        return OS1_ERR_ACCESS;
    auto* bytes = static_cast<uint8_t*>(kmalloc(n->byte_size));
    if(!bytes)
        return OS1_ERR_NO_MEMORY;
    long r = n->disk->volume.read(n->inode, 0, bytes, n->byte_size);
    if(r < 0 || uint64_t(r) != n->byte_size)
    {
        kfree(bytes);
        return r < 0 ? r : OS1_ERR_IO;
    }
    out = bytes;
    length = r;
    return 0;
}
const VfsOperations operations{
    lookup, create, remove, rename, read, write, truncate, seek, next, info, sync, snapshot, move};
}  // namespace
long ext2_mount(NativeObject* root, BlockDevice& device)
{
    if(!root || root->kind != OS1_OBJECT_DIRECTORY || device.sector_size != 512)
        return OS1_ERR_INVALID;
    uint8_t mbr[512]{};
    if(!block_read_sync(device, 0, mbr, 1))
        return OS1_ERR_IO;
    if(mbr[510] != 0x55 || mbr[511] != 0xaa)
        return OS1_ERR_NOT_FOUND;
    const uint8_t* partition = nullptr;
    for(unsigned i = 0; i < 4; ++i)
    {
        auto* p = mbr + 446 + 16 * i;
        if(p[4] == 0x83)
        {
            if(partition)
                return OS1_ERR_BAD_IMAGE;
            partition = p;
        }
    }
    if(!partition)
        return OS1_ERR_NOT_FOUND;
    uint64_t start = ext2::u32(partition + 8), count = ext2::u32(partition + 12);
    if(start < 2048 || start % 8 || !count || count % 8 || start >= device.sector_count ||
       count > device.sector_count - start || count / 8 > ext2::max_blocks)
        return OS1_ERR_BAD_IMAGE;
    // Reject overlap with every other declared partition.
    for(unsigned i = 0; i < 4; ++i)
    {
        auto* p = mbr + 446 + 16 * i;
        if(p == partition || !p[4])
            continue;
        uint64_t s = ext2::u32(p + 8), c = ext2::u32(p + 12);
        if(!c || s >= device.sector_count || c > device.sector_count - s ||
           (s < start + count && start < s + c))
            return OS1_ERR_BAD_IMAGE;
    }
    auto* fs = static_cast<DiskFs*>(kmalloc(sizeof(DiskFs), KmallocFlags::Zero));
    if(!fs)
        return OS1_ERR_NO_MEMORY;
    fs->device = &device;
    fs->start = start;
    fs->sectors = count;
    long r = fs->volume.mount({fs, read_block, write_block, flush, uint32_t(count / 8)});
    ext2::Inode inode{};
    if(r == 0)
        r = fs->volume.inode_read(2, inode);
    if(r == 0 && !ext2::directory(inode))
        r = OS1_ERR_BAD_IMAGE;
    if(r == 0)
        r = fs->volume.validate_inode(2, inode);
    if(r < 0)
    {
        kfree(fs);
        return r;
    }
    auto* mount = node(*fs, 2, inode, "disk", 4, 0);
    if(!mount)
    {
        kfree(fs);
        return OS1_ERR_NO_MEMORY;
    }
    fs->seen[2] = true;
    r = import(mount, 2);
    if(r == 0)
    {
        for(uint32_t n = 11; n <= fs->volume.inodes; ++n)
            if(bool(fs->volume.inode_bits[(n - 1) / 8] & (1u << ((n - 1) % 8))) != fs->seen[n])
                r = OS1_ERR_BAD_IMAGE;
        for(uint32_t b = fs->volume.table_end; b < fs->volume.blocks; ++b)
            if(bool(fs->volume.allocated[b / 8] & (1u << (b % 8))) != bool(fs->volume.owners[b]))
                r = OS1_ERR_BAD_IMAGE;
    }
    if(r == 0 && find(static_cast<VfsNode*>(root), "disk", 4))
        r = OS1_ERR_EXISTS;
    if(r < 0)
    {
        object_release(mount);
        return r;
    }
    mount->next = static_cast<VfsNode*>(root)->children;
    static_cast<VfsNode*>(root)->children = mount;
    return 0;
}
