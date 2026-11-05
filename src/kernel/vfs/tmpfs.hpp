#pragma once
#include <os1/tmpfs.h>

#include "vfs/initrd.hpp"
constexpr uint32_t kTmpDirectoryRights =
    kDirectoryRights | OS1_RIGHT_CREATE | OS1_RIGHT_REMOVE | OS1_RIGHT_RENAME;
constexpr uint32_t kTmpFileRights =
    OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_SEEK | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE;
constexpr uint32_t kNamespaceCeiling = kInitrdCeiling | kTmpDirectoryRights | kTmpFileRights;
// Bootstrap-only mutation of the unpublished namespace root. Reject name collisions.
long tmpfs_mount(NativeObject* root);
bool tmpfs_node(const NativeObject*);
long tmpfs_lookup(VfsNode*, const char*, size_t, NativeObject*&);
long tmpfs_create(VfsNode*, const char*, size_t, uint32_t kind, OpenInstance*&);
long tmpfs_remove(VfsNode*, const char*, size_t);
long tmpfs_rename(VfsNode*, const char*, size_t, const char*, size_t);
long tmpfs_read(VfsNode*, uint64_t offset, void*, size_t);
long tmpfs_write(VfsNode*, uint64_t offset, const void*, size_t);
long tmpfs_truncate(VfsNode*, uint64_t size);
long tmpfs_seek(VfsNode*, uint64_t cursor, int64_t offset, uint32_t origin);
long tmpfs_next(VfsNode*, uint64_t cursor, os1_directory_entry&, uint64_t& next);
os1_filesystem_info tmpfs_info(VfsNode*);
#ifdef OS1_HOST_TEST
extern int host_tmpfs_allocation_budget;
#endif
