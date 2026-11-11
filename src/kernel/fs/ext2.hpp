#pragma once
#include <os1/tmpfs.h>
#include <stddef.h>
#include <stdint.h>

namespace ext2
{
constexpr uint32_t block_bytes = 4096;
constexpr uint32_t max_blocks = 32768;
constexpr uint32_t max_inodes = 1024;
constexpr uint32_t max_file_bytes = (12 + 1024) * block_bytes;
struct Io
{
    void* context;
    bool (*read)(void*, uint32_t, void*);
    bool (*write)(void*, uint32_t, const void*);
    bool (*flush)(void*);
    uint32_t blocks;
    bool (*clock)(void*, uint64_t&) = nullptr;
    bool read_only = false;
};
struct Inode
{
    uint8_t bytes[128];
};
struct Entry
{
    uint32_t inode;
    uint8_t type;
    uint8_t length;
    char name[64];
};
// One validated block group. Callers serialize all access, including read buffers.
struct Volume
{
    Io io{};
    uint32_t blocks{}, inodes{}, table{}, table_end{}, block_bitmap{}, inode_bitmap{};
    bool failed{}, dirty{};
    uint8_t super[1024]{}, group[32]{}, allocated[4096]{}, inode_bits[4096]{};
    uint16_t owners[max_blocks]{};
    uint8_t data[4096]{}, meta[4096]{};
    long mount(Io source);
    long inode_read(uint32_t number, Inode& inode);
    long inode_write(uint32_t number, const Inode& inode);
    long validate_inode(uint32_t number, const Inode& inode);
    long read(Inode&, uint64_t offset, void*, size_t);
    long write(uint32_t number, Inode&, uint64_t offset, const void*, size_t);
    long truncate(uint32_t number, Inode&, uint64_t size);
    long next(Inode&, uint64_t offset, Entry&, uint64_t& next);
    long create(uint32_t parent_number,
                Inode& parent,
                const char*,
                size_t,
                bool directory,
                uint32_t& number,
                Inode& inode);
    long remove(
        uint32_t parent_number, Inode& parent, const char*, size_t, uint32_t number, Inode& inode);
    long rename(uint32_t parent_number,
                Inode& parent,
                uint32_t number,
                Inode& inode,
                const char*,
                size_t,
                const char*,
                size_t);
    long move(uint32_t source_parent,
              Inode& source_directory,
              const char* source,
              size_t source_bytes,
              uint32_t destination_parent,
              Inode& destination_directory,
              const char* destination,
              size_t destination_bytes,
              uint32_t number,
              Inode& inode,
              uint32_t replaced,
              Inode* replaced_inode,
              uint8_t* scratch);
    bool stamp(Inode&, uint32_t fields);
    long set_times(uint32_t, Inode&, uint32_t fields);
    long sync();
    long begin_write();
    long get_block(const Inode&, uint32_t index, uint32_t& result);
    long ensure_block(uint32_t number, Inode&, uint32_t index, uint32_t& result);
    long allocate(uint32_t number, uint32_t& result);
    long free_block(uint32_t block);
    long counters();
    long zero_tail(Inode&, uint64_t end);
    bool read_block(uint32_t block, void* out);
    bool write_block(uint32_t block, const void* in);
};
uint16_t u16(const uint8_t*);
uint32_t u32(const uint8_t*);
void put16(uint8_t*, uint16_t);
void put32(uint8_t*, uint32_t);
inline uint32_t size(const Inode& inode)
{
    return u32(inode.bytes + 4);
}
inline bool directory(const Inode& inode)
{
    return (u16(inode.bytes) & 0xf000) == 0x4000;
}
}  // namespace ext2
