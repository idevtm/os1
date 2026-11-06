#include "fs/ext2.hpp"

#include "util/memory.h"
namespace ext2
{
uint16_t u16(const uint8_t* p)
{
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
uint32_t u32(const uint8_t* p)
{
    return uint32_t(u16(p)) | uint32_t(u16(p + 2)) << 16;
}
void put16(uint8_t* p, uint16_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}
void put32(uint8_t* p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}
namespace
{
bool bit(const uint8_t* p, uint32_t n)
{
    return p[n / 8] & (1u << (n % 8));
}
void set(uint8_t* p, uint32_t n, bool value)
{
    if(value)
        p[n / 8] |= 1u << (n % 8);
    else
        p[n / 8] &= ~(1u << (n % 8));
}
bool equal(const char* a, const char* b, size_t n)
{
    for(size_t i = 0; i < n; ++i)
        if(a[i] != b[i])
            return false;
    return true;
}
uint32_t record_size(size_t name)
{
    return (8 + name + 3) & ~3u;
}
}  // namespace
bool Volume::read_block(uint32_t b, void* out)
{
    if(failed || b >= blocks || !io.read(io.context, b, out))
    {
        failed = true;
        return false;
    }
    return true;
}
bool Volume::write_block(uint32_t b, const void* in)
{
    if(failed || b >= blocks || !io.write(io.context, b, in))
    {
        failed = true;
        return false;
    }
    return true;
}
long Volume::mount(Io source)
{
    io = source;
    blocks = source.blocks;
    if(!io.read || !io.write || !io.flush || blocks < 128 || blocks > max_blocks)
        return OS1_ERR_BAD_IMAGE;
    if(!read_block(0, data))
        return OS1_ERR_IO;
    memcpy(super, data + 1024, 1024);
    blocks = u32(super + 4);
    inodes = u32(super);
    // Revision 1, 4 KiB blocks, one group, 128-byte inodes, FILETYPE only.
    if(u16(super + 56) != 0xef53 || u16(super + 58) != 1 || u32(super + 76) != 1 ||
       u32(super + 24) != 2 || u32(super + 28) != 2 || u32(super + 20) != 0 || blocks < 128 ||
       blocks > source.blocks || blocks > max_blocks || !inodes || inodes > max_inodes ||
       inodes % 32 || u32(super + 32) != 32768 || u32(super + 36) != 32768 ||
       u32(super + 40) != inodes || u16(super + 88) != 128 || u32(super + 84) != 11 ||
       u32(super + 92) != 0 || u32(super + 96) != 2 || u32(super + 100) != 0)
        return OS1_ERR_BAD_IMAGE;
    if(!read_block(1, data))
        return OS1_ERR_IO;
    memcpy(group, data, 32);
    block_bitmap = u32(group);
    inode_bitmap = u32(group + 4);
    table = u32(group + 8);
    table_end = table + inodes / 32;
    // Fixed metadata placement also prevents metadata/data aliasing.
    if(block_bitmap != 2 || inode_bitmap != 3 || table != 4 || table_end >= blocks ||
       u32(super + 12) > blocks || u32(super + 16) > inodes || u16(group + 12) != u32(super + 12) ||
       u16(group + 14) != u32(super + 16))
        return OS1_ERR_BAD_IMAGE;
    if(!read_block(2, allocated) || !read_block(3, inode_bits))
        return OS1_ERR_IO;
    uint32_t free_blocks = 0, free_inodes = 0;
    for(uint32_t b = 0; b < 32768; ++b)
    {
        if((b < table_end || b >= blocks) && !bit(allocated, b))
            return OS1_ERR_BAD_IMAGE;
        if(b < blocks && !bit(allocated, b))
            ++free_blocks;
    }
    for(uint32_t n = 0; n < 32768; ++n)
    {
        if((n < 10 || n >= inodes) && !bit(inode_bits, n))
            return OS1_ERR_BAD_IMAGE;
        if(n < inodes && !bit(inode_bits, n))
            ++free_inodes;
    }
    if(free_blocks != u32(super + 12) || free_inodes != u32(super + 16))
        return OS1_ERR_BAD_IMAGE;
    return 0;
}
long Volume::inode_read(uint32_t n, Inode& inode)
{
    if(!n || n > inodes || !bit(inode_bits, n - 1))
        return OS1_ERR_BAD_IMAGE;
    if(!read_block(table + (n - 1) / 32, meta))
        return OS1_ERR_IO;
    memcpy(inode.bytes, meta + ((n - 1) % 32) * 128, 128);
    return 0;
}
long Volume::inode_write(uint32_t n, const Inode& inode)
{
    if(!n || n > inodes)
        return OS1_ERR_BAD_IMAGE;
    if(!read_block(table + (n - 1) / 32, meta))
        return OS1_ERR_IO;
    memcpy(meta + ((n - 1) % 32) * 128, inode.bytes, 128);
    return write_block(table + (n - 1) / 32, meta) ? 0 : OS1_ERR_IO;
}
long Volume::get_block(const Inode& inode, uint32_t index, uint32_t& b)
{
    if(index >= 12 + 1024)
        return OS1_ERR_RANGE;
    if(index < 12)
        b = u32(inode.bytes + 40 + index * 4);
    else
    {
        uint32_t indirect = u32(inode.bytes + 88);
        if(!indirect)
        {
            b = 0;
            return 0;
        }
        if(indirect < table_end || indirect >= blocks || !bit(allocated, indirect))
            return OS1_ERR_BAD_IMAGE;
        if(!read_block(indirect, meta))
            return OS1_ERR_IO;
        b = u32(meta + (index - 12) * 4);
    }
    if(b && (b < table_end || b >= blocks || !bit(allocated, b)))
        return OS1_ERR_BAD_IMAGE;
    return 0;
}
long Volume::validate_inode(uint32_t n, const Inode& inode)
{
    uint16_t type = u16(inode.bytes) & 0xf000;
    if((type != 0x4000 && type != 0x8000) || size(inode) > max_file_bytes ||
       u32(inode.bytes + 32) || u32(inode.bytes + 92) || u32(inode.bytes + 96) ||
       u32(inode.bytes + 104) || u32(inode.bytes + 108) ||
       (type == 0x8000 && u16(inode.bytes + 26) != 1) ||
       (type == 0x4000 && (size(inode) != block_bytes || u16(inode.bytes + 26) < 2)))
        return OS1_ERR_BAD_IMAGE;
    uint32_t used = 0;
    uint32_t indirect = u32(inode.bytes + 88);
    if(indirect)
    {
        if(indirect < table_end || indirect >= blocks || !bit(allocated, indirect) ||
           owners[indirect])
            return OS1_ERR_BAD_IMAGE;
        owners[indirect] = n;
        ++used;
    }
    if(indirect && !read_block(indirect, meta))
        return OS1_ERR_IO;
    for(uint32_t i = 0; i < 12 + 1024; ++i)
    {
        uint32_t b = i < 12     ? u32(inode.bytes + 40 + i * 4)
                     : indirect ? u32(meta + (i - 12) * 4)
                                : 0;
        if(!b)
            continue;
        if(b < table_end || b >= blocks || !bit(allocated, b) || i >= (size(inode) + 4095) / 4096 ||
           owners[b])
            return OS1_ERR_BAD_IMAGE;
        owners[b] = n;
        ++used;
    }
    return used * 8 == u32(inode.bytes + 28) ? 0 : OS1_ERR_BAD_IMAGE;
}
long Volume::begin_write()
{
    if(failed)
        return OS1_ERR_IO;
    if(dirty)
        return 0;
    put16(super + 58, 2);  // Mark unclean before the first metadata/data mutation.
    if(!read_block(0, meta))
        return OS1_ERR_IO;
    memcpy(meta + 1024, super, 1024);
    if(!write_block(0, meta) || !io.flush(io.context))
    {
        failed = true;
        return OS1_ERR_IO;
    }
    dirty = true;
    return 0;
}
long Volume::counters()
{
    if(!read_block(1, meta))
        return OS1_ERR_IO;
    memcpy(meta, group, 32);
    if(!write_block(1, meta) || !read_block(0, meta))
        return OS1_ERR_IO;
    memcpy(meta + 1024, super, 1024);
    return write_block(0, meta) ? 0 : OS1_ERR_IO;
}
long Volume::allocate(uint32_t owner, uint32_t& out)
{
    for(uint32_t b = table_end; b < blocks; ++b)
    {
        if(bit(allocated, b))
            continue;
        set(allocated, b, true);
        owners[b] = owner;
        put32(super + 12, u32(super + 12) - 1);
        put16(group + 12, u16(group + 12) - 1);
        if(!write_block(block_bitmap, allocated) || counters() < 0)
            return OS1_ERR_IO;
        memset(meta, 0, sizeof(meta));
        if(!write_block(b, meta))
            return OS1_ERR_IO;
        out = b;
        return 0;
    }
    return OS1_ERR_LIMIT;
}
long Volume::free_block(uint32_t b)
{
    if(b < table_end || b >= blocks || !bit(allocated, b))
    {
        failed = true;
        return OS1_ERR_BAD_IMAGE;
    }
    set(allocated, b, false);
    owners[b] = 0;
    put32(super + 12, u32(super + 12) + 1);
    put16(group + 12, u16(group + 12) + 1);
    if(!write_block(block_bitmap, allocated))
        return OS1_ERR_IO;
    return counters();
}
long Volume::ensure_block(uint32_t n, Inode& inode, uint32_t index, uint32_t& b)
{
    long r = get_block(inode, index, b);
    if(r < 0 || b)
        return r;
    if(index >= 12 && !u32(inode.bytes + 88))
    {
        uint32_t indirect = 0;
        r = allocate(n, indirect);
        if(r < 0)
            return r;
        put32(inode.bytes + 88, indirect);
        put32(inode.bytes + 28, u32(inode.bytes + 28) + 8);
        r = inode_write(n, inode);
        if(r < 0)
            return r;
    }
    r = allocate(n, b);
    if(r < 0)
        return r;
    if(index < 12)
        put32(inode.bytes + 40 + index * 4, b);
    else
    {
        uint32_t indirect = u32(inode.bytes + 88);
        if(!read_block(indirect, meta))
            return OS1_ERR_IO;
        put32(meta + (index - 12) * 4, b);
        if(!write_block(indirect, meta))
            return OS1_ERR_IO;
    }
    put32(inode.bytes + 28, u32(inode.bytes + 28) + 8);
    return inode_write(n, inode);
}
long Volume::read(Inode& inode, uint64_t offset, void* out, size_t length)
{
    if(failed)
        return OS1_ERR_IO;
    if(offset >= size(inode))
        return 0;
    if(length > size(inode) - offset)
        length = size(inode) - offset;
    auto* dst = static_cast<uint8_t*>(out);
    size_t done = 0;
    while(done < length)
    {
        uint32_t b = 0;
        long r = get_block(inode, (offset + done) / 4096, b);
        if(r < 0)
            return r;
        size_t at = (offset + done) % 4096, chunk = 4096 - at;
        if(chunk > length - done)
            chunk = length - done;
        if(b)
        {
            if(!read_block(b, data))
                return OS1_ERR_IO;
            memcpy(dst + done, data + at, chunk);
        }
        else
            memset(dst + done, 0, chunk);
        done += chunk;
    }
    return done;
}
long Volume::zero_tail(Inode& inode, uint64_t end)
{
    const uint32_t old = size(inode);
    if(end <= old || !(old % 4096))
        return 0;
    uint32_t block = 0;
    long result = get_block(inode, old / 4096, block);
    if(result < 0 || !block)
        return result;
    if(!read_block(block, data))
        return OS1_ERR_IO;
    uint64_t count = end - old;
    if(count > 4096 - old % 4096)
        count = 4096 - old % 4096;
    memset(data + old % 4096, 0, count);
    return write_block(block, data) ? 0 : OS1_ERR_IO;
}
long Volume::write(uint32_t n, Inode& inode, uint64_t offset, const void* in, size_t length)
{
    if(offset > max_file_bytes || length > max_file_bytes - offset)
        return OS1_ERR_RANGE;
    if(!length)
        return 0;
    // Reserve sufficient capacity before changing a bounded native write.
    uint32_t needed = 0;
    for(uint32_t i = offset / 4096; i <= (offset + length - 1) / 4096; ++i)
    {
        uint32_t b = 0;
        long r = get_block(inode, i, b);
        if(r < 0)
            return r;
        if(!b)
            ++needed;
    }
    if((offset + length - 1) / 4096 >= 12 && !u32(inode.bytes + 88))
        ++needed;
    if(needed > u32(super + 12))
        return OS1_ERR_LIMIT;
    long r = begin_write();
    if(r < 0)
        return r;
    r = zero_tail(inode, offset);
    if(r < 0)
        return r;
    auto* src = static_cast<const uint8_t*>(in);
    size_t done = 0;
    while(done < length)
    {
        uint32_t b = 0;
        r = ensure_block(n, inode, (offset + done) / 4096, b);
        if(r < 0)
            return r;
        size_t at = (offset + done) % 4096, chunk = 4096 - at;
        if(chunk > length - done)
            chunk = length - done;
        if(!read_block(b, data))
            return OS1_ERR_IO;
        memcpy(data + at, src + done, chunk);
        if(!write_block(b, data))
            return OS1_ERR_IO;
        done += chunk;
    }
    if(offset + length > size(inode))
        put32(inode.bytes + 4, offset + length);
    r = inode_write(n, inode);
    return r < 0 ? r : static_cast<long>(done);
}
long Volume::truncate(uint32_t n, Inode& inode, uint64_t length)
{
    if(length > max_file_bytes)
        return OS1_ERR_RANGE;
    long r = begin_write();
    if(r < 0)
        return r;
    const uint32_t old = size(inode);
    r = zero_tail(inode, length);
    if(r < 0)
        return r;
    if(length < old)
    {
        // Clear retained tails so later sparse growth cannot expose stale bytes.
        if(length % 4096)
        {
            uint32_t b = 0;
            r = get_block(inode, length / 4096, b);
            if(r < 0)
                return r;
            if(b)
            {
                if(!read_block(b, data))
                    return OS1_ERR_IO;
                memset(data + length % 4096, 0, 4096 - length % 4096);
                if(!write_block(b, data))
                    return OS1_ERR_IO;
            }
        }
        for(uint32_t i = (length + 4095) / 4096; i < (old + 4095) / 4096; ++i)
        {
            uint32_t b = 0;
            r = get_block(inode, i, b);
            if(r < 0)
                return r;
            if(!b)
                continue;
            if(i < 12)
                put32(inode.bytes + 40 + i * 4, 0);
            else
            {
                uint32_t indirect = u32(inode.bytes + 88);
                if(!read_block(indirect, meta))
                    return OS1_ERR_IO;
                put32(meta + (i - 12) * 4, 0);
                if(!write_block(indirect, meta))
                    return OS1_ERR_IO;
            }
            put32(inode.bytes + 28, u32(inode.bytes + 28) - 8);
            if(inode_write(n, inode) < 0 || free_block(b) < 0)
                return OS1_ERR_IO;
        }
        if(length <= 12 * 4096 && u32(inode.bytes + 88))
        {
            uint32_t b = u32(inode.bytes + 88);
            put32(inode.bytes + 88, 0);
            put32(inode.bytes + 28, u32(inode.bytes + 28) - 8);
            if(inode_write(n, inode) < 0 || free_block(b) < 0)
                return OS1_ERR_IO;
        }
    }
    put32(inode.bytes + 4, length);
    return inode_write(n, inode);
}
long Volume::next(Inode& inode, uint64_t offset, Entry& entry, uint64_t& next_offset)
{
    if(!directory(inode) || size(inode) != 4096 || offset > 4096)
        return OS1_ERR_BAD_IMAGE;
    uint32_t b = 0;
    long r = get_block(inode, 0, b);
    if(r < 0 || !b)
        return r < 0 ? r : OS1_ERR_BAD_IMAGE;
    if(!read_block(b, data))
        return OS1_ERR_IO;
    while(offset < 4096)
    {
        if(4096 - offset < 8)
            return OS1_ERR_BAD_IMAGE;
        auto* p = data + offset;
        uint16_t rec = u16(p + 4);
        uint8_t len = p[6];
        if(rec < 8 || rec % 4 || rec > 4096 - offset || len > rec - 8)
            return OS1_ERR_BAD_IMAGE;
        offset += rec;
        if(!u32(p))
            continue;
        if(!len || len > 63 || u32(p) > inodes || (p[7] != 1 && p[7] != 2))
            return OS1_ERR_BAD_IMAGE;
        for(unsigned i = 0; i < len; ++i)
            if(!p[8 + i] || p[8 + i] == '/')
                return OS1_ERR_BAD_IMAGE;
        entry = {};
        entry.inode = u32(p);
        entry.type = p[7];
        entry.length = len;
        memcpy(entry.name, p + 8, len);
        next_offset = offset;
        return 1;
    }
    return 0;
}
long Volume::create(uint32_t parent_number,
                    Inode& parent,
                    const char* name,
                    size_t len,
                    bool dir,
                    uint32_t& n,
                    Inode& inode)
{
    if(!u32(super + 16) || (dir && !u32(super + 12)))
        return OS1_ERR_LIMIT;
    uint32_t b = 0;
    long r = get_block(parent, 0, b);
    if(r < 0)
        return r;
    if(!read_block(b, data))
        return OS1_ERR_IO;
    uint32_t at = 0, need = record_size(len);
    for(; at < 4096; at += u16(data + at + 4))
    {
        uint32_t used = u32(data + at) ? record_size(data[at + 6]) : 0;
        if(u16(data + at + 4) - used >= need)
            break;
    }
    if(at == 4096)
        return OS1_ERR_LIMIT;
    r = begin_write();
    if(r < 0)
        return r;
    for(n = 11; n <= inodes && bit(inode_bits, n - 1); ++n)
    {
    }
    if(n > inodes)
        return OS1_ERR_LIMIT;
    set(inode_bits, n - 1, true);
    put32(super + 16, u32(super + 16) - 1);
    put16(group + 14, u16(group + 14) - 1);
    if(dir)
        put16(group + 16, u16(group + 16) + 1);
    if(!write_block(inode_bitmap, inode_bits) || counters() < 0)
        return OS1_ERR_IO;
    inode = {};
    put16(inode.bytes, dir ? 0x41ed : 0x81a4);
    put16(inode.bytes + 26, dir ? 2 : 1);
    if(dir)
    {
        uint32_t child = 0;
        r = ensure_block(n, inode, 0, child);
        if(r < 0)
            return r;
        memset(data, 0, 4096);
        put32(data, n);
        put16(data + 4, 12);
        data[6] = 1;
        data[7] = 2;
        data[8] = '.';
        put32(data + 12, parent_number);
        put16(data + 16, 4084);
        data[18] = 2;
        data[19] = 2;
        data[20] = '.';
        data[21] = '.';
        if(!write_block(child, data))
            return OS1_ERR_IO;
        put32(inode.bytes + 4, 4096);
        put16(parent.bytes + 26, u16(parent.bytes + 26) + 1);
        if(inode_write(parent_number, parent) < 0)
            return OS1_ERR_IO;
    }
    if(inode_write(n, inode) < 0 || !read_block(b, data))
        return OS1_ERR_IO;
    uint32_t rec = u16(data + at + 4), used = u32(data + at) ? record_size(data[at + 6]) : 0;
    if(used)
    {
        put16(data + at + 4, used);
        at += used;
        rec -= used;
    }
    memset(data + at, 0, rec);
    put32(data + at, n);
    put16(data + at + 4, rec);
    data[at + 6] = len;
    data[at + 7] = dir ? 2 : 1;
    memcpy(data + at + 8, name, len);
    return write_block(b, data) ? 0 : OS1_ERR_IO;
}
long Volume::remove(
    uint32_t parent_number, Inode& parent, const char* name, size_t len, uint32_t n, Inode& inode)
{
    uint32_t b = 0;
    long r = get_block(parent, 0, b);
    if(r < 0)
        return r;
    if(!read_block(b, data))
        return OS1_ERR_IO;
    uint32_t at = 0;
    for(; at < 4096; at += u16(data + at + 4))
        if(u32(data + at) == n && data[at + 6] == len &&
           equal(reinterpret_cast<char*>(data + at + 8), name, len))
            break;
    if(at == 4096)
        return OS1_ERR_NOT_FOUND;
    r = begin_write();
    if(r < 0)
        return r;
    // begin_write uses meta, preserving this directory buffer.
    put32(data + at, 0);
    if(!write_block(b, data))
        return OS1_ERR_IO;
    const bool dir = directory(inode);
    r = truncate(n, inode, 0);
    if(r < 0)
        return r;
    inode = {};
    if(inode_write(n, inode) < 0)
        return OS1_ERR_IO;
    set(inode_bits, n - 1, false);
    put32(super + 16, u32(super + 16) + 1);
    put16(group + 14, u16(group + 14) + 1);
    if(dir)
    {
        put16(group + 16, u16(group + 16) - 1);
        put16(parent.bytes + 26, u16(parent.bytes + 26) - 1);
        if(inode_write(parent_number, parent) < 0)
            return OS1_ERR_IO;
    }
    if(!write_block(inode_bitmap, inode_bits))
        return OS1_ERR_IO;
    return counters();
}
long Volume::rename(Inode& parent, const char* from, size_t from_len, const char* to, size_t to_len)
{
    uint32_t b = 0;
    long r = get_block(parent, 0, b);
    if(r < 0)
        return r;
    if(!read_block(b, data))
        return OS1_ERR_IO;
    uint32_t at = 0;
    for(; at < 4096; at += u16(data + at + 4))
        if(u32(data + at) && data[at + 6] == from_len &&
           equal(reinterpret_cast<char*>(data + at + 8), from, from_len))
            break;
    if(at == 4096)
        return OS1_ERR_NOT_FOUND;
    // Bounded first profile: rename cannot expand the record into adjacent entries.
    if(record_size(to_len) > u16(data + at + 4))
        return OS1_ERR_LIMIT;
    r = begin_write();
    if(r < 0)
        return r;
    memset(data + at + 8, 0, u16(data + at + 4) - 8);
    memcpy(data + at + 8, to, to_len);
    data[at + 6] = to_len;
    return write_block(b, data) ? 0 : OS1_ERR_IO;
}
long Volume::sync()
{
    if(failed || !io.flush(io.context))
    {
        failed = true;
        return OS1_ERR_IO;
    }
    if(!dirty)
        return 0;
    if(!read_block(0, meta))
        return OS1_ERR_IO;
    put16(super + 58, 1);
    memcpy(meta + 1024, super, 1024);
    if(!write_block(0, meta) || !io.flush(io.context))
    {
        failed = true;
        return OS1_ERR_IO;
    }
    dirty = false;
    return 0;
}
}  // namespace ext2
