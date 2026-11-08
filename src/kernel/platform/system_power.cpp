#include "platform/system_power.hpp"

#include <stddef.h>

#include "platform/acpica_ec.hpp"
#include "platform/acpica_integration.hpp"
extern "C"
{
#include "acpi.h"
}
namespace
{
ACPI_GENERIC_ADDRESS control_a{}, control_b{};
UINT8 type_a{}, type_b{};

bool control_register(const ACPI_TABLE_FADT& table, bool second, ACPI_GENERIC_ADDRESS& out)
{
    const auto offset = second ? offsetof(ACPI_TABLE_FADT, XPm1bControlBlock)
                               : offsetof(ACPI_TABLE_FADT, XPm1aControlBlock);
    if(table.Header.Length >= offset + sizeof(out))
        out = second ? table.XPm1bControlBlock : table.XPm1aControlBlock;
    if(!out.Address)
    {
        if(table.Pm1ControlLength != 2 && table.Pm1ControlLength != 4)
            return false;
        out = {};
        out.SpaceId = ACPI_ADR_SPACE_SYSTEM_IO;
        out.Address = second ? table.Pm1bControlBlock : table.Pm1aControlBlock;
        out.BitWidth = table.Pm1ControlLength * 8;
    }
    return (!out.Address && second) ||
           (out.Address && !out.BitOffset && (out.BitWidth == 16 || out.BitWidth == 32) &&
            (out.SpaceId == ACPI_ADR_SPACE_SYSTEM_IO ||
             out.SpaceId == ACPI_ADR_SPACE_SYSTEM_MEMORY));
}
bool failure(PowerDiagnostic& diagnostic, const char* stage, const char* detail)
{
    diagnostic = {stage, detail};
    return false;
}
bool checked(PowerDiagnostic& diagnostic, const char* stage, ACPI_STATUS status)
{
    return ACPI_SUCCESS(status) || failure(diagnostic, stage, AcpiFormatException(status));
}
bool prepare_controls(const ACPI_TABLE_FADT& table, PowerDiagnostic& diagnostic)
{
    if(table.Flags & ACPI_FADT_HW_REDUCED)
        return failure(diagnostic, "fadt-profile", "hardware-reduced-unsupported");
    if(!control_register(table, false, control_a))
        return failure(diagnostic, "pm1a-control", "invalid-register");
    if(!control_register(table, true, control_b))
        return failure(diagnostic, "pm1b-control", "invalid-register");
    UINT64 value = 0;
    if(!checked(diagnostic, "pm1a-read", AcpiRead(&value, &control_a)))
        return false;
    if(value & 1)
        return true;
    if(!table.SmiCommand || !table.AcpiEnable)
        return failure(diagnostic, "acpi-enable", "missing-smi-command");
    if(!checked(
           diagnostic, "acpi-enable-write", AcpiOsWritePort(table.SmiCommand, table.AcpiEnable, 8)))
        return false;
    for(unsigned attempt = 0; !(value & 1) && attempt < 10000; ++attempt)
    {
        AcpiOsStall(100);
        if(!checked(diagnostic, "acpi-enable-read", AcpiRead(&value, &control_a)))
            return false;
    }
    return (value & 1) || failure(diagnostic, "acpi-enable", "SCI_EN-timeout");
}
}  // namespace
// ACPICA remains built with ACPI_REDUCED_HARDWARE=1 for passive platform discovery.
// Use its public AML and GAS helpers for explicit legacy S5 register access.
bool acpica_prepare_shutdown(PowerDiagnostic& diagnostic)
{
    diagnostic = {};
    if(!acpica_tables_initialized())
        return failure(diagnostic, "acpi-tables", "not-initialized");
    ACPI_TABLE_HEADER* header = nullptr;
    if(!checked(diagnostic, "fadt-lookup", AcpiGetTable(ACPI_SIG_FADT, 1, &header)))
        return false;
    if(!header)
        return failure(diagnostic, "fadt-lookup", "missing-table");
    if(header->Length < offsetof(ACPI_TABLE_FADT, Flags) + sizeof(UINT32))
    {
        AcpiPutTable(header);
        return failure(diagnostic, "fadt-length", "truncated-table");
    }
    control_a = {};
    control_b = {};
    bool valid = prepare_controls(*reinterpret_cast<const ACPI_TABLE_FADT*>(header), diagnostic);
    AcpiPutTable(header);
    if(!valid)
        return false;
    if(!acpica_load_namespace())
        return failure(diagnostic, "acpi-namespace", acpica_last_status());
    if(ACPI_FAILURE(acpica_ec_initialize()))
        return failure(diagnostic, "ec-initialization", acpica_ec_last_error());
    if(!checked(diagnostic, "s5-types", AcpiGetSleepTypeData(ACPI_STATE_S5, &type_a, &type_b)))
        return false;
    if(type_a > 7 || type_b > 7)
        return failure(diagnostic, "s5-types", "invalid-sleep-type");
    return checked(diagnostic, "s5-preparation", AcpiEnterSleepStatePrep(ACPI_STATE_S5));
}
bool acpica_enter_shutdown(PowerDiagnostic& diagnostic)
{
    diagnostic = {};
    UINT64 a = 0, b = 0;
    if(!checked(diagnostic, "pm1a-entry-read", AcpiRead(&a, &control_a)) ||
       (control_b.Address && !checked(diagnostic, "pm1b-entry-read", AcpiRead(&b, &control_b))))
        return false;
    constexpr UINT64 sleep_mask = (UINT64(7) << 10) | (UINT64(1) << 13);
    a = (a & ~sleep_mask) | (UINT64(type_a) << 10);
    b = (b & ~sleep_mask) | (UINT64(type_b) << 10);
    // Write type first, then enable, as required by the ACPI legacy sleep sequence.
    if(!checked(diagnostic, "pm1a-type-write", AcpiWrite(a, &control_a)) ||
       (control_b.Address && !checked(diagnostic, "pm1b-type-write", AcpiWrite(b, &control_b))))
        return false;
    if(!checked(diagnostic, "pm1a-enable-write", AcpiWrite(a | (UINT64(1) << 13), &control_a)))
        return false;
    return !control_b.Address ||
           checked(diagnostic, "pm1b-enable-write", AcpiWrite(b | (UINT64(1) << 13), &control_b));
}
bool acpica_reset_system(PowerDiagnostic& diagnostic)
{
    diagnostic = {};
    if(!acpica_tables_initialized())
        return failure(diagnostic, "acpi-tables", "not-initialized");
    return checked(diagnostic, "reset-register", AcpiReset());
}
