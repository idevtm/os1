#include <gtest/gtest.h>
#include <os1/observe.h>

#include <array>
#include <cstring>
#include <string_view>
#include <vector>

#include "arch/x86_64/cpu/cpu.hpp"
#include "drivers/bus/device.hpp"
#include "drivers/bus/driver_registry.hpp"
#include "handoff/boot_info.hpp"
#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "mm/user_copy.hpp"
#include "mm/virtual_memory.hpp"
#include "platform/acpi_aml.hpp"
#include "platform/acpica_integration.hpp"
#include "platform/platform.hpp"
#include "platform/state.hpp"
#include "proc/thread.hpp"
#include "support/physical_memory.hpp"
#include "syscall/observe.hpp"

namespace
{
constexpr uint64_t kArenaBytes = 2ull * 1024ull * 1024ull;
constexpr uint64_t kBitmapPhysical = 0x100000;
constexpr uint64_t kRsdpPhysical = 0x1000;
constexpr uint64_t kXsdtPhysical = 0x2000;
constexpr uint64_t kFadtPhysical = 0x3000;
constexpr uint64_t kFacsPhysical = 0x4000;
constexpr uint64_t kDsdtPhysical = 0x7000;
constexpr uint64_t kSsdtPhysical = 0x8000;

struct [[gnu::packed]] TestRsdp
{
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
};

struct [[gnu::packed]] TestSdtHeader
{
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

struct [[gnu::packed]] TestXsdt
{
    TestSdtHeader header;
    uint64_t entries[2];
};

struct [[gnu::packed]] TestGas
{
    uint8_t address_space_id;
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t access_size;
    uint64_t address;
};

struct [[gnu::packed]] TestFadt
{
    TestSdtHeader header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved0;
    uint8_t preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command_port;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_request;
    uint8_t pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;
    uint32_t pm1b_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;
    uint32_t gpe0_block;
    uint32_t gpe1_block;
    uint8_t pm1_event_length;
    uint8_t pm1_control_length;
    uint8_t pm2_control_length;
    uint8_t pm_timer_length;
    uint8_t gpe0_block_length;
    uint8_t gpe1_block_length;
    uint8_t gpe1_base;
    uint8_t cstate_control;
    uint16_t c2_latency;
    uint16_t c3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alarm;
    uint8_t month_alarm;
    uint8_t century;
    uint16_t boot_architecture_flags;
    uint8_t reserved1;
    uint32_t flags;
    TestGas reset_register;
    uint8_t reset_value;
    uint16_t arm_boot_architecture_flags;
    uint8_t minor_version;
    uint64_t x_firmware_control;
    uint64_t x_dsdt;
};

struct [[gnu::packed]] TestFacs
{
    char signature[4];
    uint32_t length;
    uint32_t hardware_signature;
    uint32_t firmware_waking_vector;
    uint32_t global_lock;
    uint32_t flags;
    uint64_t x_firmware_waking_vector;
    uint8_t version;
    uint8_t reserved[3];
    uint32_t ospm_flags;
    uint8_t reserved1[24];
};

uint8_t checksum_value(const void* data, size_t length)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint8_t sum = 0;
    for(size_t i = 0; i < length; ++i)
    {
        sum = static_cast<uint8_t>(sum + bytes[i]);
    }
    return static_cast<uint8_t>(0u - sum);
}

PageFrameContainer make_frames()
{
    std::array<BootMemoryRegion, 1> regions{{
        {
            .physical_start = 0,
            .length = kArenaBytes,
            .type = BootMemoryType::Usable,
            .attributes = 0,
        },
    }};

    PageFrameContainer frames;
    EXPECT_TRUE(frames.initialize(regions, kBitmapPhysical, kPageFrameBitmapQwordLimit));
    return frames;
}

BootInfo make_boot_info()
{
    BootInfo info{};
    info.magic = kBootInfoMagic;
    info.version = kBootInfoVersion;
    info.source = BootSource::TestHarness;
    info.rsdp_physical = kRsdpPhysical;
    return info;
}

void initialize_header(TestSdtHeader& header, const char* signature, uint32_t length)
{
    std::memset(&header, 0, sizeof(header));
    std::memcpy(header.signature, signature, 4);
    std::memcpy(header.oem_id, "OS1   ", 6);
    std::memcpy(header.oem_table_id, "OS1AML  ", 8);
    header.length = length;
    header.revision = 2;
    header.oem_revision = 1;
    header.creator_id = 0x314F5331;
    header.creator_revision = 1;
}

void finalize_table(TestSdtHeader& header, size_t total_length)
{
    header.length = static_cast<uint32_t>(total_length);
    header.checksum = 0;
    header.checksum = checksum_value(&header, total_length);
}

void build_acpica_firmware_tables(os1::host_test::PhysicalMemoryArena& arena, bool include_ssdt)
{
    auto* rsdp = reinterpret_cast<TestRsdp*>(arena.data() + kRsdpPhysical);
    auto* xsdt = reinterpret_cast<TestXsdt*>(arena.data() + kXsdtPhysical);
    auto* fadt = reinterpret_cast<TestFadt*>(arena.data() + kFadtPhysical);
    auto* facs = reinterpret_cast<TestFacs*>(arena.data() + kFacsPhysical);

    std::memset(rsdp, 0, sizeof(*rsdp));
    std::memcpy(rsdp->signature, "RSD PTR ", 8);
    std::memcpy(rsdp->oem_id, "OS1   ", 6);
    rsdp->revision = 2;
    rsdp->length = sizeof(*rsdp);
    rsdp->xsdt_address = kXsdtPhysical;
    rsdp->checksum = checksum_value(rsdp, 20);
    rsdp->extended_checksum = checksum_value(rsdp, sizeof(*rsdp));

    std::memset(xsdt, 0, sizeof(*xsdt));
    initialize_header(xsdt->header,
                      "XSDT",
                      static_cast<uint32_t>(sizeof(TestSdtHeader) +
                                            (include_ssdt ? 2u : 1u) * sizeof(uint64_t)));
    xsdt->entries[0] = kFadtPhysical;
    xsdt->entries[1] = include_ssdt ? kSsdtPhysical : 0;
    finalize_table(xsdt->header, xsdt->header.length);

    std::memset(fadt, 0, sizeof(*fadt));
    initialize_header(fadt->header, "FACP", sizeof(*fadt));
    fadt->preferred_pm_profile = 2;
    fadt->sci_interrupt = 9;
    fadt->firmware_ctrl = static_cast<uint32_t>(kFacsPhysical);
    fadt->x_firmware_control = kFacsPhysical;
    fadt->x_dsdt = kDsdtPhysical;
    finalize_table(fadt->header, sizeof(*fadt));

    std::memset(facs, 0, sizeof(*facs));
    std::memcpy(facs->signature, "FACS", 4);
    facs->length = sizeof(*facs);
    facs->version = 1;
}

void append_pkg_length(std::vector<uint8_t>& out, size_t body_length)
{
    if((body_length + 1u) < 0x40u)
    {
        out.push_back(static_cast<uint8_t>(body_length + 1u));
        return;
    }

    if((body_length + 2u) < 0x1000u)
    {
        const size_t encoded_length = body_length + 2u;
        out.push_back(static_cast<uint8_t>(0x40u | (encoded_length & 0x0Fu)));
        out.push_back(static_cast<uint8_t>((encoded_length >> 4) & 0xFFu));
        return;
    }

    const size_t encoded_length = body_length + 3u;
    out.push_back(static_cast<uint8_t>(0x80u | (encoded_length & 0x0Fu)));
    out.push_back(static_cast<uint8_t>((encoded_length >> 4) & 0xFFu));
    out.push_back(static_cast<uint8_t>((encoded_length >> 12) & 0xFFu));
}

void append_nameseg(std::vector<uint8_t>& out, const char* name)
{
    ASSERT_NE(nullptr, name);
    for(size_t i = 0; i < 4; ++i)
    {
        out.push_back(static_cast<uint8_t>(name[i]));
    }
}

void append_name_string(std::vector<uint8_t>& out, const char* path)
{
    ASSERT_NE(nullptr, path);
    if('\\' == path[0])
    {
        out.push_back('\\');
        ++path;
    }
    if(0 == path[0])
    {
        out.push_back(0);
        return;
    }

    std::vector<const char*> segments;
    const char* segment = path;
    while(*segment)
    {
        segments.push_back(segment);
        const char* dot = std::strchr(segment, '.');
        if(nullptr == dot)
        {
            break;
        }
        segment = dot + 1;
    }

    if(2 == segments.size())
    {
        out.push_back(0x2E);
    }
    else if(segments.size() > 2)
    {
        out.push_back(0x2F);
        out.push_back(static_cast<uint8_t>(segments.size()));
    }
    for(const char* current : segments)
    {
        append_nameseg(out, current);
    }
}

void append_integer(std::vector<uint8_t>& out, uint64_t value)
{
    if(0 == value)
    {
        out.push_back(0x00);
        return;
    }
    if(1 == value)
    {
        out.push_back(0x01);
        return;
    }
    if(value <= 0xFFu)
    {
        out.push_back(0x0A);
        out.push_back(static_cast<uint8_t>(value));
        return;
    }
    if(value <= 0xFFFFFFFFull)
    {
        out.push_back(0x0C);
        for(size_t i = 0; i < 4; ++i)
        {
            out.push_back(static_cast<uint8_t>((value >> (i * 8u)) & 0xFFu));
        }
        return;
    }
    out.push_back(0x0E);
    for(size_t i = 0; i < 8; ++i)
    {
        out.push_back(static_cast<uint8_t>((value >> (i * 8u)) & 0xFFu));
    }
}

uint32_t encode_eisa_id(const char* hardware_id)
{
    const uint32_t expanded = (static_cast<uint32_t>(hardware_id[0] - '@') << 26) |
                              (static_cast<uint32_t>(hardware_id[1] - '@') << 21) |
                              (static_cast<uint32_t>(hardware_id[2] - '@') << 16) |
                              static_cast<uint32_t>(std::strtoul(hardware_id + 3, nullptr, 16));
    return ((expanded & 0x000000FFu) << 24) | ((expanded & 0x0000FF00u) << 8) |
           ((expanded & 0x00FF0000u) >> 8) | ((expanded & 0xFF000000u) >> 24);
}

std::vector<uint8_t> make_name_integer(const char* name, uint64_t value)
{
    std::vector<uint8_t> bytes;
    bytes.push_back(0x08);
    append_nameseg(bytes, name);
    append_integer(bytes, value);
    return bytes;
}

std::vector<uint8_t> make_name_eisa_id(const char* name, const char* hardware_id)
{
    return make_name_integer(name, encode_eisa_id(hardware_id));
}

std::vector<uint8_t> make_buffer_name(const char* name, const std::vector<uint8_t>& buffer)
{
    std::vector<uint8_t> body;
    append_integer(body, buffer.size());
    body.insert(body.end(), buffer.begin(), buffer.end());

    std::vector<uint8_t> bytes;
    bytes.push_back(0x08);
    append_nameseg(bytes, name);
    bytes.push_back(0x11);
    append_pkg_length(bytes, body.size());
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

std::vector<uint8_t> make_package_bytes(const std::vector<std::vector<uint8_t>>& elements)
{
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>(elements.size()));
    for(const auto& element : elements)
    {
        body.insert(body.end(), element.begin(), element.end());
    }

    std::vector<uint8_t> bytes;
    bytes.push_back(0x12);
    append_pkg_length(bytes, body.size());
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

std::vector<uint8_t> make_name_package(const char* name,
                                       const std::vector<std::vector<uint8_t>>& elements)
{
    std::vector<uint8_t> bytes;
    bytes.push_back(0x08);
    append_nameseg(bytes, name);
    const auto package = make_package_bytes(elements);
    bytes.insert(bytes.end(), package.begin(), package.end());
    return bytes;
}

std::vector<uint8_t> make_method_return_name(const char* name, const char* return_name)
{
    std::vector<uint8_t> body;
    append_nameseg(body, name);
    body.push_back(0x00);
    body.push_back(0xA4);
    append_nameseg(body, return_name);

    std::vector<uint8_t> bytes;
    bytes.push_back(0x14);
    append_pkg_length(bytes, body.size());
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

std::vector<uint8_t> make_method_return_integer(const char* name, uint64_t value)
{
    std::vector<uint8_t> body;
    append_nameseg(body, name);
    body.push_back(0x00);
    body.push_back(0xA4);
    append_integer(body, value);

    std::vector<uint8_t> bytes;
    bytes.push_back(0x14);
    append_pkg_length(bytes, body.size());
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

std::vector<uint8_t> make_method_store_integer(const char* name,
                                               uint64_t value,
                                               const char* target_name)
{
    std::vector<uint8_t> body;
    append_nameseg(body, name);
    body.push_back(0x00);
    body.push_back(0x70);
    append_integer(body, value);
    append_nameseg(body, target_name);

    std::vector<uint8_t> bytes;
    bytes.push_back(0x14);
    append_pkg_length(bytes, body.size());
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

std::vector<uint8_t> make_device(const char* name, const std::vector<std::vector<uint8_t>>& elements)
{
    std::vector<uint8_t> body;
    append_nameseg(body, name);
    for(const auto& element : elements)
    {
        body.insert(body.end(), element.begin(), element.end());
    }

    std::vector<uint8_t> bytes;
    bytes.push_back(0x5B);
    bytes.push_back(0x82);
    append_pkg_length(bytes, body.size());
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

std::vector<uint8_t> make_scope(const char* path, const std::vector<std::vector<uint8_t>>& elements)
{
    std::vector<uint8_t> body;
    append_name_string(body, path);
    for(const auto& element : elements)
    {
        body.insert(body.end(), element.begin(), element.end());
    }

    std::vector<uint8_t> bytes;
    bytes.push_back(0x10);
    append_pkg_length(bytes, body.size());
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

std::vector<uint8_t> make_irq_resource(uint8_t irq)
{
    const uint16_t mask = static_cast<uint16_t>(1u << irq);
    return std::vector<uint8_t>{0x22,
                                static_cast<uint8_t>(mask & 0xFFu),
                                static_cast<uint8_t>((mask >> 8) & 0xFFu),
                                0x79,
                                0x00};
}

std::vector<uint8_t> make_memory32_fixed_resource(uint32_t base, uint32_t length)
{
    std::vector<uint8_t> resource{0x86, 0x09, 0x00, 0x01};
    for(size_t i = 0; i < 4; ++i)
    {
        resource.push_back(static_cast<uint8_t>((base >> (i * 8u)) & 0xFFu));
    }
    for(size_t i = 0; i < 4; ++i)
    {
        resource.push_back(static_cast<uint8_t>((length >> (i * 8u)) & 0xFFu));
    }
    resource.push_back(0x79);
    resource.push_back(0x00);
    return resource;
}

std::vector<uint8_t> make_root_prt_package()
{
    const auto route0 = make_package_bytes(
        {std::vector<uint8_t>{0x0C, 0xFF, 0xFF, 0x01, 0x00}, std::vector<uint8_t>{0x00},
         std::vector<uint8_t>{'L', 'N', 'K', 'A'}, std::vector<uint8_t>{0x00}});
    const auto route1 = make_package_bytes(
        {std::vector<uint8_t>{0x0C, 0xFF, 0xFF, 0x02, 0x00}, std::vector<uint8_t>{0x01},
         std::vector<uint8_t>{'L', 'N', 'K', 'B'}, std::vector<uint8_t>{0x00}});
    return make_package_bytes({route0, route1});
}

void write_definition_block(os1::host_test::PhysicalMemoryArena& arena,
                            uint64_t physical_address,
                            const char* signature,
                            const std::vector<uint8_t>& aml)
{
    auto* header = reinterpret_cast<TestSdtHeader*>(arena.data() + physical_address);
    initialize_header(*header, signature, sizeof(TestSdtHeader) + aml.size());
    std::memcpy(header + 1, aml.data(), aml.size());
    finalize_table(*header, sizeof(TestSdtHeader) + aml.size());
}

void build_aml_tables(os1::host_test::PhysicalMemoryArena& arena,
                      std::array<AcpiDefinitionBlock, kPlatformMaxAcpiDefinitionBlocks>& blocks,
                      size_t& block_count)
{
    const auto link_a = make_device(
        "LNKA",
        {make_name_eisa_id("_HID", "PNP0C0F"),
         make_buffer_name("CRS0", make_irq_resource(10)),
         make_method_return_name("_CRS", "CRS0"),
         make_method_return_integer("_STA", 0x0F)});
    const auto link_b = make_device(
        "LNKB",
        {make_name_eisa_id("_HID", "PNP0C0F"),
         make_buffer_name("CRS0", make_irq_resource(11)),
         make_method_return_name("_CRS", "CRS0"),
         make_method_return_integer("_STA", 0x0F)});
    const auto child_device = make_device(
        "DEV0",
        {make_name_integer("_ADR", 0x00010000u), make_name_integer("PWST", 0),
         make_method_return_integer("_STA", 0x0F), make_method_store_integer("_PS0", 1, "PWST"),
         make_method_store_integer("_PS3", 3, "PWST")});
    const auto second_child_device = make_device(
        "DEV1",
        {make_name_integer("_ADR", 0x00020000u), make_name_integer("PWST", 0),
         make_method_return_integer("_STA", 0x0F), make_method_store_integer("_PS0", 1, "PWST"),
         make_method_store_integer("_PS3", 3, "PWST")});
    const auto hpet = make_device(
        "HPET",
        {make_name_eisa_id("_HID", "PNP0103"),
         make_buffer_name("_CRS", make_memory32_fixed_resource(0xFED00000u, 0x400u)),
         make_method_return_integer("_STA", 0x0F)});
    const auto pci0 = make_device(
        "PCI0",
        {make_name_eisa_id("_HID", "PNP0A08"), make_name_integer("_BBN", 0),
         make_name_package("_PRT", {make_package_bytes(
                                         {std::vector<uint8_t>{0x0C, 0xFF, 0xFF, 0x01, 0x00},
                                          std::vector<uint8_t>{0x00},
                                          std::vector<uint8_t>{'L', 'N', 'K', 'A'},
                                          std::vector<uint8_t>{0x00}}),
                                    make_package_bytes(
                                        {std::vector<uint8_t>{0x0C, 0xFF, 0xFF, 0x02, 0x00},
                                         std::vector<uint8_t>{0x01},
                                         std::vector<uint8_t>{'L', 'N', 'K', 'B'},
                                         std::vector<uint8_t>{0x00}})}),
         link_a, link_b, child_device, second_child_device, hpet});
    const auto dsdt = make_scope("\\_SB_", {pci0});
    const auto pwrb = make_device(
        "PWRB",
        {make_name_eisa_id("_HID", "PNP0C0C"), make_method_return_integer("_STA", 0x0F)});
    const auto ssdt = make_scope("\\_SB_", {pwrb});

    write_definition_block(arena, kDsdtPhysical, "DSDT", dsdt);
    write_definition_block(arena, kSsdtPhysical, "SSDT", ssdt);

    blocks = {};
    blocks[0].active = true;
    std::memcpy(blocks[0].signature, "DSDT", 4);
    blocks[0].length = static_cast<uint32_t>(sizeof(TestSdtHeader) + dsdt.size());
    blocks[0].physical_address = kDsdtPhysical;
    blocks[1].active = true;
    std::memcpy(blocks[1].signature, "SSDT", 4);
    blocks[1].length = static_cast<uint32_t>(sizeof(TestSdtHeader) + ssdt.size());
    blocks[1].physical_address = kSsdtPhysical;
    block_count = 2;
}

const AcpiDeviceInfo* find_device(const std::array<AcpiDeviceInfo, kAcpiMaxDevices>& devices,
                                  size_t device_count,
                                  const char* path)
{
    for(size_t i = 0; i < device_count; ++i)
    {
        if(devices[i].active && (0 == std::strcmp(devices[i].path, path)))
        {
            return &devices[i];
        }
    }
    return nullptr;
}

std::array<char, 8> g_power_order{};
size_t g_power_order_count = 0;

class AcpiAml : public ::testing::Test
{
protected:
    void SetUp() override
    {
        acpica_reset_for_tests();
    }
};

class AcpicaScopedReset final
{
public:
    ~AcpicaScopedReset()
    {
        acpica_reset_for_tests();
    }
};

bool stub_probe(VirtualMemory&, PageFrameContainer&, const PciDevice&, size_t, DeviceId)
{
    return true;
}

bool suspend_driver_a(DeviceId)
{
    g_power_order[g_power_order_count++] = 'A';
    return true;
}

bool resume_driver_a(DeviceId)
{
    g_power_order[g_power_order_count++] = 'a';
    return true;
}

bool suspend_driver_b(DeviceId)
{
    g_power_order[g_power_order_count++] = 'B';
    return true;
}

bool resume_driver_b(DeviceId)
{
    g_power_order[g_power_order_count++] = 'b';
    return true;
}
}  // namespace

TEST_F(AcpiAml, LoadsDevicesResourcesRoutesAndPowerMethods)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    std::array<AcpiDefinitionBlock, kPlatformMaxAcpiDefinitionBlocks> blocks{};
    size_t block_count = 0;
    build_aml_tables(arena, blocks, block_count);
    build_acpica_firmware_tables(arena, true);

    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    AcpicaScopedReset acpica_reset;

    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info())) << acpica_last_status();
    ASSERT_TRUE(acpi_namespace_load(vm, blocks.data(), block_count))
        << acpi_namespace_last_error() << " last=" << acpi_namespace_last_object();

    std::array<AcpiDeviceInfo, kAcpiMaxDevices> devices{};
    size_t device_count = 0;
    std::array<AcpiPciRoute, kAcpiMaxPciRoutes> routes{};
    size_t route_count = 0;
    ASSERT_TRUE(acpi_build_device_info(devices.data(), device_count, routes.data(), route_count))
        << acpi_namespace_last_error();

    const AcpiDeviceInfo* hpet = find_device(devices, device_count, "\\_SB_.PCI0.HPET");
    ASSERT_NE(nullptr, hpet);
    EXPECT_EQ(0, std::strcmp("PNP0103", hpet->hardware_id));
    ASSERT_EQ(1u, hpet->resource_count);
    EXPECT_EQ(AcpiResourceKind::Memory, hpet->resources[0].kind);
    EXPECT_EQ(0xFED00000ull, hpet->resources[0].base);
    EXPECT_EQ(0x400ull, hpet->resources[0].length);

    const AcpiDeviceInfo* dev0 = find_device(devices, device_count, "\\_SB_.PCI0.DEV0");
    ASSERT_NE(nullptr, dev0);
    EXPECT_EQ(0x00010000ull, dev0->adr);
    EXPECT_EQ(0u, dev0->bus_number);
    EXPECT_NE(0u, dev0->flags & kAcpiDeviceHasPs0);
    EXPECT_NE(0u, dev0->flags & kAcpiDeviceHasPs3);

    const AcpiDeviceInfo* pwrb = find_device(devices, device_count, "\\_SB_.PWRB");
    ASSERT_NE(nullptr, pwrb);
    EXPECT_EQ(0, std::strcmp("PNP0C0C", pwrb->hardware_id));

    ASSERT_EQ(2u, route_count);
    uint32_t irq = 0;
    uint16_t flags = 0;
    ASSERT_TRUE(acpi_resolve_pci_route(0, 1, 0, 0, irq, flags));
    EXPECT_EQ(10u, irq);
    ASSERT_TRUE(acpi_resolve_pci_route(0, 2, 0, 1, irq, flags));
    EXPECT_EQ(11u, irq);

    uint64_t value = 0;
    ASSERT_TRUE(acpi_read_named_integer("\\_SB_.PCI0.DEV0.PWST", value));
    EXPECT_EQ(0u, value);
    ASSERT_TRUE(acpi_set_device_power_state("\\_SB_.PCI0.DEV0", AcpiPowerState::D0));
    ASSERT_TRUE(acpi_read_named_integer("\\_SB_.PCI0.DEV0.PWST", value));
    EXPECT_EQ(1u, value);
    ASSERT_TRUE(acpi_set_device_power_state("\\_SB_.PCI0.DEV0", AcpiPowerState::D3));
    ASSERT_TRUE(acpi_read_named_integer("\\_SB_.PCI0.DEV0.PWST", value));
    EXPECT_EQ(3u, value);
}

TEST_F(AcpiAml, SuspendsAndResumesBoundDevicesInDeterministicOrder)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    std::array<AcpiDefinitionBlock, kPlatformMaxAcpiDefinitionBlocks> blocks{};
    size_t block_count = 0;
    build_aml_tables(arena, blocks, block_count);
    build_acpica_firmware_tables(arena, true);

    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    AcpicaScopedReset acpica_reset;

    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info()));
    ASSERT_TRUE(acpi_namespace_load(vm, blocks.data(), block_count));

