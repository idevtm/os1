#pragma once

#include <stdint.h>

enum
{
    OS1_OBSERVE_ABI_VERSION = 1,
    OS1_OBSERVE_SYSTEM = 1,
    OS1_OBSERVE_PROCESSES = 2,
    OS1_OBSERVE_CPUS = 3,
    OS1_OBSERVE_PCI = 4,
    OS1_OBSERVE_INITRD = 5,
    OS1_OBSERVE_EVENTS = 6,
    OS1_OBSERVE_DEVICES = 7,
    OS1_OBSERVE_RESOURCES = 8,
    OS1_OBSERVE_IRQS = 9,
    OS1_OBSERVE_KMEM = 10,
    OS1_OBSERVE_ACPI = 11,
    OS1_OBSERVE_USB = 12,
    OS1_OBSERVE_USB_CONTROLLER = 1,
    OS1_OBSERVE_USB_ROOT_PORT = 2,
    OS1_OBSERVE_USB_DEVICE = 3,
    OS1_OBSERVE_USB_MOUNT = 4,
    OS1_OBSERVE_CONSOLE_NONE = 0,
    OS1_OBSERVE_CONSOLE_VGA = 1,
    OS1_OBSERVE_CONSOLE_FRAMEBUFFER = 2,
    OS1_OBSERVE_CONSOLE_SERIAL = 3,
    OS1_OBSERVE_PROCESS_FLAG_USER_MODE = 1u << 0,
    OS1_OBSERVE_CPU_FLAG_BSP = 1u << 0,
    OS1_OBSERVE_CPU_FLAG_BOOTED = 1u << 1,
    OS1_OBSERVE_RESOURCE_FLAG_COHERENT = 1u << 0,
    OS1_OBSERVE_EVENT_RING_CAPACITY = 256,
    OS1_OBSERVE_RESOURCE_PCI_BAR = 1,
    OS1_OBSERVE_RESOURCE_DMA = 2,
    OS1_KERNEL_EVENT_TRAP = 1,
    OS1_KERNEL_EVENT_SCHED_TRANSITION = 2,
    OS1_KERNEL_EVENT_IRQ = 3,
    OS1_KERNEL_EVENT_BLOCK_IO = 4,
    OS1_KERNEL_EVENT_PCI_BIND = 5,
    OS1_KERNEL_EVENT_USER_COPY_FAILURE = 6,
    OS1_KERNEL_EVENT_SMOKE_MARKER = 7,
    OS1_KERNEL_EVENT_TIMER_SOURCE = 8,
    OS1_KERNEL_EVENT_NET_RX = 9,
    OS1_KERNEL_EVENT_KMEM_CORRUPTION = 10,
    OS1_KERNEL_EVENT_AP_ONLINE = 11,
    OS1_KERNEL_EVENT_AP_TICK = 12,
    OS1_KERNEL_EVENT_IPI_RESCHED = 13,
    OS1_KERNEL_EVENT_KERNEL_THREAD_PING = 14,
    OS1_KERNEL_EVENT_IPI_TLB_SHOOTDOWN = 15,
    OS1_KERNEL_EVENT_THREAD_MIGRATE = 16,
    OS1_KERNEL_EVENT_RUNQ_DEPTH = 17,
    OS1_KERNEL_EVENT_FLAG_USER = 1u << 0,
    OS1_KERNEL_EVENT_FLAG_BEGIN = 1u << 1,
    OS1_KERNEL_EVENT_FLAG_SUCCESS = 1u << 2,
    OS1_KERNEL_EVENT_FLAG_FAILURE = 1u << 3,
    OS1_KERNEL_EVENT_FLAG_TO_USER = 1u << 4,
    OS1_KERNEL_EVENT_FLAG_FROM_USER = 1u << 5,
    OS1_KERNEL_EVENT_TIMER_SOURCE_PIT = 1,
    OS1_KERNEL_EVENT_TIMER_SOURCE_LAPIC = 2,
    OS1_KERNEL_EVENT_TIMER_SOURCE_AP_TICKS_DISABLED = 1u << 8,
    OS1_KERNEL_EVENT_USER_COPY_NULL_CONTEXT = 1,
    OS1_KERNEL_EVENT_USER_COPY_BAD_RANGE = 2,
    OS1_KERNEL_EVENT_USER_COPY_TRANSLATE = 3,
    OS1_KERNEL_EVENT_USER_COPY_PERMISSION = 4,
    OS1_KERNEL_EVENT_USER_COPY_STRING_TOO_LONG = 5,
    OS1_OBSERVE_INDEX_NONE = 0xFFFFu,
    OS1_OBSERVE_BOOTLOADER_NAME_BYTES = 64,
    OS1_OBSERVE_PROCESS_NAME_BYTES = 32,
    OS1_OBSERVE_DRIVER_NAME_BYTES = 32,
    OS1_OBSERVE_INITRD_PATH_BYTES = 64,
    OS1_OBSERVE_KMEM_NAME_BYTES = 32,
};

