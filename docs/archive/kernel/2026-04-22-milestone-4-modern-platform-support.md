# Milestone 4 Design — Modern Platform Support

> generated-by: Codex (GPT-5) · generated-at: 2026-04-22 · git-commit: `8ccd45bdb088643cc3a963ec1f74fa77dfe6ab33`

## Purpose

This milestone moves `os1` from “boots with a modern loader” to “understands modern machine topology.” The kernel currently depends on legacy Intel MP tables and BIOS-era assumptions. That is enough for emulator bring-up, but not enough for a serious modern hardware path.

The goal here is to adopt the core platform standards used by contemporary PCs and virtual machines:

- ACPI for platform discovery
- PCI Express for device enumeration
- VirtIO for first practical VM devices
- NVMe later, after the PCIe base is stable

## Scope

This document covers:

- ACPI table parsing
- APIC topology discovery from ACPI
- PCIe enumeration
- early driver/resource model
- VirtIO as the first practical device family
- NVMe as a later storage target

This document does not cover:

- USB/xHCI bring-up
- Wi-Fi, GPU, or audio
- power management and sleep states
- advanced ACPI execution such as AML interpreter support

## Jargon And Abbreviations

| Term | Meaning |
| --- | --- |
| ACPI | Advanced Configuration and Power Interface, the standard table-based interface firmware uses to describe modern PC hardware. |
| RSDP | Root System Description Pointer, the entry point into ACPI tables. |
| XSDT | Extended System Description Table, the 64-bit ACPI table that points to other ACPI tables. |
| MADT | Multiple APIC Description Table, the ACPI table that describes CPUs, LAPICs, IOAPICs, and interrupt overrides. |
| MCFG | ACPI table that describes PCI Express ECAM regions for memory-mapped configuration space access. |
| APIC | Advanced Programmable Interrupt Controller, the interrupt-controller architecture used on x86 systems. |
| LAPIC | Local APIC, the interrupt controller local to a CPU core. |
| IOAPIC | I/O APIC, the interrupt controller that routes external interrupts. |
| PCIe | PCI Express, the standard modern peripheral bus. |
| ECAM | Enhanced Configuration Access Mechanism, the memory-mapped method used to access PCIe configuration space. |
| BAR | Base Address Register, the register that tells software where a PCI or PCIe device maps memory or I/O resources. |
| MMIO | Memory-Mapped I/O, device registers accessed as memory addresses. |
| VirtIO | A standard virtual-device interface widely used in virtual machines. |
| NVMe | Non-Volatile Memory Express, the standard modern storage interface for PCIe SSDs. |
| MSI / MSI-X | Message Signaled Interrupts, a modern interrupt delivery mechanism used by PCIe devices. |

## Design Goals

1. Replace legacy MP-table discovery as the primary CPU and interrupt-topology source.
2. Enumerate PCIe devices using standards-based configuration-space access.
3. Build a small but extensible device/resource model.
4. Prefer virtual-machine-friendly standards first for fast progress.
5. Keep the first driver set intentionally small and high-value.

## Industry Standards And Conventions

The milestone will align with these standards and conventions:

- `ACPI` for platform discovery
- `APIC` model as described by ACPI MADT data
- `PCI Express` configuration and BAR discovery
- `VirtIO 1.x` for first practical block and network devices
- `NVMe` for later modern storage support

These are the same broad standards used by modern operating systems and hypervisors.

## Proposed Technical Solution

### 1. Add An ACPI Table Parser

Use `BootInfo.rsdp_physical` as the kernel entry point into ACPI.

Required parser capabilities:

- validate checksums
- read SDT headers safely
- prefer `XSDT` on 64-bit systems
- support `RSDT` only as legacy fallback if desired

First ACPI tables to support:

- `XSDT`
- `MADT`
- `MCFG`

Optional later tables:

- `HPET` for timer work
- `FADT` for selected fixed-platform details
- `SRAT` or NUMA-related tables, but not early

The parser does not need a full AML interpreter. That would be a much larger milestone.

### 2. Use MADT As The Primary Interrupt And CPU Topology Source

The `MADT` should replace Intel MP tables as the primary source for:

- CPU local APIC identifiers
- local APIC base address
- IOAPIC descriptors
- interrupt source overrides

The kernel should keep a legacy MP parser only as fallback while BIOS support remains.

Recommended policy:

- ACPI path first
- MP-table path only if ACPI is absent and the boot path is explicitly legacy

### 3. Normalize Platform Topology Into Kernel-Owned Structures

Add kernel-internal topology structures such as:

