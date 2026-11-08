#include "platform/system_power.hpp"

#include <stddef.h>

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
}  // namespace
// ACPICA remains built with ACPI_REDUCED_HARDWARE=1 for passive platform discovery.
// Use its public AML and GAS helpers for explicit legacy S5 register access.
bool acpica_prepare_shutdown()
{
    if(!acpica_tables_initialized())
        return false;
    ACPI_TABLE_HEADER* header = nullptr;
    if(ACPI_FAILURE(AcpiGetTable(ACPI_SIG_FADT, 1, &header)) || !header)
        return false;
    if(header->Length < offsetof(ACPI_TABLE_FADT, Flags) + sizeof(UINT32))
    {
        AcpiPutTable(header);
        return false;
    }
    const auto& table = *reinterpret_cast<const ACPI_TABLE_FADT*>(header);
    const bool reduced = (table.Flags & ACPI_FADT_HW_REDUCED) != 0;
    control_a = {};
    control_b = {};
    bool valid = !reduced && (control_register(table, false, control_a) &&
                              control_register(table, true, control_b));
    if(valid)
    {
        UINT64 value = 0;
        valid = ACPI_SUCCESS(AcpiRead(&value, &control_a));
        if(valid && !(value & 1))
        {
            valid = table.SmiCommand && table.AcpiEnable &&
                    ACPI_SUCCESS(AcpiOsWritePort(table.SmiCommand, table.AcpiEnable, 8));
            for(unsigned attempt = 0; valid && !(value & 1) && attempt < 10000; ++attempt)
            {
                AcpiOsStall(100);
                valid = ACPI_SUCCESS(AcpiRead(&value, &control_a));
            }
            valid = valid && (value & 1);
        }
    }
    AcpiPutTable(header);
    return valid && acpica_load_namespace() &&
           ACPI_SUCCESS(AcpiGetSleepTypeData(ACPI_STATE_S5, &type_a, &type_b)) && type_a <= 7 &&
           type_b <= 7 && ACPI_SUCCESS(AcpiEnterSleepStatePrep(ACPI_STATE_S5));
}
bool acpica_enter_shutdown()
{
    UINT64 a = 0, b = 0;
    if(ACPI_FAILURE(AcpiRead(&a, &control_a)) ||
       (control_b.Address && ACPI_FAILURE(AcpiRead(&b, &control_b))))
        return false;
    constexpr UINT64 sleep_mask = (UINT64(7) << 10) | (UINT64(1) << 13);
    a = (a & ~sleep_mask) | (UINT64(type_a) << 10);
    b = (b & ~sleep_mask) | (UINT64(type_b) << 10);
    // Write type first, then enable, as required by the ACPI legacy sleep sequence.
    if(ACPI_FAILURE(AcpiWrite(a, &control_a)) ||
       (control_b.Address && ACPI_FAILURE(AcpiWrite(b, &control_b))))
        return false;
    if(ACPI_FAILURE(AcpiWrite(a | (UINT64(1) << 13), &control_a)))
        return false;
    return !control_b.Address || ACPI_SUCCESS(AcpiWrite(b | (UINT64(1) << 13), &control_b));
}
bool acpica_reset_system()
{
    return acpica_tables_initialized() && ACPI_SUCCESS(AcpiReset());
}
