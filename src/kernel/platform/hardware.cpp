#include "platform/hardware.hpp"

#include "drivers/bus/device.hpp"
#include "drivers/bus/driver_registry.hpp"
#include "drivers/input/service.hpp"
#include "drivers/usb/diagnostics.hpp"
#include "freestanding/string.hpp"
#include "hardware/names.hpp"
#include "hardware/tree.hpp"
#include "mm/kmem.hpp"
#include "platform/hardware_catalog.hpp"
#include "platform/hardware_facts.hpp"
#include "platform/platform.hpp"
#include "platform/state.hpp"
#include "storage/block_device.hpp"

namespace
{
using namespace os1::hardware;
os1::hardware::Catalog* catalog{};
unsigned capture_lost{};
Spinlock catalog_lock{"hardware-catalog"};
uint64_t catalog_generation = 1;
bool catalog_frozen{};
Record platform_records[3]{};
bool platform_captured{};
uint32_t firmware_associations[kPlatformMaxPciDevices]{};
unsigned catalog_lost()
{
    return capture_lost + (catalog ? catalog->dropped : 0);
}
struct Binding
{
    uint8_t name = 0;
    uint8_t state = 0;
};
Binding bindings[kPlatformMaxPciDevices]{};
char driver_names[kMaxPciDrivers][32]{};
unsigned driver_name_count = 0;
bool driver_names_overflow = false;
static_assert(kMaxPciDrivers < 256);
static_assert(sizeof(bindings) + sizeof(driver_names) <= 1024);
uint8_t retain_driver_name(const char* name)
{
    char text[32]{};
    copy(text, sizeof(text), name);
    if(!text[0])
        return 0;
    for(unsigned i = 0; i < driver_name_count; ++i)
        if(freestanding::strings_equal(text, driver_names[i]))
            return static_cast<uint8_t>(i + 1);
    if(driver_name_count == kMaxPciDrivers)
    {
        driver_names_overflow = true;
        return 0;
    }
    copy(driver_names[driver_name_count], sizeof(driver_names[0]), text);
    return static_cast<uint8_t>(++driver_name_count);
}
unsigned count(unsigned source)
{
    if(source >= UsbDevice && source <= UsbPort)
        return catalog ? catalog->counts[source] : 0;
    if(source >= CpuIdentity && source <= BootPlatform)
        return platform_captured ? 1 : 0;
    switch(source)
    {
        case Pci:
            return g_platform.device_count;
        case Usb:
            return usb::diagnostics::count();
        case Acpi:
            return platform_acpi_device_count();
        case Processor:
            return g_platform.cpu_count;
        default:
            return 0;
    }
}
bool read(unsigned source, unsigned index, Record& out)
{
    out = {};
    out.category = OS1_HARDWARE_OTHER;
    if(index >= count(source))
        return false;
    if(source >= CpuIdentity && source <= BootPlatform)
    {
        out = platform_records[source - CpuIdentity];
        return true;
    }
    if(source >= UsbDevice && source <= UsbPort)
        return catalog && catalog->read(source, index, out);
    if(source == Pci)
    {
        const auto& p = g_platform.devices[index];
        names::describe(out.name, p.vendor_id, p.device_id, p.class_code, p.subclass, false);
        out.association = firmware_associations[index];
        copy(out.driver,
             sizeof(out.driver),
             bindings[index].name ? driver_names[bindings[index].name - 1] : "");
        static constexpr const char* states[] = {
            "discovered", "probing", "bound", "started", "stopping", "removed", "failed"};
        copy(out.state,
             sizeof(out.state),
             bindings[index].state < 7 ? states[bindings[index].state] : "unknown");
        out.category = p.class_code == 1   ? OS1_HARDWARE_STORAGE
                       : p.class_code == 2 ? OS1_HARDWARE_NETWORK
                       : p.class_code == 3 ? OS1_HARDWARE_DISPLAY
                       : p.class_code == 9 ? OS1_HARDWARE_INPUT
                       : (p.class_code == 6 || (p.class_code == 12 && p.subclass == 3))
                           ? OS1_HARDWARE_CONTROLLERS
                           : OS1_HARDWARE_OTHER;
        for(unsigned i = 0; i < g_platform.device_count; ++i)
        {
            const auto& bridge = g_platform.devices[i];
            if(i != index && bridge.segment_group == p.segment_group && bridge.class_code == 6 &&
               bridge.subclass == 4 && bridge.secondary_bus && bridge.secondary_bus == p.bus &&
               bridge.bus != p.bus)
            {
                out.parent = object_id(Pci, i);
                break;
            }
        }
        const uint64_t values[] = {p.segment_group,
                                   p.bus,
                                   p.slot,
                                   p.function,
                                   p.vendor_id,
                                   p.device_id,
                                   p.class_code,
                                   p.subclass,
                                   p.prog_if,
                                   p.revision};
        for(unsigned i = 0; i < 10; ++i)
            out.values[i] = values[i];
        for(unsigned i = 0; i < 6; ++i)
        {
            out.values[10 + i * 2] = p.bars[i].base;
            out.values[11 + i * 2] = p.bars[i].size;
        }
    }
    else if(source == Usb)
    {
        const auto& u = *usb::diagnostics::at(index);
        if(u.kind != OS1_OBSERVE_USB_ROOT_PORT && u.kind != OS1_OBSERVE_USB_DEVICE)
            return false;  // Controller and mount observations are not additional physical devices.
        copy(out.name,
             sizeof(out.name),
             u.kind == OS1_OBSERVE_USB_ROOT_PORT    ? "USB port"
             : u.kind == OS1_OBSERVE_USB_DEVICE     ? "USB device"
             : u.kind == OS1_OBSERVE_USB_CONTROLLER ? "USB controller observation"
                                                    : "USB mount observation");
        copy(out.state, sizeof(out.state), u.stage);
        if(u.pci_index < g_platform.device_count)
            out.parent = object_id(Pci, u.pci_index);
        if(u.kind == OS1_OBSERVE_USB_DEVICE)
            for(unsigned i = 0; i < count(Usb); ++i)
            {
                const auto& p = *usb::diagnostics::at(i);
                if(i == index || p.pci_index != u.pci_index || p.root_port != u.root_port)
                    continue;
                if((u.parent_address && p.kind == OS1_OBSERVE_USB_DEVICE &&
                    p.address == u.parent_address) ||
                   (!u.parent_address && p.kind == OS1_OBSERVE_USB_ROOT_PORT))
                {
                    out.parent = object_id(Usb, i);
                    break;
                }
            }
        out.category = u.sectors ? OS1_HARDWARE_STORAGE
                       : (u.stage[0] == 'h' && u.stage[1] == 'i' && u.stage[2] == 'd')
                           ? OS1_HARDWARE_INPUT
                           : OS1_HARDWARE_OTHER;
        const uint64_t values[] = {u.kind,
                                   u.pci_index,
                                   u.root_port,
                                   u.port,
                                   u.address,
                                   u.parent_address,
                                   u.status,
                                   u.detail,
                                   u.sectors};
        for(unsigned i = 0; i < 9; ++i)
            out.values[i] = values[i];
    }
    else if(source == Acpi)
    {
        const auto& a = platform_acpi_devices()[index];
        for(unsigned i = 0; i < g_platform.device_count; ++i)
            if(firmware_associations[i] == object_id(Acpi, index))
                out.association = object_id(Pci, i);
        copy(out.name, sizeof(out.name), a.path);
        copy(out.identifier, sizeof(out.identifier), a.hardware_id);
        copy(out.state,
             sizeof(out.state),
             !a.active        ? "inactive"
             : (a.status & 1) ? "present"
                              : "not_present");
        out.values[0] = a.status;
        out.values[1] = a.adr;
        out.values[2] = a.uid;
        out.values[3] = a.bus_number;
        out.values[4] = a.resource_count;
    }
    else if(source == Processor)
    {
        const auto& c = g_platform.cpus[index];
        copy(out.name, sizeof(out.name), "Processor");
        copy(out.state, sizeof(out.state), c.enabled ? "enabled" : "disabled");
        out.values[0] = c.apic_id;
        out.values[1] = c.enabled;
        out.values[2] = c.is_bsp;
    }
    else
        return false;
    return true;
}
}  // namespace
void hardware_reset_bindings()
{
    kfree(catalog);
    catalog = nullptr;
    capture_lost = 0;
    catalog_generation = 1;
    catalog_frozen = false;
    platform_captured = false;
    for(auto& id : firmware_associations)
        id = 0;
    driver_name_count = 0;
    driver_names_overflow = false;
    for(auto& binding : bindings)
        binding = {};
}
void hardware_record_probe_failure(unsigned pci_index, const char* driver)
{
    if(pci_index >= kPlatformMaxPciDevices)
        return;
    bindings[pci_index].name = retain_driver_name(driver);
    bindings[pci_index].state = static_cast<uint8_t>(DeviceState::Failed);
}
void hardware_capture_platform(const BootInfo& boot)
{
    if(catalog_frozen)
        return;
    hardware_facts::Cpuid reader = nullptr;
#if !defined(OS1_HOST_TEST)
    reader = [](uint32_t leaf, uint32_t (&w)[4]) {
        asm volatile("cpuid" : "=a"(w[0]), "=b"(w[1]), "=c"(w[2]), "=d"(w[3]) : "a"(leaf), "c"(0));
    };
#endif
    platform_records[0] = hardware_facts::cpu(reader);
    platform_records[1] = hardware_facts::memory(boot);
    auto& r = platform_records[2];
    r = {};
    copy(r.name, sizeof(r.name), boot_source_name(boot.source));
    copy(r.identifier, sizeof(r.identifier), boot.bootloader_name);
    copy(r.state, sizeof(r.state), "observed");
    r.known = 3;
    r.values[0] = boot.rsdp_physical != 0;
    r.values[1] = boot.smbios_physical != 0;
    if(boot.framebuffer.physical_address)
    {
        r.known |= 28;
        r.values[2] = boot.framebuffer.width;
        r.values[3] = boot.framebuffer.height;
        r.values[4] = boot.framebuffer.bits_per_pixel;
    }
    platform_captured = true;
}
void hardware_freeze_bindings()
{
    catalog_frozen = true;
    hardware_facts::associate(platform_acpi_devices(),
                              platform_acpi_device_count(),
                              g_platform.devices,
                              g_platform.device_count,
                              firmware_associations);

    for(unsigned i = 0; i < device_binding_count(); ++i)
    {
        const auto& b = device_bindings()[i];
        if(b.active && b.id.bus == DeviceBus::Pci && b.pci_index < kPlatformMaxPciDevices)
        {
            bindings[b.pci_index].name = retain_driver_name(b.driver_name);
            bindings[b.pci_index].state = static_cast<uint8_t>(b.state);
        }
    }
}
long hardware_query(Os1HardwareQuery& request)
{
    if(request.id >= OS1_HARDWARE_INPUT_ROOT &&
       request.id <
           OS1_HARDWARE_INPUT_ROOT + (OS1_INPUT_DEVICE_LIMIT + 1) * OS1_HARDWARE_INPUT_STRIDE)
        return native_input::query(request);
    IrqSpinGuard guard(catalog_lock);
    if(driver_names_overflow)
        return OS1_ERR_LIMIT;
    return os1::hardware::query(
        {count, read, usb::diagnostics::dropped, catalog_lost, catalog_generation}, request);
}