```cpp
struct CpuInfo {
    uint32_t apic_id;
    bool enabled;
    bool is_bsp;
};

struct IoApicInfo {
    uint32_t id;
    uint32_t address;
    uint32_t gsi_base;
};

struct InterruptOverride {
    uint8_t bus_irq;
    uint32_t global_irq;
    uint16_t flags;
};
```

After parsing ACPI:

- copy topology data into kernel-owned memory
- stop depending on raw firmware table addresses during steady state

### 4. Add PCIe Enumeration Via MCFG And ECAM

Use `MCFG` to discover ECAM ranges. Then enumerate buses, devices, and functions using memory-mapped config-space access.

Current implementation note:

- the kernel now reaches ACPI tables, ECAM space, LAPIC / IOAPIC registers, and device BARs through explicit direct-map or MMIO helpers rather than assuming broad identity-mapped physical access
- platform and device records keep physical addresses as ownership data, while CPU-side code materializes virtual pointers explicitly when it needs to touch registers or memory-backed descriptors

The first PCIe layer should support:

- reading vendor ID and device ID
- class code and subclass
- BAR discovery and sizing
- header type
- capability list access

Recommended fallback policy:

- use `MCFG` and ECAM first
- optional legacy `0xCF8/0xCFC` config I/O fallback only if BIOS compatibility still matters later

### 5. Introduce A Minimal Device Manager

Do not jump directly from “enumeration” to “drivers everywhere.” Add a small device model first.

Suggested responsibilities:

- hold discovered device descriptors
- expose BAR resources
- expose interrupt routing information
- match devices to driver probe functions

Example device record:

```cpp
struct PciDevice {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
};
```

This should be small and predictable, not a full Linux-style driver core.

### 6. Prefer VirtIO Devices First

Start with:

- `virtio-blk` for block storage
- `virtio-net` for networking only after block is stable

Why VirtIO first:

- widely available in QEMU and modern VMs
- designed to be simple for guest kernels
- much lower bring-up cost than raw AHCI or USB stacks

Recommended transport:

- `virtio-pci`

Recommended interrupt strategy:

- first working path can use simpler interrupt delivery
- MSI or MSI-X can come after basic queue setup works

### 7. Defer NVMe Until PCIe And MMIO Are Solid

NVMe is strategically important, but it is not the first storage driver to write.

Recommended order:

1. ACPI topology
2. PCIe enumeration
3. VirtIO block
4. initrd or simple filesystem using block I/O
5. NVMe

That ordering avoids mixing several new complexity layers at once.

### 8. Timer And Interrupt Notes

This milestone is primarily about platform discovery and device transport, but two related points matter:

- ACPI may provide `HPET`, which can later help timer calibration
- PCIe devices increasingly expect MSI/MSI-X rather than only legacy INTx interrupt delivery

For the first modern-platform cut:

- legacy PIC should remain disabled
- APIC routing should come from ACPI data
- MSI/MSI-X support can be a follow-on once the device manager is stable

## File And Module Plan

Likely new or updated files:

- `src/kernel/acpi/*` or equivalent ACPI parser files
- `src/kernel/pci/*`
- `src/kernel/device/*`
- updates to `src/kernel/cpu.cpp`
- updates to `src/kernel/ioapic.cpp`
- updates to `src/kernel/lapic.cpp`
- future `virtio` driver files

## Testing Strategy

Recommended QEMU environment:

- `q35` machine type
- `OVMF`
- PCIe-capable virtual machine layout
- `virtio-blk-pci`
- later `virtio-net-pci`

Suggested tests:

- ACPI checksum and table walk smoke test
- enumerate CPUs and IOAPICs from MADT
- enumerate PCIe devices from MCFG
- detect and probe a `virtio-blk` device
- read blocks from the device successfully

## Risks And Mitigations

| Risk | Mitigation |
| --- | --- |
| ACPI parser bugs cause silent platform misdetection | Validate checksums and table lengths aggressively |
| PCIe enumeration grows into a huge subsystem too early | Keep the first device model small and read-only at first |
| Too many driver targets dilute progress | Limit the initial target set to VirtIO block, then VirtIO net |
| NVMe complexity derails storage bring-up | Defer NVMe until the PCIe layer is already proven |

## Acceptance Criteria

This milestone is complete when:

1. The kernel discovers CPU and interrupt topology from ACPI `MADT`.
2. The kernel discovers PCIe configuration space from `MCFG`.
3. The kernel enumerates PCIe devices and records BAR resources.
4. The kernel probes and uses at least one `virtio-blk` device.
5. Legacy MP-table discovery is no longer the primary path on modern boots.

## Non-Goals

- USB/xHCI
- GPU acceleration
- advanced power management
- NUMA policy
- full ACPI AML interpreter support