#define OS1_KERNEL_EVENT_SMOKE_MAGIC UINT64_C(0x4F53314556454E54)

#pragma pack(push, 1)

struct Os1ObserveUsbRecord
{
    uint32_t kind, pci_index, root_port, port, address, parent_address, status, detail;
    uint64_t sectors;
    char stage[24];
};

struct Os1ObserveHeader
{
    uint32_t abi_version;
    uint32_t kind;
    uint32_t record_size;
    uint32_t record_count;
};

struct Os1ObserveSystemRecord
{
    uint32_t boot_source;
    uint32_t console_kind;
    uint64_t tick_count;
    uint64_t total_pages;
    uint64_t free_pages;
    uint32_t process_count;
    uint32_t runnable_thread_count;
    uint32_t cpu_count;
    uint32_t pci_device_count;
    uint32_t virtio_blk_present;
    uint32_t reserved0;
    uint64_t virtio_blk_capacity_sectors;
    char bootloader_name[OS1_OBSERVE_BOOTLOADER_NAME_BYTES];
};

struct Os1ObserveProcessRecord
{
    uint64_t pid;
    uint64_t tid;
    uint64_t cr3;
    uint32_t process_state;
    uint32_t thread_state;
    uint32_t flags;
    char name[OS1_OBSERVE_PROCESS_NAME_BYTES];
};

struct Os1ObserveCpuRecord
{
    uint32_t logical_index;
    uint32_t apic_id;
    uint32_t flags;
    uint64_t current_pid;
    uint64_t current_tid;
    uint32_t runq_depth;
    uint64_t timer_ticks;
    uint64_t idle_ticks;
    uint64_t migrate_in;
    uint64_t migrate_out;
};

struct Os1ObservePciBar
{
    uint64_t base;
    uint64_t size;
    uint8_t type;
};

struct Os1ObservePciRecord
{
    uint16_t segment_group;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint8_t capability_pointer;
    uint8_t bar_count;
    struct Os1ObservePciBar bars[6];
};

struct Os1ObserveDeviceRecord
{
    uint8_t bus;
    uint8_t state;
    uint16_t id;
    uint16_t pci_index;
    uint16_t reserved0;
    char driver_name[OS1_OBSERVE_DRIVER_NAME_BYTES];
};

struct Os1ObserveResourceRecord
{
    uint64_t base;
    uint64_t size;
    uint32_t flags;
    uint32_t page_count;
    uint16_t owner_id;
    uint16_t reference_id;
    uint8_t kind;
    uint8_t owner_bus;
    uint8_t entry_index;
    uint8_t detail;
};

struct Os1ObserveIrqRecord
{
    uint8_t vector;
    uint8_t kind;
    uint8_t owner_bus;
    uint8_t source_irq;
    uint16_t owner_id;
    uint16_t source_id;
    uint16_t flags;
    uint16_t reserved0;
    uint32_t gsi;
};

struct Os1ObserveInitrdRecord
{
    char path[OS1_OBSERVE_INITRD_PATH_BYTES];
    uint64_t size;
};

struct Os1ObserveEventRecord
{
    uint64_t sequence;
    uint64_t tick_count;
    uint64_t pid;
    uint64_t tid;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
    uint32_t type;
    uint32_t flags;
    uint32_t cpu;
    uint32_t reserved0;
};

struct Os1ObserveKmemRecord
{
    uint32_t cache_index;
    uint32_t object_size;
    uint32_t alignment;
    uint32_t slab_pages;
    uint32_t slab_count;
    uint32_t free_objects;
    uint32_t live_objects;
    uint32_t peak_live_objects;
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t failed_alloc_count;
    char name[OS1_OBSERVE_KMEM_NAME_BYTES];
};

struct Os1ObserveAcpiRecord
{
    // Cached PCI-pin lookup attempts and matches; no firmware methods execute.
    uint32_t route_probe_count;
    uint32_t route_success_count;
    // Legacy fields, reserved zero. Observation does not control device power.
    uint32_t power_probe_count;
    uint32_t power_success_count;
    uint32_t route_irq;
    uint16_t route_flags;
    uint8_t route_bus;
    uint8_t route_slot;
    uint8_t route_function;
    uint8_t route_pin;
    uint8_t route_source_is_gsi;
    uint8_t reserved0;
};

#pragma pack(pop)
