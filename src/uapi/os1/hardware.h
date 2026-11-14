#pragma once
#include <stdint.h>

enum
{
    OS1_HARDWARE_VERSION = 2,
    OS1_HARDWARE_DESCRIBE = 1,
    OS1_HARDWARE_CHILDREN = 2,
    OS1_HARDWARE_PAGE = 8,
    OS1_HARDWARE_OBJECT = 0,
    OS1_HARDWARE_INTEGER = 1,
    OS1_HARDWARE_STRING = 2,
    OS1_HARDWARE_METHOD = 3,
    // Live input descriptions use a separate identity and generation domain.
    OS1_HARDWARE_INPUT_ROOT = 0x40000000,
    OS1_HARDWARE_INPUT_STRIDE = 8,
    OS1_HARDWARE_KEYBOARDS = OS1_HARDWARE_INPUT_ROOT + 0x800,
    OS1_HARDWARE_POINTERS = OS1_HARDWARE_INPUT_ROOT + 0x810,
    OS1_HARDWARE_INPUT_ITEMS = OS1_HARDWARE_INPUT_ROOT + 0x1000,
    OS1_HARDWARE_INPUT_END = OS1_HARDWARE_INPUT_ROOT + 0x2000,
    OS1_HARDWARE_DISPLAY_ROOT = 0x40002000,
    OS1_HARDWARE_GM206_ROOT = OS1_HARDWARE_DISPLAY_ROOT + 7,
    OS1_HARDWARE_GM206_DIAGNOSTICS = OS1_HARDWARE_DISPLAY_ROOT + 10,
    OS1_HARDWARE_DISPLAY_END = OS1_HARDWARE_DISPLAY_ROOT + 0xb00,
    OS1_HARDWARE_DEVICES = 65536,
    OS1_HARDWARE_INVENTORY,
    OS1_HARDWARE_PCI,
    OS1_HARDWARE_USB,
    OS1_HARDWARE_TOPOLOGY,
    OS1_HARDWARE_CLASSES,
    OS1_HARDWARE_INPUT,
    OS1_HARDWARE_STORAGE,
    OS1_HARDWARE_NETWORK,
    OS1_HARDWARE_DISPLAY,
    OS1_HARDWARE_CONTROLLERS,
    OS1_HARDWARE_OTHER,
    OS1_HARDWARE_DRIVERS,
    OS1_HARDWARE_FIRMWARE,
    OS1_HARDWARE_ACPI,
    OS1_HARDWARE_PROCESSORS,
    OS1_HARDWARE_COVERAGE,
    OS1_HARDWARE_USB_DROPPED,
    OS1_HARDWARE_STORAGE_ROOT,
    OS1_HARDWARE_BLOCKS,
    OS1_HARDWARE_PARTITIONS,
    OS1_HARDWARE_FILESYSTEMS,
    OS1_HARDWARE_MOUNTS,
    OS1_HARDWARE_CATALOG_DROPPED,
    OS1_HARDWARE_USB_INTERFACES,
    OS1_HARDWARE_PLATFORM,
};

// Read-only, boot-local identities. No field is a device-control capability.
struct Os1HardwareNode
{
    uint32_t id, parent, type, reserved;
    uint64_t integer;
    char name[64];
    char text[96];
};

// Input: version, operation, id, start, generation; all other fields zero.
// Output: catalog generation, next, count, end, nodes. A changed generation cancels stale cursors.
struct Os1HardwareQuery
{
    uint32_t version, operation, id, start;
    uint64_t generation;
    uint32_t next, count, end, reserved;
    struct Os1HardwareNode nodes[OS1_HARDWARE_PAGE];
};