namespace hardware_catalog
{
namespace
{
unsigned add_locked(unsigned kind, const os1::hardware::Record& r)
{
    if(!catalog)
        catalog = static_cast<os1::hardware::Catalog*>(
            kmalloc(sizeof(os1::hardware::Catalog), KmallocFlags::Zero));
    if(!catalog)
    {
        ++capture_lost;
        return 0;
    }
    return catalog->add(kind, r);
}
os1::hardware::Record* edit_locked(unsigned id)
{
    return catalog ? catalog->edit(id) : nullptr;
}
unsigned usb_port_locked(unsigned pci, unsigned port, bool connected)
{
    Record r{};
    r.parent = object_id(Pci, pci);
    r.known = 3;
    r.category = OS1_HARDWARE_CONTROLLERS;
    copy(r.name, sizeof(r.name), "USB port");
    copy(r.state, sizeof(r.state), connected ? "connected" : "disconnected");
    r.values[0] = port;
    r.values[1] = connected;
    return add_locked(UsbPort, r);
}
unsigned usb_device_locked(unsigned parent, unsigned root, unsigned port, unsigned speed)
{
    Record r{};
    r.category = OS1_HARDWARE_OTHER;
    r.parent = parent;
    r.known = (1u << 8) | (1u << 9);
    r.values[8] = root;
    r.values[9] = port;
    if(speed)
    {
        r.values[6] = speed;
        r.known |= 1u << 6;
    }
    copy(r.name, sizeof(r.name), "USB device");
    copy(r.state, sizeof(r.state), "enumeration_failed");
    return add_locked(UsbDevice, r);
}
unsigned usb_unenumerated_locked(unsigned parent, unsigned root, unsigned port, unsigned speed)
{
    unsigned id = usb_device_locked(parent, root, port, speed);
    if(auto* record = edit_locked(id))
        copy(record->state, sizeof(record->state), speed ? "unsupported_speed" : "not_enumerated");
    return id;
}
void usb_descriptor_locked(unsigned id, const void* bytes, unsigned length)
{
    auto* r = edit_locked(id);
    auto* b = static_cast<const uint8_t*>(bytes);
    if(!r || !b || length < 18 || b[0] < 18 || b[1] != 1)
        return;
    r->values[0] = b[8] | unsigned(b[9]) << 8;
    r->values[1] = b[10] | unsigned(b[11]) << 8;
    r->values[2] = b[2] | unsigned(b[3]) << 8;
    r->values[3] = b[4];
    r->values[4] = b[5];
    r->values[5] = b[6];
    r->known |= 63;
    names::describe(r->name, r->values[0], r->values[1], b[4], b[5], true);
    if(b[4] == 9)
        r->category = OS1_HARDWARE_CONTROLLERS;
    copy(r->state, sizeof(r->state), "described");
}
void usb_configuration_locked(unsigned id,
                              const void* bytes,
                              unsigned length,
                              bool descriptor_valid)
{
    auto* record = edit_locked(id);
    if(record && freestanding::strings_equal(record->state, "unavailable"))
        return;
    if(catalog && catalog->interfaces(id, descriptor_valid ? bytes : nullptr, length) < 0)
        if(auto* r = edit_locked(id))
            copy(r->state, sizeof(r->state), "descriptor_invalid");
}
unsigned usb_bind_locked(
    unsigned id, unsigned config, unsigned number, const char* driver, bool ready)
{
    auto* existing = edit_locked(id);
    if(existing && freestanding::strings_equal(existing->state, "unavailable"))
        return 0;
    if(auto* r = edit_locked(id))
    {
        if(ready)
        {
            r->values[10] = config;
            r->known |= 1u << 10;
        }
        copy(r->state, sizeof(r->state), ready ? "configured" : "configuration_failed");
    }
    return catalog ? catalog->bind_interface(id, config, number, driver, ready) : 0;
}
void unavailable_locked(unsigned id)
{
    auto* record = edit_locked(id);
    if(!record)
        return;
    copy(record->state, sizeof(record->state), "unavailable");
    // Bounded propagation also handles an unexpected relationship cycle.
    for(unsigned pass = 0; pass < catalog->used; ++pass)
    {
        bool changed = false;
        for(unsigned i = 0; i < catalog->used; ++i)
        {
            auto& r = catalog->entries[i].record;
            auto* backing = edit_locked(r.backing);
            auto* parent = edit_locked(r.parent);
            if(((backing && freestanding::strings_equal(backing->state, "unavailable")) ||
                (parent && freestanding::strings_equal(parent->state, "unavailable"))) &&
               !freestanding::strings_equal(r.state, "unavailable"))
            {
                copy(r.state, sizeof(r.state), "unavailable");
                changed = true;
            }
        }
        if(!changed)
            break;
    }
}
void block_locked(BlockDevice& b, unsigned backing)
{
    if(b.catalog_id)
        return;
    Record r{};
    r.backing = backing;
    r.category = OS1_HARDWARE_STORAGE;
    r.known = 0x1b;
    copy(r.name, sizeof(r.name), b.name);
    copy(r.driver, sizeof(r.driver), b.name);
    copy(r.state, sizeof(r.state), "ready");
    r.values[0] = b.sector_count;
    r.values[1] = b.sector_size;
    if(b.sector_size && b.sector_count <= ~uint64_t(0) / b.sector_size)
    {
        r.values[2] = b.sector_count * b.sector_size;
        r.known |= 4;
    }
    r.values[3] = b.max_sectors_per_request;
    r.values[4] = b.queue_depth;
    b.catalog_id = add_locked(Block, r);
}
void partitions_locked(BlockDevice& b, const uint8_t* mbr)
{
    if(!b.catalog_id || mbr[510] != 0x55 || mbr[511] != 0xaa)
        return;
    for(unsigned i = 0; i < 4; ++i)
    {
        auto* p = mbr + 446 + 16 * i;
        if(!p[4])
            continue;
        Record r{};
        r.backing = b.catalog_id;
        r.category = OS1_HARDWARE_STORAGE;
        r.known = 31;
        auto u32 = [](const uint8_t* v) {
            return uint32_t(v[0]) | uint32_t(v[1]) << 8 | uint32_t(v[2]) << 16 |
                   uint32_t(v[3]) << 24;
        };
        r.values[0] = i + 1;
        r.values[1] = p[4];
        r.values[2] = u32(p + 8);
        r.values[3] = u32(p + 12);
        r.values[4] = r.values[3] * b.sector_size;
        bool valid = r.values[3] && r.values[2] < b.sector_count &&
                     r.values[3] <= b.sector_count - r.values[2];
        copy(r.name, sizeof(r.name), "MBR partition");
        copy(r.state, sizeof(r.state), valid ? "observed" : "invalid_bounds");
        add_locked(Partition, r);
    }
}
void mounted_locked(BlockDevice& b, unsigned partition, const uint8_t* super)
{
    if(!catalog || !b.catalog_id)
        return;
    unsigned backing = 0;
    for(unsigned i = 0; i < catalog->used; ++i)
    {
        auto& e = catalog->entries[i];
        if((e.id - record_base) >> 16 == Partition && e.record.backing == b.catalog_id &&
           e.record.values[0] == partition)
        {
            backing = e.id;
            break;
        }
    }
    if(!backing)
        return;
    Record r{};
    r.backing = backing;
    r.category = OS1_HARDWARE_STORAGE;
    r.known = 7;
    copy(r.name, sizeof(r.name), "ext2");
    copy(r.driver, sizeof(r.driver), "ext2");
    copy(r.state, sizeof(r.state), "mounted");
    r.values[0] = 4096;
    r.values[1] = uint32_t(super[4]) | uint32_t(super[5]) << 8 | uint32_t(super[6]) << 16 |
                  uint32_t(super[7]) << 24;
    r.values[2] = r.values[0] * r.values[1];
    const char hex[] = "0123456789abcdef";
    unsigned n = 0;
    for(unsigned i = 0; i < 16; ++i)
    {
        if(i == 4 || i == 6 || i == 8 || i == 10)
            r.identifier[n++] = '-';
        r.identifier[n++] = hex[super[104 + i] >> 4];
        r.identifier[n++] = hex[super[104 + i] & 15];
    }
    unsigned fs = add_locked(Filesystem, r);
    if(!fs)
        return;
    r = {};
    r.backing = fs;
    r.category = OS1_HARDWARE_STORAGE;
    r.known = 0;
    copy(r.name, sizeof(r.name), "/disk");
    copy(r.state, sizeof(r.state), "mounted");
    add_locked(Mount, r);
}
void usb_address_locked(unsigned id, unsigned address)
{
    if(auto* r = edit_locked(id))
    {
        r->values[7] = address;
        r->known |= 1u << 7;
    }
}
void port_state_locked(unsigned id, bool connected)
{
    if(auto* r = edit_locked(id))
    {
        r->values[1] = connected;
        copy(r->state, sizeof(r->state), connected ? "connected" : "disconnected");
    }
}
}  // namespace
unsigned usb_port(unsigned pci, unsigned port, bool connected)
{
    IrqSpinGuard guard(catalog_lock);
    ++catalog_generation;
    return usb_port_locked(pci, port, connected);
}
unsigned usb_device(unsigned parent, unsigned root, unsigned port, unsigned speed)
{
    IrqSpinGuard guard(catalog_lock);
    ++catalog_generation;
    return usb_device_locked(parent, root, port, speed);
}
unsigned usb_unenumerated(unsigned parent, unsigned root, unsigned port, unsigned speed)
{
    IrqSpinGuard guard(catalog_lock);
    ++catalog_generation;
    return usb_unenumerated_locked(parent, root, port, speed);
}
void usb_descriptor(unsigned id, const void* bytes, unsigned length)
{
    IrqSpinGuard guard(catalog_lock);
    ++catalog_generation;
    usb_descriptor_locked(id, bytes, length);
}
void usb_configuration(unsigned id, const void* bytes, unsigned length, bool valid)
{
    IrqSpinGuard guard(catalog_lock);
    ++catalog_generation;
    usb_configuration_locked(id, bytes, length, valid);
}
unsigned usb_bind(unsigned id, unsigned config, unsigned number, const char* driver, bool ready)
{
    IrqSpinGuard guard(catalog_lock);
    ++catalog_generation;
    return usb_bind_locked(id, config, number, driver, ready);
}
void unavailable(unsigned id)
{
    IrqSpinGuard guard(catalog_lock);
    ++catalog_generation;
    unavailable_locked(id);
}
void block(BlockDevice& b, unsigned backing)
{
    IrqSpinGuard guard(catalog_lock);
    ++catalog_generation;
    block_locked(b, backing);
}
void partitions(BlockDevice& b, const uint8_t* mbr)
{
    IrqSpinGuard guard(catalog_lock);
    ++catalog_generation;
    partitions_locked(b, mbr);
}
void mounted(BlockDevice& b, unsigned partition, const uint8_t* super)
{
    IrqSpinGuard guard(catalog_lock);
    ++catalog_generation;
    mounted_locked(b, partition, super);
}
void usb_address(unsigned id, unsigned address)
{
    IrqSpinGuard guard(catalog_lock);
    ++catalog_generation;
    usb_address_locked(id, address);
}
void port_state(unsigned id, bool connected)
{
    IrqSpinGuard guard(catalog_lock);
    ++catalog_generation;
    port_state_locked(id, connected);
}
void usb_input(unsigned id, uint64_t identity)
{
    if(!identity)
        return;
    IrqSpinGuard guard(catalog_lock);
    if(auto* r = edit_locked(id))
    {
        r->values[11] = identity;
        r->known |= 1u << 11;
        ++catalog_generation;
    }
}
}  // namespace hardware_catalog