    std::array<AcpiDeviceInfo, kAcpiMaxDevices> devices{};
    size_t device_count = 0;
    std::array<AcpiPciRoute, kAcpiMaxPciRoutes> routes{};
    size_t route_count = 0;
    ASSERT_TRUE(acpi_build_device_info(devices.data(), device_count, routes.data(), route_count));

    platform_reset_state();
    driver_registry_reset();
    g_power_order = {};
    g_power_order_count = 0;

    g_platform.acpi_device_capacity = device_count;
    g_platform.acpi_devices = static_cast<AcpiDeviceInfo*>(
        kcalloc(device_count, sizeof(AcpiDeviceInfo)));
    ASSERT_TRUE((0u == device_count) || (nullptr != g_platform.acpi_devices));
    g_platform.acpi_device_count = device_count;
    for(size_t i = 0; i < device_count; ++i)
    {
        g_platform.acpi_devices[i] = devices[i];
    }
    g_platform.device_count = 2;
    g_platform.devices[0].bus = 0;
    g_platform.devices[0].slot = 1;
    g_platform.devices[0].function = 0;
    g_platform.devices[1].bus = 0;
    g_platform.devices[1].slot = 2;
    g_platform.devices[1].function = 0;

    const PciDriver driver_a{
        .name = "driver-a",
        .probe = stub_probe,
        .suspend = suspend_driver_a,
        .resume = resume_driver_a,
    };
    const PciDriver driver_b{
        .name = "driver-b",
        .probe = stub_probe,
        .suspend = suspend_driver_b,
        .resume = resume_driver_b,
    };
    ASSERT_TRUE(driver_registry_add_pci_driver(driver_a));
    ASSERT_TRUE(driver_registry_add_pci_driver(driver_b));
    ASSERT_TRUE(device_binding_publish(DeviceId{DeviceBus::Pci, 0}, 0, "driver-a", nullptr));
    ASSERT_TRUE(device_binding_publish(DeviceId{DeviceBus::Pci, 1}, 1, "driver-b", nullptr));

