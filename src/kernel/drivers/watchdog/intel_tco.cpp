// Intel TCO watchdog: LPC bridge discovery through legacy PCI config ports,
// NO_REBOOT clearing in the chipset GCS register, and TCO timer programming.
#include "drivers/watchdog/intel_tco.hpp"

#include "arch/x86_64/cpu/io_port.hpp"
#include "debug/debug.hpp"
#include "handoff/memory_layout.h"
#include "mm/boot_mapping.hpp"
#include "mm/virtual_memory.hpp"

namespace
{
// LPC bridge at 00:1f.0 and its configuration registers.
constexpr uint8_t kLpcDevice = 31;
constexpr uint16_t kIntelVendorId = 0x8086;
constexpr uint16_t kPciClassIsaBridge = 0x0601;
constexpr uint8_t kLpcPmBase = 0x40;
constexpr uint8_t kLpcAcpiControl = 0x44;
constexpr uint8_t kLpcRcba = 0xF0;
constexpr uint8_t kLpcAcpiEnable = 0x80;

// Chipset configuration (RCBA) General Control and Status register.
constexpr uint64_t kRcbaGcsOffset = 0x3410;
constexpr uint32_t kGcsNoReboot = 1u << 5;

// ACPI PM I/O block and the TCO block inside it.
constexpr uint16_t kSmiEnableOffset = 0x30;
constexpr uint32_t kSmiEnableTco = 1u << 13;
constexpr uint16_t kTcoBlockOffset = 0x60;
constexpr uint16_t kTcoReload = 0x00;
constexpr uint16_t kTco1Status = 0x04;
constexpr uint16_t kTco2Status = 0x06;
constexpr uint16_t kTco1Control = 0x08;
constexpr uint16_t kTcoTimer = 0x12;
constexpr uint16_t kTco1StatusTimeout = 1u << 3;
constexpr uint16_t kTco2StatusSecondTimeout = 1u << 1;
constexpr uint16_t kTco2StatusBoot = 1u << 2;
constexpr uint16_t kTco1ControlHalt = 1u << 11;
constexpr uint16_t kTco1ControlLock = 1u << 12;
constexpr uint16_t kTcoTimerMask = 0x03FF;
constexpr uint16_t kTcoTimerMinimumTicks = 4;

bool g_armed = false;
uint16_t g_tco_base = 0;

uint32_t pci_config_read32_legacy(uint8_t device, uint8_t offset)
{
    outl(0xCF8, 0x80000000u | (static_cast<uint32_t>(device) << 11) | (offset & 0xFCu));
    return inl(0xCFC);
}

uint16_t tco_ticks(uint32_t stage_seconds)
{
    // One TCO tick is 0.6 s.
    uint32_t ticks = (stage_seconds * 10u) / 6u;
    if(ticks < kTcoTimerMinimumTicks)
    {
        ticks = kTcoTimerMinimumTicks;
    }
    if(ticks > kTcoTimerMask)
    {
        ticks = kTcoTimerMask;
    }
    return static_cast<uint16_t>(ticks);
}

void clear_timeout_status()
{
    outw(g_tco_base + kTco1Status, kTco1StatusTimeout);
    outw(g_tco_base + kTco2Status, kTco2StatusSecondTimeout | kTco2StatusBoot);
}

void program_timer(uint16_t ticks)
{
    const uint16_t timer = inw(g_tco_base + kTcoTimer);
    outw(g_tco_base + kTcoTimer, static_cast<uint16_t>((timer & ~kTcoTimerMask) | ticks));
    clear_timeout_status();
    outw(g_tco_base + kTcoReload, 1);
}
}  // namespace

