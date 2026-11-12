#pragma once

#include <stddef.h>
#include <stdint.h>

#include "fs/cpio_newc.hpp"
#include "handoff/boot_info.hpp"

// The initrd module is still a boot-time cpio archive owned by BootInfo.
// Keep the parsing surface small so later filesystem work can replace it
// without dragging CPIO details through unrelated kernel subsystems.

using InitrdFileVisitor = CpioNewcFileVisitor;

// Bind the owned BootInfo so later initrd queries can locate the module list.
void bind_initrd_boot_info(const BootInfo* boot_info);
// Visit every regular file in the cpio newc initrd.
bool for_each_initrd_file(InitrdFileVisitor visitor, void* context);
// Find one initrd entry by absolute path, returning a pointer into the archive.
bool find_initrd_file(const char* path, const uint8_t*& data, uint64_t& size);
// Normalize a cpio archive name into the kernel's absolute initrd path form.
void copy_initrd_path(char* destination, size_t destination_size, const char* archive_name);

struct NativeObject;
long initialize_initrd_namespace();
NativeObject* initrd_namespace_root();  // trusted bootstrap only, borrowed
#if defined(OS1_HOST_TEST)
struct BlockDevice;
long mount_ext2_root_for_test(BlockDevice& device);
void reset_initrd_namespace_for_test();
#endif