    ASSERT_TRUE(platform_suspend_devices());

    uint64_t value = 0;
    ASSERT_TRUE(acpi_read_named_integer("\\_SB_.PCI0.DEV0.PWST", value));
    EXPECT_EQ(3u, value);
    ASSERT_TRUE(acpi_read_named_integer("\\_SB_.PCI0.DEV1.PWST", value));
    EXPECT_EQ(3u, value);

    ASSERT_TRUE(platform_resume_devices());
    ASSERT_TRUE(acpi_read_named_integer("\\_SB_.PCI0.DEV0.PWST", value));
    EXPECT_EQ(1u, value);
    ASSERT_TRUE(acpi_read_named_integer("\\_SB_.PCI0.DEV1.PWST", value));
    EXPECT_EQ(1u, value);

    ASSERT_EQ(4u, g_power_order_count);
    EXPECT_EQ(std::string_view("BAab"), std::string_view(g_power_order.data(), g_power_order_count));

    platform_reset_state();
}

TEST_F(AcpiAml, IgnoresMalformedDeviceCrs)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);

    const auto bad0 = make_device(
        "BAD0",
        {make_name_eisa_id("_HID", "PNP0C0C"),
         make_buffer_name("_CRS", std::vector<uint8_t>{0x22}),
         make_method_return_integer("_STA", 0x0F)});
    const auto good0 = make_device(
        "GOOD",
        {make_name_eisa_id("_HID", "PNP0C0C"), make_method_return_integer("_STA", 0x0F)});
    const auto dsdt = make_scope("\\_SB_", {bad0, good0});
    write_definition_block(arena, kDsdtPhysical, "DSDT", dsdt);
    build_acpica_firmware_tables(arena, false);

    std::array<AcpiDefinitionBlock, kPlatformMaxAcpiDefinitionBlocks> blocks{};
    blocks[0].active = true;
    std::memcpy(blocks[0].signature, "DSDT", 4);
    blocks[0].length = static_cast<uint32_t>(sizeof(TestSdtHeader) + dsdt.size());
    blocks[0].physical_address = kDsdtPhysical;

    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    AcpicaScopedReset acpica_reset;

    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info()));
    ASSERT_TRUE(acpi_namespace_load(vm, blocks.data(), 1));

    std::array<AcpiDeviceInfo, kAcpiMaxDevices> devices{};
    size_t device_count = 0;
    std::array<AcpiPciRoute, kAcpiMaxPciRoutes> routes{};
    size_t route_count = 0;
    ASSERT_TRUE(acpi_build_device_info(devices.data(), device_count, routes.data(), route_count));

    const AcpiDeviceInfo* bad_device = find_device(devices, device_count, "\\_SB_.BAD0");
    ASSERT_NE(nullptr, bad_device);
    EXPECT_EQ(0u, bad_device->flags & kAcpiDeviceHasCrs);
    EXPECT_EQ(0u, bad_device->resource_count);

    const AcpiDeviceInfo* good_device = find_device(devices, device_count, "\\_SB_.GOOD");
    ASSERT_NE(nullptr, good_device);
    EXPECT_EQ(0, std::strcmp("PNP0C0C", good_device->hardware_id));
}