bool intel_tco_watchdog_arm(VirtualMemory& kernel_vm, uint32_t stage_seconds)
{
    const uint32_t id = pci_config_read32_legacy(kLpcDevice, 0x00);
    const uint32_t class_word = pci_config_read32_legacy(kLpcDevice, 0x08);
    if(((id & 0xFFFFu) != kIntelVendorId) || ((class_word >> 16) != kPciClassIsaBridge))
    {
        debug("tco: no Intel LPC bridge at 00:1f.0 id=0x")(id, 16, 8)();
        return false;
    }

    const uint32_t pm_base_register = pci_config_read32_legacy(kLpcDevice, kLpcPmBase);
    const uint32_t acpi_control = pci_config_read32_legacy(kLpcDevice, kLpcAcpiControl);
    const uint16_t pm_base = static_cast<uint16_t>(pm_base_register & 0xFF80u);
    const uint32_t rcba_register = pci_config_read32_legacy(kLpcDevice, kLpcRcba);
    debug("tco: lpc id=0x")(id, 16, 8)(" pmbase=0x")(pm_base, 16, 4)(" acpi_cntl=0x")(
        acpi_control & 0xFFu, 16, 2)(" rcba=0x")(rcba_register, 16, 8)();
    if((0u == pm_base) || (0u == (acpi_control & kLpcAcpiEnable)) || (0u == (rcba_register & 1u)))
    {
        debug("tco: PM I/O block or RCBA not enabled")();
        return false;
    }

    const uint64_t gcs_physical = static_cast<uint64_t>(rcba_register & 0xFFFFC000u) + kRcbaGcsOffset;
    if(!map_mmio_range(kernel_vm, gcs_physical & ~(kPageSize - 1), kPageSize))
    {
        debug("tco: RCBA map failed")();
        return false;
    }
    volatile uint32_t* gcs = kernel_physical_pointer<volatile uint32_t>(gcs_physical);
    const uint32_t gcs_before = *gcs;
    if(0u != (gcs_before & kGcsNoReboot))
    {
        *gcs = gcs_before & ~kGcsNoReboot;
    }
    const uint32_t gcs_after = *gcs;
    debug("tco: gcs 0x")(gcs_before, 16, 8)(" -> 0x")(gcs_after, 16, 8)();
    if(0u != (gcs_after & kGcsNoReboot))
    {
        debug("tco: NO_REBOOT is strapped/locked; watchdog cannot reset")();
        return false;
    }

    // Keep firmware SMM from servicing (and silently reloading) the timeout.
    const uint32_t smi_enable = inl(pm_base + kSmiEnableOffset);
    outl(pm_base + kSmiEnableOffset, smi_enable & ~kSmiEnableTco);

    g_tco_base = static_cast<uint16_t>(pm_base + kTcoBlockOffset);
    const uint16_t control = inw(g_tco_base + kTco1Control);
    outw(g_tco_base + kTco1Control, static_cast<uint16_t>(control | kTco1ControlHalt));
    program_timer(tco_ticks(stage_seconds));
    outw(g_tco_base + kTco1Control, static_cast<uint16_t>(control & ~kTco1ControlHalt));

    const uint16_t control_after = inw(g_tco_base + kTco1Control);
    debug("tco: smi_en 0x")(smi_enable, 16, 8)(" -> 0x")(inl(pm_base + kSmiEnableOffset), 16, 8)(
        " tco1_cnt 0x")(control, 16, 4)(" -> 0x")(control_after, 16, 4)();
    if(0u != (control_after & kTco1ControlHalt))
    {
        debug("tco: timer halt bit stuck")((0u != (control & kTco1ControlLock)) ? " (TCO_LOCK)" : "")();
        return false;
    }

    g_armed = true;
    debug("tco: armed, platform reset after ~")(2u * stage_seconds)(" s without progress")();
    return true;
}

void intel_tco_watchdog_kick()
{
    if(g_armed)
    {
        clear_timeout_status();
        outw(g_tco_base + kTcoReload, 1);
    }
}

void intel_tco_watchdog_set_timeout(uint32_t stage_seconds)
{
    if(g_armed)
    {
        program_timer(tco_ticks(stage_seconds));
    }
}

bool intel_tco_watchdog_armed()
{
    return g_armed;
}
