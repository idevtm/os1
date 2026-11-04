#pragma once

#include <stddef.h>
#include <stdint.h>

// Pure CPIO "newc" archive traversal used by the initrd wrapper and host
// tests. The parser owns only archive-format rules; BootInfo/module lifetime is
// handled by fs/initrd.cpp.

using CpioNewcFileVisitor = bool (*)(const char* archive_name,
                                     const uint8_t* file_data,
                                     uint64_t file_size,
                                     void* context);

[[nodiscard]] bool for_each_cpio_newc_file(const uint8_t* archive,
                                           uint64_t archive_size,
                                           CpioNewcFileVisitor visitor,
                                           void* context);

[[nodiscard]] bool cpio_newc_paths_equal(const char* archive_name, const char* wanted);

void copy_cpio_newc_path(char* destination, size_t destination_size, const char* archive_name);

// Strict whole-archive walker used for namespace import. Includes directories;
// rejects malformed fields, embedded NUL, unsupported links/types, and bad trailer.
using CpioNewcEntryVisitor =
    bool (*)(const char*, size_t, uint32_t, const uint8_t*, uint64_t, void*);
[[nodiscard]] bool for_each_cpio_newc_entry(const uint8_t*, uint64_t, CpioNewcEntryVisitor, void*);
