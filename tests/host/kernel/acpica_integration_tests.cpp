#include <os1/resource.h>
#include <os1/system.h>

#include "arch/x86_64/cpu/cpu.hpp"
#include "handoff/boot_info.hpp"
#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "mm/virtual_memory.hpp"
#include "platform/acpica_diagnostics.hpp"
#include "platform/acpica_ec.hpp"
#include "platform/acpica_integration.hpp"
#include "platform/state.hpp"
#include "platform/system_control.hpp"
#include "platform/system_power.hpp"
#include "support/physical_memory.hpp"

extern "C"
{
#include "acpi.h"
}

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace
{
constexpr uint64_t kArenaBytes = 2ull * 1024ull * 1024ull;
constexpr uint64_t kBitmapPhysical = 0x100000;
constexpr uint64_t kRsdpPhysical = 0x1000;
constexpr uint64_t kXsdtPhysical = 0x2000;
constexpr uint64_t kMadtPhysical = 0x3000;
constexpr uint64_t kFadtPhysical = 0x4000;
constexpr uint64_t kDsdtPhysical = 0x5000;
constexpr uint64_t kOslMemoryPhysical = 0x18000;
constexpr uint64_t kOslEcamBasePhysical = 0x40000;

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
    uint64_t entries[20];
};

struct [[gnu::packed]] TestMadt
{
    TestSdtHeader header;
    uint32_t lapic_address;
    uint32_t flags;
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

uint8_t checksum_value(const void* data, size_t length)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint8_t sum = 0;
    for(size_t index = 0; index < length; ++index)
    {
        sum = static_cast<uint8_t>(sum + bytes[index]);
    }
    return static_cast<uint8_t>(0u - sum);
}

void initialize_header(TestSdtHeader& header, const char* signature, uint32_t length)
{
    std::memset(&header, 0, sizeof(header));
    std::memcpy(header.signature, signature, 4);
    std::memcpy(header.oem_id, "OS1   ", 6);
    std::memcpy(header.oem_table_id, "OS1ACPIC", 8);
    header.length = length;
    header.revision = 1;
    header.oem_revision = 1;
    header.creator_id = 0x314F5331u;
    header.creator_revision = 1;
}

void finalize_sdt(TestSdtHeader& header)
{
    header.checksum = 0;
    header.checksum = checksum_value(&header, header.length);
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

BootInfo make_boot_info(uint64_t rsdp_physical)
{
    BootInfo info{};
    info.magic = kBootInfoMagic;
    info.version = kBootInfoVersion;
    info.source = BootSource::TestHarness;
    info.rsdp_physical = rsdp_physical;
    return info;
}

void build_tables(os1::host_test::PhysicalMemoryArena& arena,
                  bool include_fadt,
                  bool corrupt_rsdp_checksum)
{
    auto* rsdp = reinterpret_cast<TestRsdp*>(arena.data() + kRsdpPhysical);
    auto* xsdt = reinterpret_cast<TestXsdt*>(arena.data() + kXsdtPhysical);
    auto* madt = reinterpret_cast<TestMadt*>(arena.data() + kMadtPhysical);
    auto* fadt = reinterpret_cast<TestFadt*>(arena.data() + kFadtPhysical);
    auto* dsdt = reinterpret_cast<TestSdtHeader*>(arena.data() + kDsdtPhysical);

    std::memset(rsdp, 0, sizeof(*rsdp));
    std::memcpy(rsdp->signature, "RSD PTR ", 8);
    std::memcpy(rsdp->oem_id, "OS1   ", 6);
    rsdp->revision = 2;
    rsdp->length = sizeof(*rsdp);
    rsdp->xsdt_address = kXsdtPhysical;
    rsdp->checksum = checksum_value(rsdp, 20);
    rsdp->extended_checksum = checksum_value(rsdp, sizeof(*rsdp));
    if(corrupt_rsdp_checksum)
    {
        ++rsdp->checksum;
        ++rsdp->extended_checksum;
    }

    std::memset(xsdt, 0, sizeof(*xsdt));
    initialize_header(xsdt->header,
                      "XSDT",
                      static_cast<uint32_t>(sizeof(TestSdtHeader) +
                                            (include_fadt ? 2u : 1u) * sizeof(uint64_t)));
    xsdt->entries[0] = kMadtPhysical;
    xsdt->entries[1] = include_fadt ? kFadtPhysical : 0;
    finalize_sdt(xsdt->header);

    std::memset(madt, 0, sizeof(*madt));
    initialize_header(madt->header, "APIC", sizeof(*madt));
    madt->lapic_address = 0xFEE00000u;
    madt->flags = 1;
    finalize_sdt(madt->header);

    std::memset(dsdt, 0, sizeof(*dsdt));
    initialize_header(*dsdt, "DSDT", sizeof(*dsdt));
    finalize_sdt(*dsdt);

    if(include_fadt)
    {
        std::memset(fadt, 0, sizeof(*fadt));
        initialize_header(fadt->header, "FACP", sizeof(*fadt));
        fadt->sci_interrupt = 9;
        fadt->x_dsdt = kDsdtPhysical;
        finalize_sdt(fadt->header);
    }
}

class AcpicaIntegration : public ::testing::Test
{
protected:
    void SetUp() override
    {
        g_platform = {};
        acpica_reset_for_tests();
    }
};

class AcpicaScopedReset final
{
public:
    ~AcpicaScopedReset()
    {
        g_platform = {};
        acpica_reset_for_tests();
    }
};
}  // namespace