TEST_F(AcpiAml, IgnoresMalformedDevicePrt)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);

    const auto pci0 = make_device(
        "PCI0",
        {make_name_eisa_id("_HID", "PNP0A08"), make_name_integer("_BBN", 0),
         make_name_package("_PRT", {std::vector<uint8_t>{0x00}}),
         make_method_return_integer("_STA", 0x0F)});
    const auto pwrb = make_device(
        "PWRB",
        {make_name_eisa_id("_HID", "PNP0C0C"), make_method_return_integer("_STA", 0x0F)});
    const auto dsdt = make_scope("\\_SB_", {pci0, pwrb});
    write_definition_block(arena, kDsdtPhysical, "DSDT", dsdt);
    build_acpica_firmware_tables(arena, false);

    std::array<AcpiDefinitionBlock, kPlatformMaxAcpiDefinitionBlocks> blocks{};
    blocks[0].active = true;
    std::memcpy(blocks[0].signature, "DSDT", 4);
    blocks[0].length = static_cast<uint32_t>(sizeof(TestSdtHeader) + dsdt.size());
    blocks[0].physical_address = kDsdtPhysical;

    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    AcpicaScopedReset acpica_reset;

    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info()));
    ASSERT_TRUE(acpi_namespace_load(vm, blocks.data(), 1));

    std::array<AcpiDeviceInfo, kAcpiMaxDevices> devices{};
    size_t device_count = 0;
    std::array<AcpiPciRoute, kAcpiMaxPciRoutes> routes{};
    size_t route_count = 0;
    ASSERT_TRUE(acpi_build_device_info(devices.data(), device_count, routes.data(), route_count));

    const AcpiDeviceInfo* pci_device = find_device(devices, device_count, "\\_SB_.PCI0");
    ASSERT_NE(nullptr, pci_device);
    EXPECT_EQ(0u, pci_device->flags & kAcpiDeviceHasPrt);
    EXPECT_EQ(0u, route_count);

    const AcpiDeviceInfo* power_button = find_device(devices, device_count, "\\_SB_.PWRB");
    ASSERT_NE(nullptr, power_button);
    EXPECT_EQ(0, std::strcmp("PNP0C0C", power_button->hardware_id));
}

