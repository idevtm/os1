#pragma once
#include <os1/vfs.h>

#include "resource/descriptor.hpp"
constexpr uint32_t kDirectoryRights =
    OS1_RIGHT_LOOKUP | OS1_RIGHT_ENUMERATE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE;
constexpr uint32_t kFileRights =
    OS1_RIGHT_READ | OS1_RIGHT_SEEK | OS1_RIGHT_INSPECT | OS1_RIGHT_EXECUTE | OS1_RIGHT_DELEGATE;
constexpr uint32_t kInitrdCeiling = kDirectoryRights | kFileRights;
struct Tmpfs;
struct VfsNode : NativeObject
{
    Tmpfs* filesystem;
    uint64_t cookie;
    bool linked;
    VfsNode* children;
    VfsNode* next;
    const uint8_t* bytes;
    uint64_t byte_size;
    uint16_t name_size;
    bool explicit_entry;
    char name[OS1_NAME_MAX];
};
// On failure root is null and all unpublished nodes are released.
long vfs_import_initrd(const uint8_t* bytes, uint64_t size, NativeObject*& root);
long vfs_resolve(NativeObject* directory, const char* path, size_t size, NativeObject*& result);
long vfs_open(DescriptorTable&,
              int directory,
              const char* path,
              size_t bytes,
              uint32_t rights,
              uint32_t flags,
              uint32_t ceiling,
              OpenInstance*& result);
long vfs_open_descriptor(DescriptorTable&,
                         int directory,
                         const char*,
                         size_t,
                         uint32_t rights,
                         uint32_t flags,
                         uint32_t ceiling);
