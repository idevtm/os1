#pragma once
#include "vfs/tmpfs.hpp"
struct VfsOperations
{
    long (*lookup)(VfsNode*, const char*, size_t, NativeObject*&);
    long (*create)(VfsNode*, const char*, size_t, uint32_t, OpenInstance*&);
    long (*remove)(VfsNode*, const char*, size_t);
    long (*rename)(VfsNode*, const char*, size_t, const char*, size_t);
    long (*read)(VfsNode*, uint64_t, void*, size_t);
    long (*write)(VfsNode*, uint64_t, const void*, size_t, bool, uint64_t*);
    long (*truncate)(VfsNode*, uint64_t);
    long (*seek)(VfsNode*, uint64_t, int64_t, uint32_t);
    long (*next)(VfsNode*, uint64_t, os1_directory_entry&, uint64_t&);
    os1_filesystem_info (*info)(VfsNode*);
    long (*sync)(VfsNode*);
    long (*snapshot)(VfsNode*, uint8_t*&, uint64_t&);
};
bool vfs_backed(const NativeObject*);
extern const VfsOperations tmpfs_operations;