TEST_F(AcpiAml, RunsDeviceIniBeforeSelectedMethodEvaluation)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);

    const auto dev0 = make_device(
        "DEV0",
        {make_name_eisa_id("_HID", "PNP0C0C"), make_name_integer("INIV", 0),
         make_method_store_integer("_INI", 1, "INIV"), make_method_return_integer("_STA", 0x0F)});
    const auto dsdt = make_scope("\\_SB_", {dev0});
    write_definition_block(arena, kDsdtPhysical, "DSDT", dsdt);
    build_acpica_firmware_tables(arena, false);

    std::array<AcpiDefinitionBlock, kPlatformMaxAcpiDefinitionBlocks> blocks{};
    blocks[0].active = true;
    std::memcpy(blocks[0].signature, "DSDT", 4);
    blocks[0].length = static_cast<uint32_t>(sizeof(TestSdtHeader) + dsdt.size());
    blocks[0].physical_address = kDsdtPhysical;

    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    AcpicaScopedReset acpica_reset;

    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info()));
    ASSERT_TRUE(acpi_namespace_load(vm, blocks.data(), 1))
        << acpi_namespace_last_error() << " last=" << acpi_namespace_last_object();

    uint64_t value = 0;
    ASSERT_TRUE(acpi_read_named_integer("\\_SB_.DEV0.INIV", value));
    EXPECT_EQ(1u, value);

    std::array<AcpiDeviceInfo, kAcpiMaxDevices> devices{};
    size_t device_count = 0;
    std::array<AcpiPciRoute, kAcpiMaxPciRoutes> routes{};
    size_t route_count = 0;
    ASSERT_TRUE(acpi_build_device_info(devices.data(), device_count, routes.data(), route_count));

    const AcpiDeviceInfo* device = find_device(devices, device_count, "\\_SB_.DEV0");
    ASSERT_NE(nullptr, device);
    EXPECT_EQ(0x0Fu, device->status);
}
namespace
{
class AcpiObservation : public ::testing::Test
{
protected:
    os1::host_test::PhysicalMemoryArena arena{32ull * 1024 * 1024};
    PageFrameContainer frames;
    Thread* thread = nullptr;
    static constexpr uint64_t buffer = kUserImageBase;
    static constexpr size_t bytes = sizeof(Os1ObserveHeader) + sizeof(Os1ObserveAcpiRecord);

