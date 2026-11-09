#include "platform/hardware.hpp"

#include "drivers/bus/device.hpp"
#include "drivers/usb/diagnostics.hpp"
#include "hardware/tree.hpp"
#include "platform/platform.hpp"
#include "platform/state.hpp"

namespace
{
using namespace os1::hardware;
struct Binding
{
    char name[32]{};
    unsigned state = 0;
};
Binding bindings[kPlatformMaxPciDevices]{};
unsigned count(unsigned source)
{
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
    if(index >= count(source))
        return false;
    if(source == Pci)
    {
        const auto& p = g_platform.devices[index];
        copy(out.name, sizeof(out.name), "PCI function");
        copy(out.driver, sizeof(out.driver), bindings[index].name);
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
    for(auto& binding : bindings)
        binding = {};
}
void hardware_record_probe_failure(unsigned pci_index, const char* driver)
{
    if(pci_index >= kPlatformMaxPciDevices)
        return;
    os1::hardware::copy(bindings[pci_index].name, sizeof(bindings[pci_index].name), driver);
    bindings[pci_index].state = static_cast<unsigned>(DeviceState::Failed);
}
void hardware_freeze_bindings()
{
    for(unsigned i = 0; i < device_binding_count(); ++i)
    {
        const auto& b = device_bindings()[i];
        if(b.active && b.id.bus == DeviceBus::Pci && b.pci_index < kPlatformMaxPciDevices)
        {
            os1::hardware::copy(
                bindings[b.pci_index].name, sizeof(bindings[b.pci_index].name), b.driver_name);
            bindings[b.pci_index].state = static_cast<unsigned>(b.state);
        }
    }
}
long hardware_query(Os1HardwareQuery& request)
{
    return os1::hardware::query({count, read, usb::diagnostics::dropped}, request);
}
