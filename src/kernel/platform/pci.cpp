// PCI ECAM enumerator. It walks buses/functions from ACPI MCFG windows, sizes
// BARs, maps MMIO ranges needed by drivers, and records normalized PciDevice
// entries for platform probing.
#include "platform/pci.hpp"

#include "debug/debug.hpp"
#include "handoff/memory_layout.h"
#include "mm/boot_mapping.hpp"
#include "mm/virtual_memory.hpp"
#include "platform/pci_config.hpp"
#include "util/string.h"

namespace
{
[[nodiscard]] inline uint64_t align_down(uint64_t value, uint64_t alignment)
{
    return value & ~(alignment - 1);
}

[[nodiscard]] inline uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

void size_pci_bars(PciDevice& device)
{
    const uint8_t header_type = pci_header_type_kind(device.header_type);
    const uint8_t bar_limit = (0x01u == header_type) ? 2u : ((0x00u == header_type) ? 6u : 0u);
    device.bar_count = bar_limit;
    if(0 == bar_limit)
    {
        return;
    }

    const uint16_t command = pci_config_read16(device, 0x04);
    pci_config_write16(device, 0x04, static_cast<uint16_t>(command & ~0x3u));

    for(uint8_t index = 0; index < bar_limit; ++index)
    {
        const uint16_t offset = static_cast<uint16_t>(0x10 + index * 4);
        const uint32_t original = pci_config_read32(device, offset);
        if(0 == original)
        {
            continue;
        }

        PciBarInfo& bar = device.bars[index];
        if(original & 0x1u)
        {
            pci_config_write32(device, offset, 0xFFFFFFFFu);
            const uint32_t sized = pci_config_read32(device, offset);
            pci_config_write32(device, offset, original);
            const uint64_t bar_size = pci_io_bar_size_from_sizing_mask(sized);
            if(0 == bar_size)
            {
                continue;
            }
            bar.base = original & ~0x3u;
            bar.size = bar_size;
            bar.type = PciBarType::Io;
            continue;
        }

        const bool is_64_bit = 0x2u == ((original >> 1) & 0x3u);
        const uint32_t original_high = is_64_bit ? pci_config_read32(device, offset + 4) : 0u;
        pci_config_write32(device, offset, 0xFFFFFFFFu);
        if(is_64_bit)
        {
            pci_config_write32(device, offset + 4, 0xFFFFFFFFu);
        }
        const uint32_t sized_low = pci_config_read32(device, offset);
        const uint32_t sized_high = is_64_bit ? pci_config_read32(device, offset + 4) : 0u;
        pci_config_write32(device, offset, original);
        if(is_64_bit)
        {
            pci_config_write32(device, offset + 4, original_high);
        }

        uint64_t base = original & ~0xFull;
        PciBarType type = PciBarType::Mmio32;
        if(is_64_bit)
        {
            base |= static_cast<uint64_t>(original_high) << 32;
            type = PciBarType::Mmio64;
        }
        const uint64_t bar_size = pci_bar_size_from_sizing_mask(sized_low, sized_high, is_64_bit);
        if(0 == bar_size)
        {
            if(is_64_bit)
            {
                ++index;
            }
            continue;
        }

        bar.base = base;
        bar.size = bar_size;
        bar.type = type;
        if(is_64_bit && (index < 5u))
        {
            device.bars[index + 1].type = PciBarType::Unused;
            ++index;
        }
    }

    pci_config_write16(device, 0x04, command);
}

[[nodiscard]] bool record_pci_device(const PciEcamRegion& region,
                                     PciDevice* devices,
                                     size_t& device_count,
                                     uint8_t bus,
                                     uint8_t slot,
                                     uint8_t function,
                                     uint64_t config_physical)
{
    if(device_count >= kPlatformMaxPciDevices)
    {
        debug("pci: device table full")();
        return false;
    }

    PciDevice& device = devices[device_count++];
    memset(&device, 0, sizeof(device));
    device.segment_group = region.segment_group;
    device.bus = bus;
    device.slot = slot;
    device.function = function;
    device.config_physical = config_physical;
    device.vendor_id = pci_config_read16(config_physical, 0x00);
    device.device_id = pci_config_read16(config_physical, 0x02);
    device.revision = pci_config_read8(config_physical, 0x08);
    device.prog_if = pci_config_read8(config_physical, 0x09);
    device.subclass = pci_config_read8(config_physical, 0x0A);
    device.class_code = pci_config_read8(config_physical, 0x0B);
    device.header_type = pci_config_read8(config_physical, 0x0E);
    if((device.header_type & 0x7f) == 1)
        device.secondary_bus = pci_config_read8(config_physical, 0x19);
    device.interrupt_line = pci_config_read8(config_physical, 0x3C);
    device.interrupt_pin = pci_config_read8(config_physical, 0x3D);

    const uint16_t status = pci_config_read16(config_physical, 0x06);
    if(status & (1u << 4))
    {
        device.capability_pointer = pci_config_read8(config_physical, 0x34);
    }

    size_pci_bars(device);
    return true;
}
}  // namespace

bool enumerate_pci(VirtualMemory& kernel_vm,
                   const PciEcamRegion* regions,
                   size_t region_count,
                   PciDevice* devices,
                   size_t& device_count)
{
    if((nullptr == regions) || (nullptr == devices))
    {
        return false;
    }

    device_count = 0;
    for(size_t region_index = 0; region_index < region_count; ++region_index)
    {
        const PciEcamRegion& region = regions[region_index];
        const uint64_t region_size = static_cast<uint64_t>(region.bus_end - region.bus_start + 1u)
                                     << 20;
        if(!map_mmio_range(kernel_vm, region.base_address, region_size))
        {
            return false;
        }

        for(uint16_t bus = region.bus_start; bus <= region.bus_end; ++bus)
        {
            for(uint8_t slot = 0; slot < 32; ++slot)
            {
                const uint64_t function0 = region.base_address +
                                           (static_cast<uint64_t>(bus - region.bus_start) << 20) +
                                           (static_cast<uint64_t>(slot) << 15);
                const uint16_t vendor0 = pci_config_read16(function0, 0x00);
                if(0xFFFFu == vendor0)
                {
                    continue;
                }

                const uint8_t header_type = pci_config_read8(function0, 0x0E);
                const uint8_t function_limit = (header_type & 0x80u) ? 8u : 1u;
                for(uint8_t function = 0; function < function_limit; ++function)
                {
                    const uint64_t config_physical =
                        function0 + (static_cast<uint64_t>(function) << 12);
                    if(0xFFFFu == pci_config_read16(config_physical, 0x00))
                    {
                        continue;
                    }
                    if(!record_pci_device(region,
                                          devices,
                                          device_count,
                                          static_cast<uint8_t>(bus),
                                          slot,
                                          function,
                                          config_physical))
                    {
                        return false;
                    }
                }
            }
        }
    }

    debug("pci: enumerated devices=")(device_count)();
    return true;
}