TEST_F(AcpicaIntegration, InitializesTableManagerFromBootRsdp)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    build_tables(arena, true, false);

    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    AcpicaScopedReset acpica_reset;

    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info(kRsdpPhysical)));
    EXPECT_TRUE(acpica_tables_initialized());
    EXPECT_STREQ("AE_OK", acpica_last_status());

    char fadt_signature[] = "FACP";
    ACPI_TABLE_HEADER* fadt = nullptr;
    ASSERT_EQ(AE_OK, AcpiGetTable(fadt_signature, 1, &fadt));
    ASSERT_NE(nullptr, fadt);
    EXPECT_EQ(0, std::memcmp(fadt->Signature, "FACP", 4));
    AcpiPutTable(fadt);

    char madt_signature[] = "APIC";
    ACPI_TABLE_HEADER* madt = nullptr;
    ASSERT_EQ(AE_OK, AcpiGetTable(madt_signature, 1, &madt));
    ASSERT_NE(nullptr, madt);
    EXPECT_EQ(0, std::memcmp(madt->Signature, "APIC", 4));
    AcpiPutTable(madt);
}

TEST_F(AcpicaIntegration, BoundsSsdtInventoryAndKeepsTablesAvailableToAcpica)
{
    constexpr uint64_t mcfg_physical = 0x6000;
    constexpr uint64_t ssdt_physical = 0x7000;
    constexpr size_t ssdt_inventory = kPlatformMaxAcpiDefinitionBlocks - 1;
    for(unsigned supplied : {15, 16, 17})
    {
        SCOPED_TRACE(supplied);
        os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
        build_tables(arena, true, false);
        auto* xsdt = reinterpret_cast<TestXsdt*>(arena.data() + kXsdtPhysical);
        auto* mcfg = reinterpret_cast<TestSdtHeader*>(arena.data() + mcfg_physical);
        initialize_header(*mcfg, "MCFG", sizeof(*mcfg));
        finalize_sdt(*mcfg);
        for(unsigned i = 0; i < supplied; ++i)
        {
            const uint64_t address = ssdt_physical + 0x100 * i;
            auto* header = reinterpret_cast<TestSdtHeader*>(arena.data() + address);
            // Name(Sxxx, ByteConst) gives each table distinct valid AML.
            const uint8_t aml[]{
                0x08, 'S', '0', uint8_t('0' + i / 10), uint8_t('0' + i % 10), 0x0a, uint8_t(i)};
            initialize_header(*header, "SSDT", sizeof(*header) + sizeof(aml));
            std::memcpy(header + 1, aml, sizeof(aml));
            finalize_sdt(*header);
            xsdt->entries[i + 2] = address;
        }
        // Find required tables even when they follow entries outside the inventory.
        xsdt->entries[supplied + 2] = mcfg_physical;
        xsdt->header.length = sizeof(TestSdtHeader) + (supplied + 3) * sizeof(uint64_t);
        finalize_sdt(xsdt->header);

        PageFrameContainer frames = make_frames();
        kmem_init(frames);
        VirtualMemory vm(frames);
        const auto boot = make_boot_info(kRsdpPhysical);
        AcpicaScopedReset reset;
        ASSERT_TRUE(acpica_initialize_tables(vm, boot));
        uint64_t madt = 0, mcfg_address = 0, hpet = 0, fadt = 0;
        std::array<uint64_t, ssdt_inventory + 1> inventory{};
        inventory.back() = UINT64_MAX;
        size_t retained = 0;
        ASSERT_TRUE(acpica_discover_tables(
            madt, mcfg_address, hpet, fadt, inventory.data(), ssdt_inventory, retained));
        EXPECT_EQ(retained, ssdt_inventory);
        EXPECT_EQ(madt, kMadtPhysical);
        EXPECT_EQ(mcfg_address, mcfg_physical);
        EXPECT_EQ(fadt, kFadtPhysical);
        EXPECT_EQ(inventory.back(), UINT64_MAX);
        for(size_t i = 0; i < retained; ++i)
            EXPECT_EQ(inventory[i], ssdt_physical + 0x100 * i);
        ASSERT_TRUE(acpica_load_namespace());
        char name[]{"\\S000"};
        name[3] += (supplied - 1) / 10;
        name[4] += (supplied - 1) % 10;
        ACPI_HANDLE last = nullptr;
        EXPECT_EQ(AE_OK, AcpiGetHandle(nullptr, name, &last));
        EXPECT_NE(last, nullptr);
    }
}

TEST_F(AcpicaIntegration, RejectsMissingBootRsdp)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    build_tables(arena, true, false);

    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    AcpicaScopedReset acpica_reset;

    EXPECT_FALSE(acpica_initialize_tables(vm, make_boot_info(0)));
    EXPECT_FALSE(acpica_tables_initialized());
    EXPECT_STREQ("AE_NOT_FOUND", acpica_last_status());
}

TEST_F(AcpicaIntegration, RejectsBrokenOrIncompleteFirmwareTables)
{
    os1::host_test::PhysicalMemoryArena bad_checksum_arena(kArenaBytes);
    build_tables(bad_checksum_arena, true, true);

    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    AcpicaScopedReset acpica_reset;

    EXPECT_FALSE(acpica_initialize_tables(vm, make_boot_info(kRsdpPhysical)));
    EXPECT_FALSE(acpica_tables_initialized());

    os1::host_test::PhysicalMemoryArena missing_fadt_arena(kArenaBytes);
    build_tables(missing_fadt_arena, false, false);

    EXPECT_FALSE(acpica_initialize_tables(vm, make_boot_info(kRsdpPhysical)));
    EXPECT_FALSE(acpica_tables_initialized());
}

TEST_F(AcpicaIntegration, OslSemaphoresEnforceBounds)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    AcpicaScopedReset acpica_reset;

    ACPI_SEMAPHORE handle = nullptr;
    EXPECT_EQ(AE_BAD_PARAMETER, AcpiOsCreateSemaphore(1, 2, &handle));
    EXPECT_EQ(AE_BAD_PARAMETER, AcpiOsCreateSemaphore(1, 0, nullptr));

    ASSERT_EQ(AE_OK, AcpiOsCreateSemaphore(2, 1, &handle));
    ASSERT_NE(nullptr, handle);

    EXPECT_EQ(AE_OK, AcpiOsWaitSemaphore(handle, 1, 0));
    EXPECT_EQ(AE_TIME, AcpiOsWaitSemaphore(handle, 1, 0));
    EXPECT_EQ(AE_TIME, AcpiOsWaitSemaphore(handle, 0, 0));
    EXPECT_EQ(AE_OK, AcpiOsSignalSemaphore(handle, 1));
    EXPECT_EQ(AE_LIMIT, AcpiOsSignalSemaphore(handle, 2));
    EXPECT_EQ(AE_OK, AcpiOsDeleteSemaphore(handle));
}

