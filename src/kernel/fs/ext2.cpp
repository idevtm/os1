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
    inode_write_block = 0;
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
    if(failed)
        return OS1_ERR_IO;

    const uint32_t block = table + (n - 1) / 32;
    if(inode_write_block != block && !read_block(block, inode_write_data))
    {
        inode_write_block = 0;
        return OS1_ERR_IO;
    }

    memcpy(inode_write_data + ((n - 1) % 32) * 128, inode.bytes, 128);
    // Retain only the last successfully written table block.
    if(!write_block(block, inode_write_data))
    {
        inode_write_block = 0;
        return OS1_ERR_IO;
    }
    inode_write_block = block;
    return 0;
}
long Volume::get_block(const Inode& inode, uint32_t index, uint32_t& b, bool* indirect_loaded)
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
        if(!indirect_loaded || !*indirect_loaded)
        {
            if(!read_block(indirect, meta))
                return OS1_ERR_IO;
            if(indirect_loaded)
                *indirect_loaded = true;
        }
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
    if(io.read_only)
        return OS1_ERR_ACCESS;
    if(failed)
        return OS1_ERR_IO;
    if(dirty)
        return 0;
    put16(super + 58, 0);  // Clear EXT2_VALID_FS without setting EXT2_ERROR_FS.
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
    bool indirect_loaded = false;
    while(done < length)
    {
        uint32_t b = 0;
        long r = get_block(inode, (offset + done) / 4096, b, &indirect_loaded);
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
bool Volume::stamp(Inode& inode, uint32_t fields)
{
    uint64_t now = 0;
    if(!io.clock || !io.clock(io.context, now) || !now || now / 1000000000ull > UINT32_MAX)
        return false;
    const uint32_t seconds = now / 1000000000ull;
    if(!seconds)
        return false;
    if(fields & OS1_TIME_ACCESS)
        put32(inode.bytes + 8, seconds);
    if(fields & OS1_TIME_MODIFY)
        put32(inode.bytes + 16, seconds);
    put32(inode.bytes + 12, seconds);
    return true;
}
long Volume::set_times(uint32_t number, Inode& inode, uint32_t fields)
{
    if(io.read_only)
        return OS1_ERR_ACCESS;
    if(!fields || fields & ~(OS1_TIME_ACCESS | OS1_TIME_MODIFY))
        return OS1_ERR_INVALID;
    Inode changed = inode;
    if(!stamp(changed, fields))
        return OS1_ERR_UNSUPPORTED;
    long r = begin_write();
    if(r < 0)
        return r;
    r = inode_write(number, changed);
    if(r == 0)
        inode = changed;
    return r;
}
long Volume::write(uint32_t n, Inode& inode, uint64_t offset, const void* in, size_t length)
{
    if(offset > max_file_bytes || length > max_file_bytes - offset)
        return OS1_ERR_RANGE;
    if(!length)
        return 0;
    // Reserve sufficient capacity before changing a bounded native write.
    uint32_t needed = 0;
    uint32_t first_block = 0;
    const uint32_t first_index = offset / block_bytes;
    for(uint32_t i = first_index; i <= (offset + length - 1) / block_bytes; ++i)
    {
        uint32_t b = 0;
        long r = get_block(inode, i, b);
        if(r < 0)
            return r;
        if(i == first_index)
            first_block = b;
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
        uint32_t b = done == 0 ? first_block : 0;
        if(!b)
        {
            r = ensure_block(n, inode, (offset + done) / 4096, b);
            if(r < 0)
                return r;
        }
        size_t at = (offset + done) % 4096, chunk = 4096 - at;
        if(chunk > length - done)
            chunk = length - done;
        const void* block = src + done;
        if(chunk != block_bytes)
        {
            if(!read_block(b, data))
                return OS1_ERR_IO;
            memcpy(data + at, src + done, chunk);
            block = data;
        }
        if(!write_block(b, block))
            return OS1_ERR_IO;
        done += chunk;
    }
    if(offset + length > size(inode))
        put32(inode.bytes + 4, offset + length);
    stamp(inode, OS1_TIME_MODIFY);
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
    stamp(inode, OS1_TIME_MODIFY);
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
    stamp(inode, OS1_TIME_ACCESS | OS1_TIME_MODIFY);
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
    }
    if((stamp(parent, OS1_TIME_MODIFY) || dir) && inode_write(parent_number, parent) < 0)
        return OS1_ERR_IO;
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
    }
    if((stamp(parent, OS1_TIME_MODIFY) || dir) && inode_write(parent_number, parent) < 0)
        return OS1_ERR_IO;
    if(!write_block(inode_bitmap, inode_bits))
        return OS1_ERR_IO;
    return counters();
}
long Volume::rename(uint32_t parent_number,
                    Inode& parent,
                    uint32_t number,
                    Inode& inode,
                    const char* from,
                    size_t from_len,
                    const char* to,
                    size_t to_len)
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
    if(!write_block(b, data))
        return OS1_ERR_IO;
    if(stamp(parent, OS1_TIME_MODIFY) && inode_write(parent_number, parent) < 0)
        return OS1_ERR_IO;
    if(stamp(inode, 0) && inode_write(number, inode) < 0)
        return OS1_ERR_IO;
    return 0;
}

