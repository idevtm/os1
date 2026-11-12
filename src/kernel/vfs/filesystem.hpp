#pragma once
#include "vfs/tmpfs.hpp"
struct VfsOperations
{
    long (*lookup)(VfsNode*, const char*, size_t, NativeObject*&);
    long (*create)(VfsNode*, const char*, size_t, uint32_t, OpenInstance*&);
    long (*remove)(VfsNode*, const char*, size_t, uint32_t);
    long (*rename)(VfsNode*, const char*, size_t, const char*, size_t);
    long (*read)(VfsNode*, uint64_t, void*, size_t);
    long (*write)(VfsNode*, uint64_t, const void*, size_t, bool, uint64_t*);
    long (*truncate)(VfsNode*, uint64_t);
    long (*seek)(VfsNode*, uint64_t, int64_t, uint32_t);
    long (*next)(VfsNode*, uint64_t, os1_directory_entry&, uint64_t&);
    os1_filesystem_info (*info)(VfsNode*);
    long (*sync)(VfsNode*);
    long (*snapshot)(VfsNode*, uint8_t*&, uint64_t&);
    long (*move)(VfsNode*, const char*, size_t, VfsNode*, const char*, size_t, bool, uint64_t);
    long (*metadata)(VfsNode*, os1_metadata&) = nullptr;
    long (*space)(VfsNode*, os1_space&) = nullptr;
    // Internal entry points: caller holds NamespaceGuard.
    long (*create_locked)(VfsNode*, const char*, size_t, uint32_t, OpenInstance*&) = nullptr;
    long (*remove_locked)(VfsNode*, const char*, size_t, uint32_t) = nullptr;
    long (*times)(VfsNode*, os1_file_times&) = nullptr;
    long (*set_times)(VfsNode*, uint32_t) = nullptr;
};
bool vfs_backed(const NativeObject*);
extern const VfsOperations tmpfs_operations;