    void SetUp() override
    {
        acpica_reset_for_tests();
        std::array<BootMemoryRegion, 1> regions{{{0, arena.size(), BootMemoryType::Usable, 0}}};
        ASSERT_TRUE(frames.initialize(regions, 0x300000, kPageFrameBitmapQwordLimit));
        kmem_init(frames);
        platform_reset_state();
        ASSERT_TRUE(init_tasks(frames));
        std::array<AcpiDefinitionBlock, kPlatformMaxAcpiDefinitionBlocks> blocks{};
        size_t block_count = 0;
        build_aml_tables(arena, blocks, block_count);
        build_acpica_firmware_tables(arena, true);
        VirtualMemory vm(frames);
        ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info()));
        ASSERT_TRUE(acpi_namespace_load(vm, blocks.data(), block_count));
        g_platform.acpi_devices =
            static_cast<AcpiDeviceInfo*>(kcalloc(kAcpiMaxDevices, sizeof(AcpiDeviceInfo)));
        ASSERT_NE(nullptr, g_platform.acpi_devices);
        g_platform.acpi_device_capacity = kAcpiMaxDevices;
        ASSERT_TRUE(acpi_build_device_info(g_platform.acpi_devices,
                                           g_platform.acpi_device_count,
                                           g_platform.acpi_pci_routes,
                                           g_platform.acpi_pci_route_count));
        g_platform.device_count = 1;
        g_platform.devices[0].slot = 1;
        g_platform.devices[0].interrupt_pin = 1;
        ASSERT_TRUE(vm.allocate_and_map(buffer, 1, PageFlags::User | PageFlags::Write));
        Process* process = create_user_process("acpi-observe", vm.root());
        ASSERT_NE(nullptr, process);
        thread = create_user_thread(process, buffer, buffer + 4096, frames, false);
        ASSERT_NE(nullptr, thread);
        set_current_thread(thread);
    }

    void TearDown() override
    {
        set_current_thread(nullptr);
        platform_reset_state();
        acpica_reset_for_tests();
    }

    long observe(uint64_t address = buffer, size_t length = bytes)
    {
        return sys_observe(ObserveContext{.frames = &frames}, OS1_OBSERVE_ACPI, address, length);
    }

    Os1ObserveAcpiRecord snapshot()
    {
        Os1ObserveAcpiRecord record{};
        EXPECT_TRUE(copy_from_user(
            frames, thread, buffer + sizeof(Os1ObserveHeader), &record, sizeof(record)));
        return record;
    }

    void expect_unchanged_power()
    {
        uint64_t value = 99;
        ASSERT_TRUE(acpi_read_named_integer("\\_SB_.PCI0.DEV0.PWST", value));
        EXPECT_EQ(0u, value);
        ASSERT_TRUE(acpi_read_named_integer("\\_SB_.PCI0.DEV1.PWST", value));
        EXPECT_EQ(0u, value);
    }
};
}  // namespace