TEST_F(AcpicaIntegration, OslMapsAndAccessesPhysicalMemory)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    build_tables(arena, true, false);

    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    AcpicaScopedReset acpica_reset;

    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info(kRsdpPhysical)));

    void* mapped = AcpiOsMapMemory(kOslMemoryPhysical, sizeof(uint64_t));
    ASSERT_NE(nullptr, mapped);
    EXPECT_EQ(kernel_physical_pointer<void>(kOslMemoryPhysical), mapped);

    UINT64 value = 0;
    EXPECT_EQ(AE_OK, AcpiOsWriteMemory(kOslMemoryPhysical + 0x00, 0x5Au, 8));
    EXPECT_EQ(AE_OK, AcpiOsReadMemory(kOslMemoryPhysical + 0x00, &value, 8));
    EXPECT_EQ(0x5Au, value);

    EXPECT_EQ(AE_OK, AcpiOsWriteMemory(kOslMemoryPhysical + 0x08, 0xBEEFu, 16));
    EXPECT_EQ(AE_OK, AcpiOsReadMemory(kOslMemoryPhysical + 0x08, &value, 16));
    EXPECT_EQ(0xBEEFu, value);

    EXPECT_EQ(AE_OK, AcpiOsWriteMemory(kOslMemoryPhysical + 0x10, 0xAABBCCDDu, 32));
    EXPECT_EQ(AE_OK, AcpiOsReadMemory(kOslMemoryPhysical + 0x10, &value, 32));
    EXPECT_EQ(0xAABBCCDDu, value);

    EXPECT_EQ(AE_OK,
              AcpiOsWriteMemory(kOslMemoryPhysical + 0x18, 0x1122334455667788ull, 64));
    EXPECT_EQ(AE_OK, AcpiOsReadMemory(kOslMemoryPhysical + 0x18, &value, 64));
    EXPECT_EQ(0x1122334455667788ull, value);

    EXPECT_EQ(AE_BAD_PARAMETER, AcpiOsReadMemory(kOslMemoryPhysical, nullptr, 8));
    EXPECT_EQ(AE_BAD_PARAMETER, AcpiOsReadMemory(kOslMemoryPhysical, &value, 24));
    EXPECT_EQ(AE_BAD_PARAMETER, AcpiOsWriteMemory(kOslMemoryPhysical, 0x55u, 24));

    UINT32 port_value = 0;
    EXPECT_EQ(AE_BAD_PARAMETER, AcpiOsReadPort(0x70, nullptr, 8));
    EXPECT_EQ(AE_SUPPORT, AcpiOsReadPort(0x70, &port_value, 8));
    EXPECT_EQ(AE_SUPPORT, AcpiOsWritePort(0x70, 0x12u, 8));

    AcpiOsUnmapMemory(mapped, sizeof(uint64_t));
}

TEST_F(AcpicaIntegration, OslReadsAndWritesPublishedPciConfigSpace)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    build_tables(arena, true, false);

    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    AcpicaScopedReset acpica_reset;

    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info(kRsdpPhysical)));

    g_platform.ecam_region_count = 1;
    g_platform.ecam_regions[0].base_address = kOslEcamBasePhysical;
    g_platform.ecam_regions[0].segment_group = 0;
    g_platform.ecam_regions[0].bus_start = 0;
    g_platform.ecam_regions[0].bus_end = 0;

    ACPI_PCI_ID pci_id{};
    pci_id.Segment = 0;
    pci_id.Bus = 0;
    pci_id.Device = 1;
    pci_id.Function = 0;

    constexpr uint64_t kDevicePhysicalBase = kOslEcamBasePhysical + (1ull << 15u);
    *kernel_physical_pointer<volatile uint32_t>(kDevicePhysicalBase + 0x10) = 0xAABBCCDDu;

    UINT64 value = 0;
    EXPECT_EQ(AE_OK, AcpiOsReadPciConfiguration(&pci_id, 0x10, &value, 32));
    EXPECT_EQ(0xAABBCCDDu, value);

    EXPECT_EQ(AE_OK, AcpiOsWritePciConfiguration(&pci_id, 0x14, 0x55AAu, 16));
    EXPECT_EQ(0x55AAu, *kernel_physical_pointer<volatile uint16_t>(kDevicePhysicalBase + 0x14));

    EXPECT_EQ(AE_BAD_PARAMETER, AcpiOsReadPciConfiguration(&pci_id, 0x10, &value, 24));

    pci_id.Bus = 1;
    EXPECT_EQ(AE_NOT_FOUND, AcpiOsReadPciConfiguration(&pci_id, 0x10, &value, 32));
    EXPECT_EQ(AE_NOT_FOUND, AcpiOsWritePciConfiguration(&pci_id, 0x10, 0x11u, 32));
}

TEST_F(AcpicaIntegration, ReducedHardwareShutdownIsRejectedWithoutEnteringSleep)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    auto frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    AcpicaScopedReset cleanup;
    build_tables(arena, true, false);
    auto* fadt = reinterpret_cast<TestFadt*>(arena.data() + kFadtPhysical);
    fadt->flags = ACPI_FADT_HW_REDUCED;
    finalize_sdt(fadt->header);
    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info(kRsdpPhysical)));
    PowerDiagnostic diagnostic;
    EXPECT_FALSE(acpica_prepare_shutdown(diagnostic));
    EXPECT_STREQ("fadt-profile", diagnostic.stage);
    EXPECT_STREQ("hardware-reduced-unsupported", diagnostic.detail);
    EXPECT_FALSE(acpica_prepare_shutdown(diagnostic));  // No polling sleep loop.
    EXPECT_STREQ("fadt-profile", diagnostic.stage);
}

