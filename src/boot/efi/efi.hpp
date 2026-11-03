// Minimal UEFI 2.x definitions for the os1 EFI pre-loader. Only the services
// the pre-loader calls are typed; unused table slots are opaque pointers so the
// layouts still match the specification.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define EFIAPI __attribute__((ms_abi))

using EFI_STATUS = uint64_t;
using EFI_HANDLE = void*;
using EFI_EVENT = void*;
using EFI_PHYSICAL_ADDRESS = uint64_t;

constexpr EFI_STATUS EFI_SUCCESS = 0;
constexpr EFI_STATUS kEfiErrorBit = 1ull << 63;
constexpr EFI_STATUS EFI_BUFFER_TOO_SMALL = kEfiErrorBit | 5;
constexpr EFI_STATUS EFI_NOT_FOUND = kEfiErrorBit | 14;

[[nodiscard]] inline bool efi_error(EFI_STATUS status)
{
    return 0 != (status & kEfiErrorBit);
}

struct EFI_GUID
{
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

struct EFI_TABLE_HEADER
{
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
};

struct EFI_INPUT_KEY
{
    uint16_t scan_code;
    char16_t unicode_char;
};

struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL
{
    EFI_STATUS(EFIAPI* reset)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL*, bool);
    EFI_STATUS(EFIAPI* read_key_stroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL*, EFI_INPUT_KEY*);
    EFI_EVENT wait_for_key;
};

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL
{
    void* reset;
    EFI_STATUS(EFIAPI* output_string)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, const char16_t*);
};

enum EFI_ALLOCATE_TYPE : uint32_t
{
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
};

enum EFI_MEMORY_TYPE : uint32_t
{
    EfiReservedMemoryType = 0,
    EfiLoaderCode = 1,
    EfiLoaderData = 2,
    EfiBootServicesCode = 3,
    EfiBootServicesData = 4,
    EfiRuntimeServicesCode = 5,
    EfiRuntimeServicesData = 6,
    EfiConventionalMemory = 7,
    EfiUnusableMemory = 8,
    EfiACPIReclaimMemory = 9,
    EfiACPIMemoryNVS = 10,
};

struct EFI_MEMORY_DESCRIPTOR
{
    uint32_t type;
    uint32_t pad;
    EFI_PHYSICAL_ADDRESS physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
};

struct EFI_DEVICE_PATH_PROTOCOL
{
    uint8_t type;
    uint8_t sub_type;
    uint8_t length[2];
};

struct EFI_BOOT_SERVICES
{
    EFI_TABLE_HEADER header;
    void* raise_tpl;
    void* restore_tpl;
    EFI_STATUS(EFIAPI* allocate_pages)(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE, uint64_t, EFI_PHYSICAL_ADDRESS*);
    EFI_STATUS(EFIAPI* free_pages)(EFI_PHYSICAL_ADDRESS, uint64_t);
    EFI_STATUS(EFIAPI* get_memory_map)(uint64_t*, EFI_MEMORY_DESCRIPTOR*, uint64_t*, uint64_t*, uint32_t*);
    EFI_STATUS(EFIAPI* allocate_pool)(EFI_MEMORY_TYPE, uint64_t, void**);
    EFI_STATUS(EFIAPI* free_pool)(void*);
    void* create_event;
    void* set_timer;
    EFI_STATUS(EFIAPI* wait_for_event)(uint64_t, EFI_EVENT*, uint64_t*);
    void* signal_event;
    void* close_event;
    void* check_event;
    void* install_protocol_interface;
    void* reinstall_protocol_interface;
    void* uninstall_protocol_interface;
    EFI_STATUS(EFIAPI* handle_protocol)(EFI_HANDLE, const EFI_GUID*, void**);
    void* reserved;
    void* register_protocol_notify;
    void* locate_handle;
    void* locate_device_path;
    void* install_configuration_table;
    EFI_STATUS(EFIAPI* load_image)(bool, EFI_HANDLE, EFI_DEVICE_PATH_PROTOCOL*, void*, uint64_t, EFI_HANDLE*);
    EFI_STATUS(EFIAPI* start_image)(EFI_HANDLE, uint64_t*, char16_t**);
    void* exit;
    void* unload_image;
    void* exit_boot_services;
    void* get_next_monotonic_count;
    EFI_STATUS(EFIAPI* stall)(uint64_t);
    EFI_STATUS(EFIAPI* set_watchdog_timer)(uint64_t, uint64_t, uint64_t, const char16_t*);
};

struct EFI_SYSTEM_TABLE
{
    EFI_TABLE_HEADER header;
    char16_t* firmware_vendor;
    uint32_t firmware_revision;
    EFI_HANDLE console_in_handle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL* con_in;
    EFI_HANDLE console_out_handle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* con_out;
    EFI_HANDLE standard_error_handle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* std_err;
    void* runtime_services;
    EFI_BOOT_SERVICES* boot_services;
    uint64_t number_of_table_entries;
    void* configuration_table;
};

struct EFI_LOADED_IMAGE_PROTOCOL
{
    uint32_t revision;
    EFI_HANDLE parent_handle;
    EFI_SYSTEM_TABLE* system_table;
    EFI_HANDLE device_handle;
    EFI_DEVICE_PATH_PROTOCOL* file_path;
    void* reserved;
    uint32_t load_options_size;
    void* load_options;
    void* image_base;
    uint64_t image_size;
    EFI_MEMORY_TYPE image_code_type;
    EFI_MEMORY_TYPE image_data_type;
    void* unload;
};

constexpr uint64_t EFI_FILE_MODE_READ = 0x1;
constexpr uint64_t EFI_FILE_MODE_WRITE = 0x2;
constexpr uint64_t EFI_FILE_MODE_CREATE = 0x8000000000000000ull;
constexpr uint64_t EFI_FILE_DIRECTORY = 0x10;

struct EFI_FILE_PROTOCOL
{
    uint64_t revision;
    EFI_STATUS(EFIAPI* open)(EFI_FILE_PROTOCOL*, EFI_FILE_PROTOCOL**, const char16_t*, uint64_t, uint64_t);
    EFI_STATUS(EFIAPI* close)(EFI_FILE_PROTOCOL*);
    void* delete_file;
    EFI_STATUS(EFIAPI* read)(EFI_FILE_PROTOCOL*, uint64_t*, void*);
    EFI_STATUS(EFIAPI* write)(EFI_FILE_PROTOCOL*, uint64_t*, const void*);
    void* get_position;
    EFI_STATUS(EFIAPI* set_position)(EFI_FILE_PROTOCOL*, uint64_t);
    void* get_info;
    void* set_info;
    EFI_STATUS(EFIAPI* flush)(EFI_FILE_PROTOCOL*);
};

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
{
    uint64_t revision;
    EFI_STATUS(EFIAPI* open_volume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*, EFI_FILE_PROTOCOL**);
};

constexpr EFI_GUID kLoadedImageProtocolGuid{
    0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
constexpr EFI_GUID kSimpleFileSystemProtocolGuid{
    0x964E5B22, 0x6459, 0x11d2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
constexpr EFI_GUID kDevicePathProtocolGuid{
    0x09576E91, 0x6D3F, 0x11d2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