TEST_F(AcpiObservation, BoundDeviceSnapshotsNeverInvokePowerMethods)
{
    ASSERT_TRUE(device_binding_publish(DeviceId{DeviceBus::Pci, 0}, 0, "active-device", nullptr));
    for(unsigned i = 0; i < 3; ++i)
    {
        ASSERT_EQ(static_cast<long>(bytes), observe());
        auto record = snapshot();
        EXPECT_EQ(1u, record.route_probe_count);
        EXPECT_EQ(1u, record.route_success_count);
        EXPECT_EQ(10u, record.route_irq);
        EXPECT_EQ(0u, record.power_probe_count);
        EXPECT_EQ(0u, record.power_success_count);
        expect_unchanged_power();
    }
}

TEST_F(AcpiObservation, UnboundDevicesNeverTriggerFallbackPowerMethods)
{
    ASSERT_EQ(static_cast<long>(bytes), observe());
    EXPECT_EQ(0u, snapshot().power_success_count);
    expect_unchanged_power();
}

TEST_F(AcpiObservation, CacheMissDoesNotUseLiveFirmwareResolver)
{
    g_platform.acpi_pci_route_count = 0;
    ASSERT_EQ(static_cast<long>(bytes), observe());
    const auto record = snapshot();
    EXPECT_EQ(1u, record.route_probe_count);
    EXPECT_EQ(0u, record.route_success_count);
    EXPECT_EQ(0u, record.route_irq);
    expect_unchanged_power();
}

TEST_F(AcpiObservation, InvalidBuffersCannotInvokePowerMethods)
{
    EXPECT_EQ(-1, observe(0));
    EXPECT_EQ(-1, observe(buffer, bytes - 1));
    EXPECT_EQ(-1, observe(buffer + 4096));
    // The header fits, but copying the record crosses into an unmapped page.
    EXPECT_EQ(-1, observe(buffer + 4096 - sizeof(Os1ObserveHeader)));
    expect_unchanged_power();
}

TEST_F(AcpiObservation, EmptyInventoryIsAValidZeroedSnapshot)
{
    g_platform.device_count = 0;
    ASSERT_EQ(static_cast<long>(bytes), observe());
    Os1ObserveHeader header{};
    ASSERT_TRUE(copy_from_user(frames, thread, buffer, &header, sizeof(header)));
    EXPECT_EQ(OS1_OBSERVE_ABI_VERSION, header.abi_version);
    EXPECT_EQ(OS1_OBSERVE_ACPI, header.kind);
    EXPECT_EQ(sizeof(Os1ObserveAcpiRecord), header.record_size);
    EXPECT_EQ(1u, header.record_count);
    const auto record = snapshot();
    const Os1ObserveAcpiRecord empty{};
    EXPECT_EQ(0, std::memcmp(&empty, &record, sizeof(record)));
    expect_unchanged_power();
}