TEST_F(AcpicaIntegration, ShutdownDiagnosticsDistinguishMissingTablesAndInvalidControls)
{
    PowerDiagnostic diagnostic{"stale", "stale"};
    EXPECT_FALSE(acpica_prepare_shutdown(diagnostic));
    EXPECT_STREQ("acpi-tables", diagnostic.stage);
    EXPECT_STREQ("not-initialized", diagnostic.detail);
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    auto frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    AcpicaScopedReset cleanup;
    build_tables(arena, true, false);
    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info(kRsdpPhysical)));
    EXPECT_FALSE(acpica_prepare_shutdown(diagnostic));
    EXPECT_STREQ("pm1a-control", diagnostic.stage);
    EXPECT_STREQ("invalid-register", diagnostic.detail);
}

TEST_F(AcpicaIntegration, ShutdownDiagnosticsPreserveMissingS5Exception)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    auto frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    AcpicaScopedReset cleanup;
    build_tables(arena, true, false);
    auto* table = reinterpret_cast<ACPI_TABLE_FADT*>(arena.data() + kFadtPhysical);
    table->Header.Length = sizeof(*table);
    table->Pm1ControlLength = 2;
    table->XPm1aControlBlock = {ACPI_ADR_SPACE_SYSTEM_MEMORY, 16, 0, 2, kOslMemoryPhysical};
    *reinterpret_cast<uint16_t*>(arena.data() + kOslMemoryPhysical) = 1;  // SCI_EN.
    finalize_sdt(*reinterpret_cast<TestSdtHeader*>(table));
    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info(kRsdpPhysical)));
    PowerDiagnostic diagnostic;
    EXPECT_FALSE(acpica_prepare_shutdown(diagnostic));
    EXPECT_STREQ("s5-types", diagnostic.stage);
    EXPECT_STREQ("AE_NOT_FOUND", diagnostic.detail);
}

extern bool (*host_acpica_memory_write_fault)(ACPI_PHYSICAL_ADDRESS, UINT64, UINT32);
extern void (*host_acpica_stall_observer)(UINT32);

TEST_F(AcpicaIntegration, WaitsAfterPm1aEnableEvenIfPm1bEnableFails)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    auto frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    AcpicaScopedReset cleanup;
    build_tables(arena, true, false);
    auto* table = reinterpret_cast<ACPI_TABLE_FADT*>(arena.data() + kFadtPhysical);
    table->Header.Length = sizeof(*table);
    table->Pm1ControlLength = 2;
    table->XPm1aControlBlock = {ACPI_ADR_SPACE_SYSTEM_MEMORY, 16, 0, 2, kOslMemoryPhysical};
    table->XPm1bControlBlock = {ACPI_ADR_SPACE_SYSTEM_MEMORY, 16, 0, 2, kOslMemoryPhysical + 2};
    *kernel_physical_pointer<uint16_t>(kOslMemoryPhysical) = 1;  // SCI_EN
    finalize_sdt(*reinterpret_cast<TestSdtHeader*>(table));
    const uint8_t s5[] = {0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x02, 0x0A, 5, 0x0A, 5};
    auto* dsdt = reinterpret_cast<TestSdtHeader*>(arena.data() + kDsdtPhysical);
    dsdt->length = sizeof(*dsdt) + sizeof(s5);
    std::memcpy(dsdt + 1, s5, sizeof(s5));
    finalize_sdt(*dsdt);
    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info(kRsdpPhysical)));
    PowerDiagnostic diagnostic;
    ASSERT_TRUE(acpica_prepare_shutdown(diagnostic)) << diagnostic.stage;
    constexpr uint16_t sleep_enable = 1u << 13;
    static UINT32 wait_us;
    wait_us = 0;
    host_acpica_memory_write_fault = [](ACPI_PHYSICAL_ADDRESS address, UINT64 value, UINT32) {
        return address == kOslMemoryPhysical + 2 && (value & sleep_enable);
    };
    host_acpica_stall_observer = [](UINT32 microseconds) {
        EXPECT_NE(0u, *kernel_physical_pointer<uint16_t>(kOslMemoryPhysical) & sleep_enable);
        wait_us += microseconds;
    };
    EXPECT_FALSE(acpica_enter_shutdown(diagnostic));
    host_acpica_memory_write_fault = nullptr;
    host_acpica_stall_observer = nullptr;
    EXPECT_STREQ("pm1b-enable-write", diagnostic.stage);
    EXPECT_STREQ("AE_ERROR", diagnostic.detail);
    EXPECT_EQ(100'000u, wait_us);
}

void host_console_output_clear();
const std::string& host_console_output();

TEST_F(AcpicaIntegration, ShutdownReportsUnhandledRegionAndMethod)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    auto frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    AcpicaScopedReset cleanup;
    build_tables(arena, true, false);
    auto* table = reinterpret_cast<ACPI_TABLE_FADT*>(arena.data() + kFadtPhysical);
    table->Header.Length = sizeof(*table);
    table->Pm1ControlLength = 2;
    table->XPm1aControlBlock = {ACPI_ADR_SPACE_SYSTEM_MEMORY, 16, 0, 2, kOslMemoryPhysical};
    *reinterpret_cast<uint16_t*>(arena.data() + kOslMemoryPhysical) = 1;
    finalize_sdt(*reinterpret_cast<TestSdtHeader*>(table));
    // Name (_S5, Package (2) {5,5})
    // OperationRegion (ERAM, EmbeddedControl, 0, 16)
    // Field (ERAM, ByteAcc, NoLock, Preserve) {TEST, 8}
    // Method (_PTS, 1) {Store (One, TEST)}
    const uint8_t aml[] = {0x08, '_',  'S', '5', '_', 0x12, 0x06, 0x02, 0x0A, 5,   0x0A, 5,
                           0x5B, 0x80, 'E', 'R', 'A', 'M',  3,    0,    0x0A, 16,  0x5B, 0x81,
                           0x0B, 'E',  'R', 'A', 'M', 1,    'T',  'E',  'S',  'T', 8,    0x14,
                           0x0C, '_',  'P', 'T', 'S', 1,    0x70, 1,    'T',  'E', 'S',  'T'};
    auto* dsdt = reinterpret_cast<TestSdtHeader*>(arena.data() + kDsdtPhysical);
    dsdt->length = sizeof(*dsdt) + sizeof(aml);
    std::memcpy(dsdt + 1, aml, sizeof(aml));
    finalize_sdt(*dsdt);
    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info(kRsdpPhysical)));
    for(unsigned repeat = 0; repeat < 2; ++repeat)
    {
        host_console_output_clear();
        EXPECT_EQ(OS1_ERR_UNSUPPORTED, system_control(OS1_SYSTEM_SHUTDOWN));
        const auto& output = host_console_output();
        EXPECT_NE(std::string::npos, output.find("No handler for Region [ERAM]"));
        EXPECT_NE(std::string::npos, output.find("EmbeddedControl"));
        EXPECT_NE(std::string::npos, output.find("\\_PTS"));
        EXPECT_NE(std::string::npos, output.find("AE_NOT_EXIST"));
        EXPECT_NE(std::string::npos, output.find("stage=s5-preparation detail=AE_NOT_EXIST"));
    }
}

