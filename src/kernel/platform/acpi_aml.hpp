#pragma once

#include <stddef.h>
#include <stdint.h>

#include "platform/types.hpp"

class VirtualMemory;

constexpr size_t kAcpiDevicePathBytes = 64;
constexpr size_t kAcpiHardwareIdBytes = 8;
constexpr size_t kAcpiMaxDevices = 96;
constexpr size_t kAcpiMaxDeviceResources = 8;
constexpr size_t kAcpiMaxPciRoutes = 64;
constexpr uint16_t kAcpiDeviceIndexNone = 0xFFFFu;
constexpr uint16_t kAcpiDeviceHasHid = 1u << 0;
constexpr uint16_t kAcpiDeviceHasUid = 1u << 1;
constexpr uint16_t kAcpiDeviceHasAdr = 1u << 2;
constexpr uint16_t kAcpiDeviceHasBbn = 1u << 3;
constexpr uint16_t kAcpiDeviceHasCrs = 1u << 4;
constexpr uint16_t kAcpiDeviceHasPrt = 1u << 5;
constexpr uint16_t kAcpiDeviceHasPs0 = 1u << 6;
constexpr uint16_t kAcpiDeviceHasPs3 = 1u << 7;

constexpr uint16_t kAcpiDevicePciRoot = 1u << 8;
constexpr uint16_t kAcpiDeviceRootLocation = 1u << 9;

enum class AcpiResourceKind : uint8_t
{
    Irq = 1,
    Io = 2,
    Memory = 3,
};

enum class AcpiPowerState : uint8_t
{
    D0 = 0,
    D3 = 3,
};

struct AcpiResourceInfo
{
    AcpiResourceKind kind;
    uint8_t reserved0;
    uint16_t flags;
    uint64_t base;
    uint64_t length;
};

struct AcpiDeviceInfo
{
    bool active;
    uint8_t resource_count;
    uint16_t flags;
    uint32_t status;
    uint32_t hid_eisa_id;
    uint64_t uid;
    uint64_t adr;
    uint8_t bus_number;
    uint16_t pci_segment;
    char name[5];
    char hardware_id[kAcpiHardwareIdBytes];
    char path[kAcpiDevicePathBytes];
    AcpiResourceInfo resources[kAcpiMaxDeviceResources];
};

struct AcpiPciRoute
{
    bool active;
    bool source_is_gsi;
    uint8_t bus_number;
    uint8_t slot;
    uint8_t function;
    uint8_t pin;
    uint16_t source_device_index;
    uint16_t flags;
    uint32_t irq;
    char source_path[kAcpiDevicePathBytes];
};

bool acpi_namespace_load(VirtualMemory& kernel_vm,
                         const AcpiDefinitionBlock* definition_blocks,
                         size_t definition_block_count);
const char* acpi_namespace_last_error();
const char* acpi_namespace_last_object();
bool acpi_build_device_info(AcpiDeviceInfo* devices,
                            size_t& device_count,
                            AcpiPciRoute* routes,
                            size_t& route_count);
bool acpi_resolve_pci_route(uint8_t bus,
                            uint8_t slot,
                            uint8_t function,
                            uint8_t pin,
                            uint32_t& irq,
                            uint16_t& flags);
bool acpi_resolve_pci_route_details(uint8_t bus,
                                    uint8_t slot,
                                    uint8_t function,
                                    uint8_t pin,
                                    uint32_t& irq,
                                    uint16_t& flags,
                                    bool& source_is_gsi);
bool acpi_device_supports_power_state(const char* path, AcpiPowerState state);
bool acpi_set_device_power_state(const char* path, AcpiPowerState state);
bool acpi_read_named_integer(const char* path, uint64_t& value);