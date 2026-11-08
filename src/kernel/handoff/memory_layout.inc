; Shared fixed addresses for BIOS assembly, AP trampoline assembly, and the C++
; handoff/memory_layout.h constants. Keep this file synchronized with that header.

%define BOOT_INFO_MAGIC 0x4F5331424F4F5431
%define BOOT_INFO_VERSION 1
%define BOOT_SOURCE_BIOS_LEGACY 1

%define PAGE_SIZE 0x1000

%define LOADER16_LOAD_ADDRESS 0x1000
%define AP_TRAMPOLINE_ADDRESS LOADER16_LOAD_ADDRESS

%define BOOT_INFO_ADDRESS 0x0500
%define BOOT_DISK_PACKET_ADDRESS 0x0800
%define LOADER_DISK_PACKET_ADDRESS 0x0820
%define LOADER_DISK_RANGE_STATE_ADDRESS 0x0840
%define BOOT_MEMORY_REGION_BUFFER_ADDRESS 0x6000
%define BOOT_MODULE_INFO_BUFFER_ADDRESS 0x7000
%define BOOT_STRING_BUFFER_ADDRESS 0x7200
%define BOOT_STRING_BUFFER_SIZE_BYTES 0x0E00
%define BOOT_MEMORY_REGION_CAPACITY 128

; After BIOS disk and boot-log I/O, reuse their scratch page for page tables.
%define EARLY_LONG_MODE_PAGE_TABLES_ADDRESS 0x9000
%include "kernel_layout.inc"

%define AP_STARTUP_CPU_PAGE_ADDRESS 0x20
%define AP_STARTUP_RIP_ADDRESS 0x28
%define AP_STARTUP_CR3_ADDRESS 0x30
%define AP_STARTUP_IDT_ADDRESS 0x38
%define AP_STARTUP_IDT_SIZE_BYTES 6

%define PAGE_FRAME_BITMAP_BASE_ADDRESS 0x198000
%define PAGE_FRAME_BITMAP_SIZE_BYTES 0x80000

%define EARLY_RESERVED_PHYSICAL_END 0x20000

; Persistent RAM boot log for BIOS bring-up on real hardware. The BIOS loader
; saves the previous boot's log from this window onto the boot disk, re-arms the
; header, and publishes the window to the kernel as the "bootlog" boot module.
%define BOOTLOG_PHYSICAL_ADDRESS 0x04000000
%define BOOTLOG_MAGIC 0x31474F4C4231534F
%define BOOTLOG_VERSION 1
%define BOOTLOG_HEADER_BYTES 64

%define KERNEL_PML4_INDEX ((KERNEL_VIRTUAL_OFFSET >> 39) & 0x1FF)
%define DIRECT_MAP_PML4_INDEX ((DIRECT_MAP_BASE >> 39) & 0x1FF)

%define USER_PML4_INDEX 1
%define USER_SPACE_BASE 0x0000008000000000
%define USER_IMAGE_BASE 0x0000008000400000
%define USER_STACK_TOP 0x0000008040000000
%define USER_STACK_PAGES 16
%define KERNEL_THREAD_STACK_PAGES 4

%define BOOT_TEXT_COLUMNS 80
%define BOOT_TEXT_ROWS 25
