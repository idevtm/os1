#pragma once
#include "handoff/boot_info.hpp"
#include "hardware/tree.hpp"
#include "platform/acpi_aml.hpp"

namespace hardware_facts
{
using os1::hardware::copy;
using os1::hardware::Record;
using Cpuid = void (*)(uint32_t leaf, uint32_t (&words)[4]);
inline Record cpu(Cpuid cpuid)
{
    Record r{};
    copy(r.name, sizeof(r.name), "Boot processor");
    copy(r.state, sizeof(r.state), cpuid ? "observed" : "unavailable");
    if(!cpuid)
        return r;
    uint32_t w[4]{};
    cpuid(0, w);
    unsigned maximum = w[0];
    char vendor[13]{};
    unsigned order[] = {1, 3, 2};
    for(unsigned i = 0; i < 3; ++i)
        for(unsigned j = 0; j < 4; ++j)
            vendor[i * 4 + j] = char(w[order[i]] >> (8 * j));
    copy(r.identifier, sizeof(r.identifier), vendor);
    if(maximum >= 1)
    {
        cpuid(1, w);
        unsigned family = (w[0] >> 8) & 15;
        r.values[0] = family + (family == 15 ? (w[0] >> 20) & 255 : 0);
        r.values[1] = ((w[0] >> 4) & 15) | ((family == 6 || family == 15) ? (w[0] >> 12) & 240 : 0);
        r.values[2] = w[0] & 15;
        r.known = 7;
    }
    cpuid(0x80000000, w);
    unsigned extended = w[0];
    if(extended >= 0x80000004)
    {
        char brand[49]{};
        for(unsigned i = 0; i < 3; ++i)
        {
            cpuid(0x80000002 + i, w);
            for(unsigned j = 0; j < 16; ++j)
                brand[i * 16 + j] = char(w[j / 4] >> (8 * (j % 4)));
        }
        const char* start = brand;
        while(*start == ' ')
            ++start;
        if(*start)
            copy(r.name, sizeof(r.name), start);
    }
    if(extended >= 0x80000008)
    {
        cpuid(0x80000008, w);
        for(unsigned i = 0; i < 2; ++i)
        {
            unsigned bits = (w[0] >> (8 * i)) & 255;
            if(bits && bits <= 64)
            {
                r.values[3 + i] = bits;
                r.known |= 1u << (3 + i);
            }
        }
    }
    return r;
}
inline Record memory(const BootInfo& boot)
{
    Record r{};
    copy(r.name, sizeof(r.name), "Boot memory map (not installed DIMM capacity)");
    copy(r.state, sizeof(r.state), "invalid_map");
    if(!boot.memory_map || !boot.memory_map_count ||
       boot.memory_map_count > kBootInfoMaxMemoryRegions)
        return r;
    for(unsigned i = 0; i < boot.memory_map_count; ++i)
    {
        const auto& a = boot.memory_map[i];
        if(a.length > UINT64_MAX - a.physical_start)
            return r;
        for(unsigned j = 0; j < i; ++j)
        {
            const auto& b = boot.memory_map[j];
            if(a.length && b.length && a.physical_start < b.physical_start + b.length &&
               b.physical_start < a.physical_start + a.length)
                return r;
        }
    }
    for(unsigned i = 0; i < boot.memory_map_count; ++i)
    {
        const auto& a = boot.memory_map[i];
        unsigned type = unsigned(a.type);
        unsigned index = type >= 1 && type <= 9 ? type - 1 : 9;
        r.values[index] += a.length;  // Disjoint ranges cannot overflow the address space.
    }
    r.values[10] = boot.memory_map_count;
    r.known = (1u << 11) - 1;
    copy(r.state, sizeof(r.state), "observed");
    return r;
}
// Match only an immediate ACPI device parent. A scope is not a new bus.
inline unsigned parent(const AcpiDeviceInfo* acpi, unsigned count, unsigned child)
{
    unsigned best = count, length = 0;
    for(unsigned i = 0; i < count; ++i)
    {
        if(i == child)
            continue;
        unsigned n = 0;
        while(acpi[i].path[n] && acpi[i].path[n] == acpi[child].path[n])
            ++n;
        if(n > length && !acpi[i].path[n] && acpi[child].path[n] == '.')
        {
            best = i;
            length = n;
        }
    }
    return best;
}
inline void associate(const AcpiDeviceInfo* acpi,
                      unsigned acpi_count,
                      const PciDevice* pci,
                      unsigned pci_count,
                      uint32_t* result)
{
    using namespace os1::hardware;
    if(pci_count > kPlatformMaxPciDevices)
        return;
    for(unsigned i = 0; i < pci_count; ++i)
        result[i] = 0;
    if(!acpi || acpi_count > source_limit)
        return;
    for(unsigned pass = 0; pass < 16; ++pass)
    {
        uint32_t next[kPlatformMaxPciDevices]{};
        for(unsigned a = 0; a < acpi_count; ++a)
        {
            const auto& device = acpi[a];
            if(!(device.flags & kAcpiDeviceHasAdr) || (device.flags & kAcpiDevicePciRoot) ||
               device.adr >> 32 || (device.adr >> 16) > 31 || (device.adr & 65535) > 7)
                continue;
            unsigned p = parent(acpi, acpi_count, a), segment = 0, bus = 0;
            if(p == acpi_count)
                continue;
            if(acpi[p].flags & kAcpiDevicePciRoot)
            {
                if(!(acpi[p].flags & kAcpiDeviceRootLocation))
                    continue;
                segment = acpi[p].pci_segment;
                bus = acpi[p].bus_number;
                unsigned roots = 0;
                for(unsigned i = 0; i < acpi_count; ++i)
                    if((acpi[i].flags & kAcpiDeviceRootLocation) &&
                       acpi[i].pci_segment == segment && acpi[i].bus_number == bus)
                        ++roots;
                if(roots != 1)
                    continue;
            }
            else
            {
                unsigned bridge = pci_count;
                for(unsigned i = 0; i < pci_count; ++i)
                    if(result[i] == object_id(Acpi, p))
                        bridge = i;
                if(bridge == pci_count || pci[bridge].class_code != 6 ||
                   pci[bridge].subclass != 4 || !pci[bridge].secondary_bus ||
                   pci[bridge].secondary_bus == pci[bridge].bus)
                    continue;
                segment = pci[bridge].segment_group;
                bus = pci[bridge].secondary_bus;
            }
            unsigned match = pci_count, matches = 0;
            for(unsigned i = 0; i < pci_count; ++i)
                if(pci[i].segment_group == segment && pci[i].bus == bus &&
                   pci[i].slot == device.adr >> 16 && pci[i].function == (device.adr & 65535))
                {
                    match = i;
                    ++matches;
                }
            if(matches == 1)
                next[match] = next[match] ? UINT32_MAX : object_id(Acpi, a);
        }
        bool changed = false;
        for(unsigned i = 0; i < pci_count; ++i)
        {
            unsigned id = next[i] == UINT32_MAX ? 0 : next[i];
            changed |= id != result[i];
            result[i] = id;
        }
        if(!changed)
            break;
    }
}
}  // namespace hardware_facts
