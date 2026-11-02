// PCI ECAM enumeration. The platform layer provides ACPI MCFG windows; this
// module walks functions, sizes BARs, and returns normalized PciDevice records.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "platform/platform.hpp"

class VirtualMemory;

// Decode a memory BAR size from the values read back after writing all ones.
// A 64-bit BAR whose upper half reads back 0 hardwires its upper address bits
// (it can only live below 4 GiB); its size comes from the low half alone.
[[nodiscard]] inline uint64_t pci_bar_size_from_sizing_mask(uint32_t sized_low,
                                                            uint32_t sized_high,
                                                            bool is_64_bit)
{
    uint64_t size_mask = static_cast<uint64_t>(sized_low & ~0xFu);
    if(!is_64_bit)
    {
        return (0u == size_mask) ? 0u : ((~size_mask + 1u) & 0xFFFFFFFFull);
    }
    const uint64_t high_mask = ((0u == sized_high) && (0u != size_mask)) ? 0xFFFFFFFFull : sized_high;
    size_mask |= high_mask << 32;
    return (0u == size_mask) ? 0u : (~size_mask + 1u);
}

// Enumerate all PCI functions reachable through the supplied ECAM regions.
bool enumerate_pci(VirtualMemory& kernel_vm,
                   const PciEcamRegion* regions,
                   size_t region_count,
                   PciDevice* devices,
                   size_t& device_count);
