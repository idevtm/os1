// Internal module ABI. Fixed-width C records; no kernel C++ objects cross it.
#pragma once
#include <stdint.h>

#define OS1_DRIVER_ABI 1u
#define OS1_DRIVER_MODULE_BASE UINT64_C(0xffffffffa0000000)
#define OS1_DRIVER_MODULE_BYTES UINT64_C(0x20000)
#define OS1_DRIVER_FILE_LIMIT UINT64_C(0x20000)

struct os1_driver_host
{
    uint32_t version, bytes;
    void* (*open)(uint32_t pci_index, uint32_t config_bytes, uint64_t features);
    int (*read_config)(void*, uint32_t offset, void* output, uint32_t bytes);
    uint8_t* (*queue)(void*, uint32_t index, uint32_t device_writes, uint32_t* capacity);
    int (*interrupt)(void*, void (*callback)(void*), void* context);
    int (*start)(void*);
    int (*submit)(void*, uint32_t queue, uint32_t bytes);
    int (*complete)(void*, uint32_t queue, uint32_t* bytes);
    int (*stop)(void*);
    uint64_t (*now_ns)();
    void (*log)(const char*);
    void (*received)(uint32_t bytes, uint32_t ethertype);
    void (*checked)(uint32_t bytes, uint32_t ethertype, uint32_t cache_entries);
};
struct os1_driver_exports
{
    uint32_t version, bytes;
    int (*probe)(uint32_t pci_index);
    int (*check)();
    int (*stop)();
};
typedef const struct os1_driver_exports* (*os1_driver_entry)(const struct os1_driver_host*);