TEST_F(AcpicaIntegration, DiagnosticCaptureIsScopedAndBounded)
{
    host_console_output_clear();
    AcpiOsPrintf("outside capture\n");
    EXPECT_TRUE(host_console_output().empty());
    {
        AcpicaDiagnosticCapture discarded;
        AcpiOsPrintf("discard me\n");
    }
    {
        AcpicaDiagnosticCapture capture;
        AcpiOsPrintf("region=%s code=%8.8X value=%u\n", "ERAM", 0x1001, 7);
        {
            AcpicaDiagnosticCapture nested;
            nested.report();  // Must not detach or clear the outer capture.
        }
        EXPECT_TRUE(host_console_output().empty());
        capture.report();
        EXPECT_EQ("acpica: shutdown diagnostics\nregion=ERAM code=00001001 value=7\n",
                  host_console_output());
        capture.report();  // No duplicate report.
        EXPECT_EQ("acpica: shutdown diagnostics\nregion=ERAM code=00001001 value=7\n",
                  host_console_output());
    }
    host_console_output_clear();
    {
        AcpicaDiagnosticCapture capture;
        std::string oversized(600, 'x');
        AcpiOsPrintf("%s", oversized.c_str());
        capture.report();
    }
    EXPECT_NE(std::string::npos, host_console_output().find("output truncated"));
    EXPECT_LT(host_console_output().size(), 400u);
    host_console_output_clear();
    {
        AcpicaDiagnosticCapture capture;
        for(unsigned i = 0; i < 100; ++i)
            AcpiOsPrintf("line %u: bounded output\n", i);
        AcpiOsPrintf("final failure\n");
        capture.report();
    }
    EXPECT_NE(std::string::npos, host_console_output().find("output truncated"));
    EXPECT_NE(std::string::npos, host_console_output().find("final failure"));
    EXPECT_LT(host_console_output().size(), 2200u);
}

TEST_F(AcpicaIntegration, DiagnosticCaptureExcludesOtherThread)
{
    // Simulate a CPU switch while capture is attached to the requesting thread.
    cpu owner{}, other{};
    int owner_token, other_token;
    owner.current_thread = reinterpret_cast<Thread*>(&owner_token);
    other.current_thread = reinterpret_cast<Thread*>(&other_token);
    auto* saved = g_cpu_host_current;
    g_cpu_host_current = &owner;
    host_console_output_clear();
    {
        AcpicaDiagnosticCapture capture;
        AcpiOsPrintf("owner before\n");
        g_cpu_host_current = &other;
        AcpiOsPrintf("unrelated error\n");
        g_cpu_host_current = &owner;
        AcpiOsPrintf("owner after\n");
        capture.report();
    }
    g_cpu_host_current = saved;
    EXPECT_EQ("acpica: shutdown diagnostics\nowner before\nowner after\n", host_console_output());
}

