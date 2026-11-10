#pragma once
#include <os1/hardware.h>
#include <os1/vfs.h>
#include <stddef.h>

namespace os1::hardware
{
enum Source : unsigned
{
    Pci = 1,
    Usb,
    Acpi,
    Processor,
    UsbDevice,
    UsbInterface,
    Block,
    Partition,
    Filesystem,
    Mount,
    UsbPort,
    CpuIdentity,
    Memory,
    BootPlatform
};
constexpr unsigned record_base = 0x100000, source_limit = 1024;
constexpr unsigned object_id(unsigned source, unsigned index)
{
    return record_base + (source << 16) + (index << 6);
}
struct Record
{
    uint32_t parent = 0, category = 0;
    char name[96]{}, state[96]{}, driver[96]{}, identifier[96]{};
    uint64_t values[22]{};
    uint64_t known = 0;
    uint32_t backing = 0, association = 0;
};
struct SourceView
{
    unsigned (*count)(unsigned);
    bool (*read)(unsigned, unsigned, Record&);
    unsigned (*lost)() = nullptr;
    unsigned (*catalog_lost)() = nullptr;
};
inline void copy(char* out, unsigned capacity, const char* in)
{
    unsigned n = 0;
    while(in && *in && n + 1 < capacity)
    {
        unsigned char c = *in++;
        out[n++] = c >= 32 && c <= 126 ? c : '?';
    }
    out[n] = 0;
}
inline void indexed(char* out, unsigned capacity, const char* prefix, unsigned index)
{
    copy(out, capacity, prefix);
    unsigned n = 0;
    while(out[n])
        ++n;
    char digits[12]{};
    unsigned count = 0;
    do
    {
        digits[count++] = '0' + index % 10;
        index /= 10;
    } while(index);
    while(count && n + 1 < capacity)
        out[n++] = digits[--count];
    out[n] = 0;
}
inline unsigned group(unsigned source)
{
    const unsigned groups[] = {
        0, OS1_HARDWARE_PCI, OS1_HARDWARE_USB, OS1_HARDWARE_ACPI, OS1_HARDWARE_PROCESSORS};
    if(source <= Processor)
        return groups[source];
    switch(source)
    {
        case CpuIdentity:
        case Memory:
        case BootPlatform:
            return OS1_HARDWARE_PLATFORM;
        case UsbDevice:
        case UsbPort:
            return OS1_HARDWARE_USB;
        case UsbInterface:
            return OS1_HARDWARE_USB_INTERFACES;
        case Block:
            return OS1_HARDWARE_BLOCKS;
        case Partition:
            return OS1_HARDWARE_PARTITIONS;
        case Filesystem:
            return OS1_HARDWARE_FILESYSTEMS;
        case Mount:
            return OS1_HARDWARE_MOUNTS;
        default:
            return 0;
    }
}
inline const char* detail_name(unsigned source, unsigned index)
{
    static constexpr const char* pci[] = {"segment",    "bus",        "slot",
                                          "function",   "vendor_id",  "device_id",
                                          "class_code", "subclass",   "programming_interface",
                                          "revision",   "bar_0_base", "bar_0_size",
                                          "bar_1_base", "bar_1_size", "bar_2_base",
                                          "bar_2_size", "bar_3_base", "bar_3_size",
                                          "bar_4_base", "bar_4_size", "bar_5_base",
                                          "bar_5_size"};
    static constexpr const char* usb[] = {"record_kind",
                                          "pci_index",
                                          "root_port",
                                          "port",
                                          "address",
                                          "parent_address",
                                          "status",
                                          "detail",
                                          "sectors"};
    static constexpr const char* acpi[] = {
        "status", "address", "uid", "bus_number", "resource_count"};
    static constexpr const char* cpu[] = {"apic_id", "enabled", "is_bsp"};
    if(source == Pci && index < 22)
        return pci[index];
    if(source == Usb && index < 9)
        return usb[index];
    if(source == Acpi && index < 5)
        return acpi[index];
    if(source == Processor && index < 3)
        return cpu[index];
    static constexpr const char* device[] = {"vendor_id",
                                             "product_id",
                                             "usb_version",
                                             "device_class",
                                             "device_subclass",
                                             "device_protocol",
                                             "speed_mbps_x10",
                                             "address",
                                             "root_port",
                                             "port",
                                             "configuration"};
    static constexpr const char* interface[] = {"number",
                                                "alternate_setting",
                                                "class_code",
                                                "subclass",
                                                "protocol",
                                                "endpoint_count",
                                                "configuration"};
    static constexpr const char* block[] = {
        "sector_count", "sector_size", "size_bytes", "max_sectors_per_request", "queue_depth"};
    static constexpr const char* partition[] = {
        "number", "type_code", "start_sector", "sector_count", "size_bytes"};
    static constexpr const char* filesystem[] = {"block_size", "block_count", "size_bytes"};
    static constexpr const char* port[] = {"port", "connected"};
    if(source == UsbDevice && index < 11)
        return device[index];
    if(source == UsbInterface && index < 7)
        return interface[index];
    if(source == Block && index < 5)
        return block[index];
    if(source == Partition && index < 5)
        return partition[index];
    if(source == Filesystem && index < 3)
        return filesystem[index];
    if(source == UsbPort && index < 2)
        return port[index];
    static constexpr const char* identity[] = {
        "family", "model", "stepping", "physical_address_bits", "virtual_address_bits"};
    static constexpr const char* memory[] = {"usable_bytes",
                                             "reserved_bytes",
                                             "acpi_reclaimable_bytes",
                                             "acpi_nvs_bytes",
                                             "bad_bytes",
                                             "mmio_bytes",
                                             "framebuffer_bytes",
                                             "kernel_image_bytes",
                                             "bootloader_reclaimable_bytes",
                                             "unknown_type_bytes",
                                             "region_count"};
    static constexpr const char* boot[] = {"acpi_available",
                                           "smbios_available",
                                           "framebuffer_width",
                                           "framebuffer_height",
                                           "framebuffer_bits_per_pixel"};
    if(source == CpuIdentity && index < 5)
        return identity[index];
    if(source == Memory && index < 11)
        return memory[index];
    if(source == BootPlatform && index < 5)
        return boot[index];
    return nullptr;
}
inline bool describe(const SourceView& source,
                     unsigned id,
                     Os1HardwareNode& node,
                     bool normalized = true)
{
    node = {};
    node.id = id;
    if(normalized && id >= OS1_HARDWARE_STORAGE_ROOT && id <= OS1_HARDWARE_PLATFORM)
    {
        const char* names[] = {"storage",
                               "devices",
                               "partitions",
                               "filesystems",
                               "mounts",
                               "catalog_records_dropped",
                               "usb_interfaces",
                               "platform"};
        copy(node.name, sizeof(node.name), names[id - OS1_HARDWARE_STORAGE_ROOT]);
        copy(node.text, sizeof(node.text), "HardwareCollection");
        if(id >= OS1_HARDWARE_BLOCKS && id <= OS1_HARDWARE_FILESYSTEMS)
            node.parent = OS1_HARDWARE_STORAGE_ROOT;
        if(id == OS1_HARDWARE_USB_INTERFACES)
            node.parent = OS1_HARDWARE_INVENTORY;
        if(id == OS1_HARDWARE_CATALOG_DROPPED)
        {
            node.parent = OS1_HARDWARE_DEVICES;
            node.type = OS1_HARDWARE_INTEGER;
            node.integer = source.catalog_lost ? source.catalog_lost() : 0;
        }
        return true;
    }
    if(id == OS1_HARDWARE_COVERAGE || id == OS1_HARDWARE_USB_DROPPED)
    {
        node.parent = OS1_HARDWARE_DEVICES;
        node.type = id == OS1_HARDWARE_COVERAGE ? OS1_HARDWARE_STRING : OS1_HARDWARE_INTEGER;
        copy(node.name,
             sizeof(node.name),
             id == OS1_HARDWARE_COVERAGE ? "coverage" : "usb_records_dropped");
        copy(node.text,
             sizeof(node.text),
             normalized ? "boot snapshot; normalized hardware and observed storage; no hotplug or "
                          "control interfaces"
                        : "boot snapshot; cached PCI/USB/ACPI/CPU records; no hotplug or control "
                          "interfaces");
        node.integer = source.lost ? source.lost() : 0;
        return true;
    }
    if(id >= OS1_HARDWARE_DEVICES && id <= OS1_HARDWARE_PROCESSORS)
    {
        static constexpr const char* names[] = {"devices",
                                                "inventory",
                                                "pci",
                                                "usb",
                                                "topology",
                                                "classes",
                                                "input",
                                                "storage",
                                                "network",
                                                "display",
                                                "controllers",
                                                "other",
                                                "drivers",
                                                "firmware",
                                                "acpi",
                                                "processors"};
        copy(node.name, sizeof(node.name), names[id - OS1_HARDWARE_DEVICES]);
        copy(node.text, sizeof(node.text), "HardwareCollection");
        if(id == OS1_HARDWARE_PCI || id == OS1_HARDWARE_USB)
            node.parent = OS1_HARDWARE_INVENTORY;
        else if(id >= OS1_HARDWARE_INPUT && id <= OS1_HARDWARE_OTHER)
            node.parent = OS1_HARDWARE_CLASSES;
        else if(id == OS1_HARDWARE_ACPI)
            node.parent = OS1_HARDWARE_FIRMWARE;
        else if(id != OS1_HARDWARE_DEVICES && id != OS1_HARDWARE_FIRMWARE &&
                id != OS1_HARDWARE_PROCESSORS)
            node.parent = OS1_HARDWARE_DEVICES;
        return true;
    }
    if(id < record_base || id >= object_id((normalized ? BootPlatform : Processor) + 1, 0))
        return false;
    unsigned kind = (id - record_base) >> 16, index = (id & 0xffff) >> 6, field = id & 63;
    if(!kind || index >= source.count(kind) || index >= source_limit)
        return false;
    Record record{};
    if(!source.read(kind, index, record))
        return false;
    unsigned object = object_id(kind, index);
    node.parent = field ? object : group(kind);
    if(!field)
    {
        const char* prefixes[] = {"",
                                  "pci_",
                                  "usb_",
                                  "acpi_",
                                  "cpu_",
                                  "usb_",
                                  "interface_",
                                  "block_",
                                  "partition_",
                                  "filesystem_",
                                  "mount_",
                                  "port_",
                                  "cpu",
                                  "memory",
                                  "boot"};
        if(kind >= CpuIdentity)
            copy(node.name, sizeof(node.name), prefixes[kind]);
        else
            indexed(node.name, sizeof(node.name), prefixes[kind], index);
        const char* types[] = {"",
                               "PciFunction",
                               "HardwareNode",
                               "AcpiDevice",
                               "Processor",
                               "UsbDevice",
                               "UsbInterface",
                               "BlockDeviceInfo",
                               "PartitionInfo",
                               "FilesystemInfo",
                               "MountInfo",
                               "UsbPort",
                               "CpuIdentity",
                               "BootMemory",
                               "BootPlatform"};
        copy(node.text, sizeof(node.text), normalized ? types[kind] : "HardwareNode");
        return true;
    }
    static constexpr const char* fields[] = {
        "", "id", "name", "state", "driver", "parent_id", "children", "category"};
    if(kind == Acpi && field == 31)
    {
        copy(node.name, sizeof(node.name), "hardware_id");
        node.type = OS1_HARDWARE_STRING;
        copy(node.text, sizeof(node.text), record.identifier);
        return true;
    }
    if(normalized && (kind == Pci || kind == Acpi) && (field == 38 || field == 39))
    {
        copy(node.name,
             sizeof(node.name),
             field == 38 ? (kind == Pci ? "firmware" : "devices")
                         : (kind == Pci ? "firmware_id" : "device_id"));
        node.type = field == 38 ? OS1_HARDWARE_OBJECT : OS1_HARDWARE_INTEGER;
        copy(node.text, sizeof(node.text), "HardwareCollection");
        node.integer = record.association;
        return true;
    }
    if(normalized && kind == CpuIdentity && field == 30)
    {
        copy(node.name, sizeof(node.name), "vendor");
        node.type = OS1_HARDWARE_STRING;
        copy(node.text, sizeof(node.text), record.identifier);
        return true;
    }
    if(normalized && field >= 32 && field <= 37)
    {
        const char* names[] = {
            "attachment", "backing", "consumers", "identifier", "observation", "backing_id"};
        copy(node.name, sizeof(node.name), names[field - 32]);
        if(field <= 34)
        {
            node.type = OS1_HARDWARE_OBJECT;
            copy(node.text, sizeof(node.text), "HardwareCollection");
        }
        else if(field == 37)
        {
            node.type = OS1_HARDWARE_INTEGER;
            node.integer = record.backing;
        }
        else
        {
            node.type = OS1_HARDWARE_STRING;
            copy(node.text, sizeof(node.text), field == 35 ? record.identifier : "boot_snapshot");
        }
        return true;
    }
    if(kind >= UsbDevice && field >= 8 && field < 30 &&
       !(record.known & (uint64_t(1) << (field - 8))))
        return false;
    const char* name = field < 8 ? fields[field] : detail_name(kind, field - 8);
    if(!name)
        return false;
    copy(node.name, sizeof(node.name), name);
    node.type = OS1_HARDWARE_INTEGER;
    if(field == 1)
        node.integer = object;
    else if(field >= 2 && field <= 4)
    {
        node.type = OS1_HARDWARE_STRING;
        copy(node.text,
             sizeof(node.text),
             field == 2   ? record.name
             : field == 3 ? record.state
                          : record.driver);
    }
    else if(field == 5)
        node.integer = record.parent;
    else if(field == 6)
    {
        node.type = OS1_HARDWARE_OBJECT;
        copy(node.text, sizeof(node.text), "HardwareCollection");
    }
    else if(field == 7)
        node.integer = record.category;
    else
        node.integer = record.values[field - 8];
    return true;
}
inline long query(const SourceView& source, Os1HardwareQuery& q)
{
    if((q.version != 1 && q.version != OS1_HARDWARE_VERSION) || q.reserved || q.next || q.count ||
       q.end || (q.operation != OS1_HARDWARE_DESCRIBE && q.operation != OS1_HARDWARE_CHILDREN))
        return OS1_ERR_INVALID;
    if(q.generation && q.generation != 1)
        return OS1_ERR_CANCELLED;
    for(const auto& node : q.nodes)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&node);
        for(unsigned i = 0; i < sizeof(node); ++i)
            if(bytes[i])
                return OS1_ERR_INVALID;
    }
    bool normalized = q.version == 2;
    Os1HardwareNode parent{};
    if(!describe(source, q.id, parent, normalized))
        return OS1_ERR_NOT_FOUND;
    q.generation = 1;
    if(q.operation == OS1_HARDWARE_DESCRIBE)
    {
        if(q.start)
            return OS1_ERR_INVALID;
        q.nodes[0] = parent;
        q.count = q.end = 1;
        return 0;
    }
    if(parent.type != OS1_HARDWARE_OBJECT)
        return OS1_ERR_INVALID;
    unsigned matched = 0;
    auto emit = [&](unsigned id) {
        Os1HardwareNode node{};
        if(!describe(source, id, node, normalized))
            return;
        if(matched++ >= q.start && q.count < OS1_HARDWARE_PAGE)
            q.nodes[q.count++] = node;
    };
    if(q.id >= record_base && (q.id & 63) == 0)
    {
        for(unsigned f = 1; f < (normalized ? 40u : 32u); ++f)
            emit(q.id + f);
    }
    else
    {
        for(unsigned g = OS1_HARDWARE_DEVICES;
            g <= (normalized ? OS1_HARDWARE_PLATFORM : OS1_HARDWARE_USB_DROPPED);
            ++g)
        {
            Os1HardwareNode node{};
            if(describe(source, g, node, normalized) && node.parent == q.id)
                emit(g);
        }
        for(unsigned s = Pci; s <= (normalized ? BootPlatform : Processor); ++s)
        {
            if(normalized && s == Usb)
                continue;
            unsigned count = source.count(s);
            if(count > source_limit)
                return OS1_ERR_LIMIT;
            for(unsigned i = 0; i < count; ++i)
            {
                unsigned id = object_id(s, i);
                bool include = q.id == group(s);
                if(q.id == OS1_HARDWARE_TOPOLOGY || q.id == OS1_HARDWARE_DRIVERS ||
                   (q.id >= OS1_HARDWARE_INPUT && q.id <= OS1_HARDWARE_OTHER) ||
                   q.id >= record_base)
                {
                    Record record{};
                    if(!source.read(s, i, record))
                        continue;
                    if(q.id == OS1_HARDWARE_TOPOLOGY)
                        include = (s == Pci || s == Usb || s == UsbDevice || s == UsbPort) &&
                                  !record.parent;
                    else if(q.id == OS1_HARDWARE_DRIVERS)
                        include = record.driver[0];
                    else if(q.id >= record_base)
                    {
                        const unsigned owner = q.id & ~63u, field = q.id & 63;
                        Record selected{};
                        unsigned owner_kind = (owner - record_base) >> 16;
                        unsigned owner_index = (owner & 0xffff) >> 6;
                        if(!source.read(owner_kind, owner_index, selected))
                            return OS1_ERR_NOT_FOUND;
                        include = field == 6    ? record.parent == owner
                                  : field == 32 ? id == selected.parent
                                  : field == 33 ? id == selected.backing
                                  : field == 34 ? record.backing == owner
                                  : field == 38 ? id == selected.association
                                                : false;
                    }
                    else
                        include = record.category == q.id && s != Acpi && s != Processor &&
                                  s < CpuIdentity;
                }
                if(include)
                    emit(id);
            }
        }
    }
    if(q.start > matched)
        return OS1_ERR_INVALID;
    q.next = q.start + q.count;
    q.end = q.next == matched;
    return 0;
}
}  // namespace os1::hardware