TEST_F(AcpiObservation, CachedRoutesMatchBusSlotPinAndFunction)
{
    ASSERT_GE(g_platform.acpi_pci_route_count, 1u);
    auto& route = g_platform.acpi_pci_routes[0];
    EXPECT_EQ(&route, platform_cached_acpi_pci_route(0, 1, 0, 0));
    EXPECT_EQ(&route, platform_cached_acpi_pci_route(0, 1, 7, 0));
    EXPECT_EQ(nullptr, platform_cached_acpi_pci_route(1, 1, 0, 0));
    EXPECT_EQ(nullptr, platform_cached_acpi_pci_route(0, 3, 0, 0));
    EXPECT_EQ(nullptr, platform_cached_acpi_pci_route(0, 1, 0, 1));
    route.function = 2;
    EXPECT_EQ(nullptr, platform_cached_acpi_pci_route(0, 1, 0, 0));
    EXPECT_EQ(&route, platform_cached_acpi_pci_route(0, 1, 2, 0));
    route.active = false;
    EXPECT_EQ(nullptr, platform_cached_acpi_pci_route(0, 1, 2, 0));
    expect_unchanged_power();
}

TEST_F(AcpiObservation, BootCachesEnumeratedRoutesOnceWithoutPowerChanges)
{
    g_platform.acpi_pci_route_count = 0;
    platform_cache_enumerated_pci_routes();
    ASSERT_EQ(1u, g_platform.acpi_pci_route_count);
    EXPECT_EQ(10u, g_platform.acpi_pci_routes[0].irq);
    platform_cache_enumerated_pci_routes();
    EXPECT_EQ(1u, g_platform.acpi_pci_route_count);
    ASSERT_EQ(static_cast<long>(bytes), observe());
    EXPECT_EQ(1u, snapshot().route_success_count);
    expect_unchanged_power();
}

TEST_F(AcpiObservation, BootCacheBoundsAndMissingRoutesRemainPassiveAtObservation)
{
    g_platform.acpi_pci_route_count = 0;
    g_platform.devices[0].interrupt_pin = 0;
    platform_cache_enumerated_pci_routes();
    EXPECT_EQ(0u, g_platform.acpi_pci_route_count);
    g_platform.devices[0].interrupt_pin = 5;
    platform_cache_enumerated_pci_routes();
    EXPECT_EQ(0u, g_platform.acpi_pci_route_count);
    g_platform.devices[0].interrupt_pin = 4;
    platform_cache_enumerated_pci_routes();
    EXPECT_EQ(0u, g_platform.acpi_pci_route_count);
    // A full cache cannot overwrite any route, even when the device lacks a match.
    g_platform.devices[0].interrupt_pin = 1;
    g_platform.acpi_pci_route_count = kAcpiMaxPciRoutes;
    g_platform.acpi_pci_routes[kAcpiMaxPciRoutes - 1].irq = 123;
    platform_cache_enumerated_pci_routes();
    EXPECT_EQ(kAcpiMaxPciRoutes, g_platform.acpi_pci_route_count);
    EXPECT_EQ(123u, g_platform.acpi_pci_routes[kAcpiMaxPciRoutes - 1].irq);
    expect_unchanged_power();
}

TEST_F(AcpiAml, PciRootLocationDefaultsDoNotHideInvalidFirmwareValues)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    auto root = [](const char* name, std::vector<std::vector<uint8_t>> fields) {
        fields.insert(fields.begin(), make_name_eisa_id("_HID", "PNP0A08"));
        return make_device(name, fields);
    };
    const auto dsdt =
        make_scope("\\_SB_",
                   {root("PCI0", {}),
                    root("PCI1", {make_name_integer("_SEG", 2), make_name_integer("_BBN", 7)}),
                    root("PCI2", {make_name_integer("_SEG", 65536)}),
                    root("PCI3", {make_name_integer("_BBN", 256)}),
                    root("PCI4", {make_name_package("_SEG", {std::vector<uint8_t>{0x00}})})});
    write_definition_block(arena, kDsdtPhysical, "DSDT", dsdt);
    build_acpica_firmware_tables(arena, false);
    std::array<AcpiDefinitionBlock, kPlatformMaxAcpiDefinitionBlocks> blocks{};
    blocks[0].active = true;
    std::memcpy(blocks[0].signature, "DSDT", 4);
    blocks[0].length = static_cast<uint32_t>(sizeof(TestSdtHeader) + dsdt.size());
    blocks[0].physical_address = kDsdtPhysical;
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    AcpicaScopedReset acpica_reset;
    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info()));
    ASSERT_TRUE(acpi_namespace_load(vm, blocks.data(), 1));
    std::array<AcpiDeviceInfo, kAcpiMaxDevices> devices{};
    size_t device_count = 0, route_count = 0;
    std::array<AcpiPciRoute, kAcpiMaxPciRoutes> routes{};
    ASSERT_TRUE(acpi_build_device_info(devices.data(), device_count, routes.data(), route_count));
    for(unsigned i = 0; i < 5; ++i)
    {
        char path[] = "\\_SB_.PCI0";
        path[9] += i;
        const auto* d = find_device(devices, device_count, path);
        ASSERT_NE(nullptr, d) << path;
        EXPECT_TRUE(d->flags & kAcpiDevicePciRoot);
        EXPECT_EQ(i < 2, bool(d->flags & kAcpiDeviceRootLocation));
        if(i < 2)
        {
            EXPECT_EQ(i ? 2u : 0u, d->pci_segment);
            EXPECT_EQ(i ? 7u : 0u, d->bus_number);
        }
    }
}