namespace
{
using Aml = std::vector<uint8_t>;
void append(Aml& out, const Aml& bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}
Aml package(Aml opcode, const Aml& body)
{
    size_t size = body.size() + (body.size() + 1 < 64 ? 1 : 2);
    if(size < 64)
        opcode.push_back(static_cast<uint8_t>(size));
    else
    {
        opcode.push_back(static_cast<uint8_t>(0x40 | (size & 15)));
        opcode.push_back(static_cast<uint8_t>(size >> 4));
    }
    append(opcode, body);
    return opcode;
}
void pci_route_tables(os1::host_test::PhysicalMemoryArena& arena, bool broken_pic = false)
{
    build_tables(arena, true, false);
    // _PIC(Arg0) stores the selected interrupt model. _PRT reads that value.
    Aml aml{0x08, 'P', 'M', 'O', 'D', 0};
    append(aml,
           package({0x14},
                   {'_',
                    'P',
                    'I',
                    'C',
                    1,
                    0x70,
                    0x68,
                    broken_pic ? uint8_t('X') : uint8_t('P'),
                    'M',
                    'O',
                    'D'}));
    auto route = [](uint8_t gsi) {
        Aml entry{4, 0x0c, 0xff, 0xff, 1, 0, 0, 0, 0x0a, gsi};
        Aml entries{1};
        append(entries, package({0x12}, entry));
        return package({0x12}, entries);
    };
    Aml body{'P', 'C', 'I', '0', 0x08, '_', 'H',  'I', 'D', 0x0d, 'P', 'N',
             'P', '0', 'A', '0', '8',  0,   0x08, '_', 'B', 'B',  'N', 0};
    Aml prt{'_', 'P', 'R', 'T', 0};
    Aml apic{0x5c, 'P', 'M', 'O', 'D', 0xa4};
    append(apic, route(16));
    append(prt, package({0xa0}, apic));
    prt.push_back(0xa4);
    append(prt, route(9));
    append(body, package({0x14}, prt));
    append(aml, package({0x5b, 0x82}, body));
    auto* dsdt = reinterpret_cast<TestSdtHeader*>(arena.data() + kDsdtPhysical);
    dsdt->length = sizeof(*dsdt) + aml.size();
    std::memcpy(dsdt + 1, aml.data(), aml.size());
    finalize_sdt(*dsdt);
}

void ec_tables(os1::host_test::PhysicalMemoryArena& arena,
               int glk = 0,
               uint16_t data_port = 0x62,
               uint16_t command_port = 0x66,
               bool fixed = true,
               bool duplicate = false,
               int sta = -1,
               unsigned reg_mode = 0,
               bool orphan = false)
{
    build_tables(arena, true, false);
    auto* table = reinterpret_cast<ACPI_TABLE_FADT*>(arena.data() + kFadtPhysical);
    table->Header.Length = sizeof(*table);
    table->Pm1ControlLength = 2;
    table->XPm1aControlBlock = {ACPI_ADR_SPACE_SYSTEM_MEMORY, 16, 0, 2, kOslMemoryPhysical};
    *reinterpret_cast<uint16_t*>(arena.data() + kOslMemoryPhysical) = 1;
    finalize_sdt(*reinterpret_cast<TestSdtHeader*>(table));
    Aml aml{0x08, 'R', 'G', 'O', 'N', 0, 0x08, '_', 'S', '5', '_'};
    append(aml, package({0x12}, {2, 0x0A, 5, 0x0A, 5}));
    Aml resources;
    for(auto port : {data_port, command_port})
    {
        if(fixed)
            append(resources, {0x4B, uint8_t(port), uint8_t(port >> 8), 1});
        else
            append(resources,
                   {0x47,
                    1,
                    uint8_t(port),
                    uint8_t(port >> 8),
                    uint8_t(port),
                    uint8_t(port >> 8),
                    1,
                    1});
    }
    append(resources, {0x79, 0});
    for(unsigned index = 0; index < (duplicate ? 2u : 1u); ++index)
    {
        Aml body{'E',
                 'C',
                 uint8_t('0' + index),
                 '_',
                 0x08,
                 '_',
                 'H',
                 'I',
                 'D',
                 0x0D,
                 'P',
                 'N',
                 'P',
                 '0',
                 'C',
                 '0',
                 '9',
                 0};
        if(sta >= 0)
            append(body, {0x08, '_', 'S', 'T', 'A', 0x0A, uint8_t(sta)});
        if(glk >= 0)
            append(body, {0x08, '_', 'G', 'L', 'K', 0x0A, uint8_t(glk)});
        append(body, {0x08, '_', 'C', 'R', 'S'});
        Aml buffer{0x0A, uint8_t(resources.size())};
        append(buffer, resources);
        append(body, package({0x11}, buffer));
        if(!orphan)
        {
            // OperationRegion (ECOR, EmbeddedControl, Zero, 256)
            append(body, {0x5B, 0x80, 'E', 'C', 'O', 'R', 3, 0, 0x0B, 0, 1});
            append(body, package({0x5B, 0x81}, {'E', 'C', 'O', 'R', 1, 'D', 'A', 'T', 'A', 16}));
        }
        Aml reg{'_', 'R', 'E', 'G', 2};
        if(reg_mode == 1)
            append(reg, {0x70, 'M', 'I', 'S', 'S', 0x60});  // Missing AML name.
        if(reg_mode == 2)
            append(reg, {0x70, 1, 'D', 'A', 'T', 'A'});
        append(reg, {0x70, 0x69, 0x5C, 'R', 'G', 'O', 'N'});
        append(body, package({0x14}, reg));
        append(body, package({0x14}, {'R', 'E', 'A', 'D', 0, 0xA4, 'D', 'A', 'T', 'A'}));
        append(body, package({0x14}, {'W', 'R', 'I', 'T', 1, 0x70, 0x68, 'D', 'A', 'T', 'A'}));
        append(aml, package({0x5B, 0x82}, body));
    }
    // _PTS writes One to the first controller's DATA field.
    append(
        aml,
        package(
            {0x14},
            {'_', 'P', 'T', 'S', 1, 0x70, 1, 0x5C, 0x2E, 'E', 'C', '0', '_', 'D', 'A', 'T', 'A'}));
    auto* dsdt = reinterpret_cast<TestSdtHeader*>(arena.data() + kDsdtPhysical);
    dsdt->length = sizeof(*dsdt) + aml.size();
    std::memcpy(dsdt + 1, aml.data(), aml.size());
    finalize_sdt(*dsdt);
}
struct EcDevice
{
    std::array<uint8_t, 256> memory{};
    uint16_t data_port = 0x62, command_port = 0x66;
    enum Phase
    {
        Idle,
        ReadAddress,
        WriteAddress,
        WriteData
    } phase = Idle;
    uint8_t address{}, response{};
    bool ready{}, stuck_input{}, drop_response{}, stick_after_write{}, nested{};
    int fail_read_address = -1;
    unsigned stale{}, reads{}, writes{}, delay{}, nested_status = AE_OK;
    std::vector<std::pair<uint16_t, uint8_t>> sent;
    static uint32_t read(void* context, uint16_t port, uint8_t& value)
    {
        auto& ec = *static_cast<EcDevice*>(context);
        ++ec.reads;
        if(port == ec.command_port)
        {
            value = uint8_t((ec.stuck_input ? 2 : 0) | ((ec.ready || ec.stale) ? 1 : 0));
            return AE_OK;
        }
        if(port != ec.data_port)
            return AE_BAD_PARAMETER;
        if(ec.stale)
        {
            --ec.stale;
            value = 0xDD;
            return AE_OK;
        }
        if(!ec.ready || ec.address == ec.fail_read_address)
            return AE_IO_ERROR;
        value = ec.response;
        ec.ready = false;
        return AE_OK;
    }
    static uint32_t write(void* context, uint16_t port, uint8_t value)
    {
        auto& ec = *static_cast<EcDevice*>(context);
        ++ec.writes;
        ec.sent.emplace_back(port, value);
        if(port == ec.command_port)
        {
            if(ec.phase != Idle || (value != 0x80 && value != 0x81))
                return AE_BAD_PARAMETER;
            if(ec.nested)
            {
                ec.nested = false;
                uint64_t other = 0;
                ec.nested_status = acpica_ec_test_access(0, ACPI_READ, 0, 8, other);
            }
            ec.phase = value == 0x80 ? ReadAddress : WriteAddress;
            return AE_OK;
        }
        if(port != ec.data_port)
            return AE_BAD_PARAMETER;
        if(ec.phase == ReadAddress)
        {
            ec.address = value;
            ec.response = ec.memory[value];
            ec.ready = !ec.drop_response;
            ec.phase = Idle;
        }
        else if(ec.phase == WriteAddress)
        {
            ec.address = value;
            ec.phase = WriteData;
        }
        else if(ec.phase == WriteData)
        {
            ec.memory[ec.address] = value;
            ec.phase = Idle;
            ec.stuck_input = ec.stick_after_write;
        }
        else
            return AE_BAD_PARAMETER;
        return AE_OK;
    }
    static void stall(void* context, uint32_t us)
    {
        static_cast<EcDevice*>(context)->delay += us;
    }
    void attach()
    {
        acpica_ec_set_test_io({this, read, write, stall});
    }
};
}  // namespace