namespace
{
bool erase_entry(uint8_t* block, const char* name, size_t bytes, uint32_t number)
{
    for(unsigned at = 0; at < block_bytes; at += u16(block + at + 4))
        if(u32(block + at) == number && block[at + 6] == bytes &&
           equal(reinterpret_cast<char*>(block + at + 8), name, bytes))
        {
            put32(block + at, 0);
            return true;
        }
    return false;
}
// Validated directory records only. Compact in place; output never overtakes input.
unsigned compact(uint8_t* block)
{
    unsigned output = 0, last = 0;
    for(unsigned at = 0; at < block_bytes;)
    {
        const unsigned record = u16(block + at + 4);
        if(u32(block + at))
        {
            const unsigned size = record_size(block[at + 6]);
            for(unsigned i = 0; i < size; ++i)
                block[output + i] = block[at + i];
            put16(block + output + 4, size);
            last = output;
            output += size;
        }
        at += record;
    }
    if(output)
        put16(block + last + 4, block_bytes - last);
    else
    {
        memset(block, 0, block_bytes);
        put16(block + 4, block_bytes);
    }
    return output;
}
bool insert_entry(uint8_t* block, const char* name, size_t bytes, uint32_t number, bool directory)
{
    const unsigned need = record_size(bytes);
    for(unsigned at = 0; at < block_bytes; at += u16(block + at + 4))
    {
        unsigned record = u16(block + at + 4),
                 used = u32(block + at) ? record_size(block[at + 6]) : 0;
        if(record - used < need)
            continue;
        if(used)
        {
            put16(block + at + 4, used);
            at += used;
            record -= used;
        }
        memset(block + at, 0, record);
        put32(block + at, number);
        put16(block + at + 4, record);
        block[at + 6] = bytes;
        block[at + 7] = directory ? 2 : 1;
        memcpy(block + at + 8, name, bytes);
        return true;
    }
    return false;
}
}  // namespace
long Volume::move(uint32_t source_parent,
                  Inode& source_directory,
                  const char* source,
                  size_t a,
                  uint32_t destination_parent,
                  Inode& destination_directory,
                  const char* destination,
                  size_t b,
                  uint32_t number,
                  Inode& inode,
                  uint32_t replaced,
                  Inode* replaced_inode,
                  uint8_t* scratch)
{
    uint32_t from_block = 0, to_block = 0;
    long r = get_block(source_directory, 0, from_block);
    if(r < 0)
        return r;
    r = get_block(destination_directory, 0, to_block);
    if(r < 0)
        return r;
    auto* from = scratch;
    auto* to = from_block == to_block ? scratch : scratch + block_bytes;
    if(!read_block(from_block, from) || (to != from && !read_block(to_block, to)))
        return OS1_ERR_IO;
    if(!erase_entry(from, source, a, number) ||
       (replaced && !erase_entry(to, destination, b, replaced)))
        return OS1_ERR_NOT_FOUND;
    compact(from);
    if(to != from)
        compact(to);
    if(!insert_entry(to, destination, b, number, directory(inode)))
        return OS1_ERR_LIMIT;
    r = begin_write();
    if(r < 0)
        return r;
    // Flush the destination before removing the source entry.
    if(to != from && (!write_block(to_block, to) || !io.flush(io.context)))
    {
        failed = true;
        return OS1_ERR_IO;
    }
    if(!write_block(from_block, from))
        return OS1_ERR_IO;
    if(directory(inode) && source_parent != destination_parent)
    {
        uint32_t block = 0;
        if(get_block(inode, 0, block) < 0 || !read_block(block, data))
        {
            failed = true;
            return OS1_ERR_IO;
        }
        bool found = false;
        for(unsigned at = 0; at < block_bytes; at += u16(data + at + 4))
            if(data[at + 6] == 2 && data[at + 8] == '.' && data[at + 9] == '.')
            {
                put32(data + at, destination_parent);
                found = true;
                break;
            }
        if(!found || !write_block(block, data))
        {
            failed = true;
            return OS1_ERR_IO;
        }
        put16(source_directory.bytes + 26, u16(source_directory.bytes + 26) - 1);
        put16(destination_directory.bytes + 26, u16(destination_directory.bytes + 26) + 1);
    }
    if(replaced)
    {
        const bool dir = directory(*replaced_inode);
        if(truncate(replaced, *replaced_inode, 0) < 0)
        {
            failed = true;
            return OS1_ERR_IO;
        }
        *replaced_inode = {};
        if(inode_write(replaced, *replaced_inode) < 0)
            return OS1_ERR_IO;
        set(inode_bits, replaced - 1, false);
        put32(super + 16, u32(super + 16) + 1);
        put16(group + 14, u16(group + 14) + 1);
        if(dir)
        {
            put16(group + 16, u16(group + 16) - 1);
            put16(destination_directory.bytes + 26, u16(destination_directory.bytes + 26) - 1);
        }
        if(!write_block(inode_bitmap, inode_bits) || counters() < 0)
            return OS1_ERR_IO;
    }
    stamp(source_directory, OS1_TIME_MODIFY);
    stamp(destination_directory, OS1_TIME_MODIFY);
    if(stamp(inode, 0) && inode_write(number, inode) < 0)
        return OS1_ERR_IO;
    if(inode_write(source_parent, source_directory) < 0 ||
       (source_parent != destination_parent &&
        inode_write(destination_parent, destination_directory) < 0))
        return OS1_ERR_IO;
    return 0;
}
long Volume::sync()
{
    if(io.read_only)
        return failed ? OS1_ERR_IO : 0;
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