TEST_F(AcpicaIntegration, SelectsApicModeAndKeepsDirectPciRoutesLevelLow)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    pci_route_tables(arena);
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    const auto boot = make_boot_info(kRsdpPhysical);
    AcpicaScopedReset reset;
    ASSERT_TRUE(acpica_initialize_tables(vm, boot));
    ASSERT_TRUE(acpica_load_namespace());

    AcpiDeviceInfo root{};
    root.active = true;
    root.bus_number = 0;
    std::strcpy(root.path, "\\PCI0");
    g_platform.acpi_devices = &root;
    g_platform.acpi_device_count = 1;
    auto check_route = [] {
        uint32_t irq = 0;
        uint16_t flags = 0;
        bool direct = false;
        ASSERT_TRUE(acpica_resolve_pci_route_details(0, 1, 0, 0, irq, flags, direct));
        EXPECT_EQ(irq, 16u);
        EXPECT_EQ(flags, 0x0fu);
        EXPECT_TRUE(direct);
    };
    check_route();

    std::vector<AcpiDeviceInfo> devices(kAcpiMaxDevices);
    std::vector<AcpiPciRoute> routes(kAcpiMaxPciRoutes);
    size_t device_count = 0, route_count = 0;
    ASSERT_TRUE(acpica_build_device_info(devices.data(), device_count, routes.data(), route_count));
    ASSERT_EQ(route_count, 1u);
    EXPECT_EQ(routes[0].irq, 16u);
    EXPECT_EQ(routes[0].flags, 0x0fu);
    EXPECT_TRUE(routes[0].source_is_gsi);
    g_platform.acpi_device_count = 0;  // Verify the cached path without the namespace fallback.
    check_route();
}

TEST_F(AcpicaIntegration, ReportsFailureFromPresentPicMethod)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    pci_route_tables(arena, true);
    PageFrameContainer frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    const auto boot = make_boot_info(kRsdpPhysical);
    AcpicaScopedReset reset;
    ASSERT_TRUE(acpica_initialize_tables(vm, boot));
    EXPECT_FALSE(acpica_load_namespace());
    EXPECT_NE(std::string(acpica_namespace_last_error()), "ok");
}


TEST_F(AcpicaIntegration, EcConnectsRegionAndExecutesShutdownFirmware)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    auto frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    EcDevice ec;
    AcpicaScopedReset cleanup;
    ec_tables(arena);
    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info(kRsdpPhysical)));
    ec.attach();
    ASSERT_TRUE(acpica_load_namespace());
    EXPECT_EQ(0u, ec.reads + ec.writes);  // Ordinary namespace activation does not probe EC ports.
    uint64_t connected = 42;
    ASSERT_TRUE(acpica_read_named_integer("\\RGON", connected));
    EXPECT_EQ(0u, connected);
    PowerDiagnostic diagnostic;
    ASSERT_TRUE(acpica_prepare_shutdown(diagnostic))
        << diagnostic.stage << ": " << diagnostic.detail;
    ASSERT_TRUE(acpica_read_named_integer("\\RGON", connected));
    EXPECT_EQ(1u, connected);
    EXPECT_EQ(1u, ec.memory[0]);
    EXPECT_EQ(0u, ec.memory[1]);
    const std::vector<std::pair<uint16_t, uint8_t>> expected{
        {0x66, 0x81}, {0x62, 0}, {0x62, 1}, {0x66, 0x81}, {0x62, 1}, {0x62, 0}};
    EXPECT_EQ(expected, ec.sent);
    ec.memory[0] = 0x34;
    ec.memory[1] = 0x12;
    ASSERT_TRUE(acpica_read_named_integer("\\EC0.READ", connected));
    EXPECT_EQ(0x1234u, connected);
    ASSERT_TRUE(
        acpica_prepare_shutdown(diagnostic));  // Persistent handler, no duplicate installation.
}

TEST_F(AcpicaIntegration, EcValidatesAccessAndSerializesTransactions)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    auto frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    EcDevice ec;
    AcpicaScopedReset cleanup;
    ec_tables(arena, -1, 0x164, 0x168, false);
    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info(kRsdpPhysical)));
    ec.data_port = 0x164;
    ec.command_port = 0x168;
    ec.attach();
    ASSERT_TRUE(acpica_load_namespace());
    ASSERT_EQ(AE_OK, acpica_ec_initialize());
    uint64_t value = 0x8877665544332211ull;
    EXPECT_EQ(AE_BAD_PARAMETER, acpica_ec_test_access(0, ACPI_WRITE, 255, 16, value));
    EXPECT_EQ(AE_BAD_PARAMETER, acpica_ec_test_access(0, 2, 0, 8, value));
    EXPECT_EQ(AE_BAD_PARAMETER, acpica_ec_test_access(0, ACPI_READ, 0, 7, value));
    EXPECT_EQ(0u, ec.reads + ec.writes);
    ec.nested = true;
    ASSERT_EQ(AE_OK, acpica_ec_test_access(0, ACPI_WRITE, 248, 64, value));
    EXPECT_EQ(AE_TIME, ec.nested_status);
    EXPECT_EQ(0x11u, ec.memory[248]);
    EXPECT_EQ(0x88u, ec.memory[255]);
    value = 0;
    ec.stale = 1;
    ASSERT_EQ(AE_OK, acpica_ec_test_access(0, ACPI_READ, 248, 64, value));
    EXPECT_EQ(0x8877665544332211ull, value);
}

TEST_F(AcpicaIntegration, EcRejectsUnsafeResourcesWithoutPortAccess)
{
    for(unsigned scenario = 0; scenario < 4; ++scenario)
    {
        os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
        auto frames = make_frames();
        kmem_init(frames);
        VirtualMemory vm(frames);
        EcDevice ec;
        AcpicaScopedReset cleanup;
        ec_tables(arena,
                  scenario == 0 ? 1 : 0,
                  scenario == 1 ? 0 : 0x62,
                  scenario == 2 ? 0x62 : 0x66,
                  true,
                  scenario == 3);
        ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info(kRsdpPhysical)));
        ec.attach();
        ASSERT_TRUE(acpica_load_namespace());
        EXPECT_NE(AE_OK, acpica_ec_initialize());
        EXPECT_EQ(0u, ec.reads + ec.writes);
        EXPECT_NE(AE_OK, acpica_ec_initialize());
    }
}

TEST_F(AcpicaIntegration, EcTimeoutsAreBoundedAndDoNotRetryUncertainTransactions)
{
    for(unsigned scenario = 0; scenario < 5; ++scenario)
    {
        os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
        auto frames = make_frames();
        kmem_init(frames);
        VirtualMemory vm(frames);
        EcDevice ec;
        AcpicaScopedReset cleanup;
        ec_tables(arena);
        ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info(kRsdpPhysical)));
        ec.attach();
        ASSERT_TRUE(acpica_load_namespace());
        ASSERT_EQ(AE_OK, acpica_ec_initialize());
        ec.stuck_input = scenario == 0;
        ec.drop_response = scenario == 1;
        ec.stick_after_write = scenario == 2;
        ec.stale = scenario == 3 ? 17 : 0;
        ec.fail_read_address = scenario == 4 ? 1 : -1;
        uint64_t value = 0xBEEF;
        const auto function = scenario == 2 ? ACPI_WRITE : ACPI_READ;
        const auto status = acpica_ec_test_access(0, function, 0, 16, value);
        EXPECT_EQ(scenario == 4 ? AE_IO_ERROR : AE_TIME, status);
        if(function == ACPI_READ)
            EXPECT_EQ(0xBEEFu, value);
        EXPECT_LE(ec.delay, 100006u);
        const auto calls = ec.reads + ec.writes;
        EXPECT_EQ(status, acpica_ec_test_access(0, ACPI_READ, 0, 8, value));
        EXPECT_EQ(calls, ec.reads + ec.writes);
    }
}

TEST_F(AcpicaIntegration, EcConnectionErrorsBlockSleepPreparation)
{
    for(unsigned scenario = 0; scenario < 3; ++scenario)
    {
        os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
        auto frames = make_frames();
        kmem_init(frames);
        VirtualMemory vm(frames);
        EcDevice ec;
        AcpicaScopedReset cleanup;
        ec_tables(arena, 0, 0x62, 0x66, true, false, -1, scenario == 2 ? 2 : 1, scenario == 1);
        ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info(kRsdpPhysical)));
        ec.stuck_input = scenario == 2;
        ec.attach();
        PowerDiagnostic diagnostic;
        EXPECT_FALSE(acpica_prepare_shutdown(diagnostic));
        EXPECT_STREQ("ec-initialization", diagnostic.stage);
        EXPECT_EQ(0u, ec.memory[0]);  // _PTS must not execute after a failed connection.
        const auto calls = ec.reads + ec.writes;
        EXPECT_FALSE(acpica_prepare_shutdown(diagnostic));
        EXPECT_EQ(calls, ec.reads + ec.writes);
    }
}

TEST_F(AcpicaIntegration, EcSkipsDisabledController)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    auto frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    EcDevice ec;
    AcpicaScopedReset cleanup;
    ec_tables(arena, 0, 0x62, 0x66, true, false, 9);
    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info(kRsdpPhysical)));
    ec.attach();
    ASSERT_TRUE(acpica_load_namespace());
    ASSERT_EQ(AE_OK, acpica_ec_initialize());
    uint64_t value = 0;
    EXPECT_EQ(AE_NOT_EXIST, acpica_ec_test_access(0, ACPI_READ, 0, 8, value));
    EXPECT_EQ(0u, ec.reads + ec.writes);
}

TEST_F(AcpicaIntegration, EcConnectsOrphanRegOnce)
{
    os1::host_test::PhysicalMemoryArena arena(kArenaBytes);
    auto frames = make_frames();
    kmem_init(frames);
    VirtualMemory vm(frames);
    EcDevice ec;
    AcpicaScopedReset cleanup;
    ec_tables(arena, 0, 0x62, 0x66, true, false, -1, 0, true);
    ASSERT_TRUE(acpica_initialize_tables(vm, make_boot_info(kRsdpPhysical)));
    ec.attach();
    ASSERT_TRUE(acpica_load_namespace());
    ASSERT_EQ(AE_OK, acpica_ec_initialize());
    uint64_t connected = 0;
    ASSERT_TRUE(acpica_read_named_integer("\\RGON", connected));
    EXPECT_EQ(1u, connected);
    EXPECT_EQ(0u, ec.reads + ec.writes);
    ASSERT_EQ(AE_OK, acpica_ec_initialize());
}
