// Early xHCI PCI driver. This owns controller reset, MMIO resource claims,
// DMA ring allocation, interrupter 0 setup, and lifecycle teardown.
#include "drivers/usb/xhci.hpp"

#include "console/console_input.hpp"
#include "debug/debug.hpp"
#include "debug/event_ring.hpp"
#include "drivers/bus/device.hpp"
#include "drivers/bus/resource.hpp"
#include "drivers/usb/hid_keyboard.hpp"
#include "drivers/usb/xhci_controller.hpp"
#include "handoff/memory_layout.h"
#include "mm/boot_mapping.hpp"
#include "mm/dma.hpp"
#include "mm/page_frame.hpp"
#include "platform/irq_registry.hpp"
#include "platform/pci_config.hpp"
#include "platform/pci_msi.hpp"
#include "platform/state.hpp"
#include "util/memory.h"

extern PageFrameContainer page_frames;

namespace
{
constexpr uint8_t kPciClassSerialBus = 0x0Cu;
constexpr uint8_t kPciSubclassUsb = 0x03u;
constexpr uint8_t kPciProgIfXhci = 0x30u;
constexpr PciMatch kXhciMatches[]{
    {
        .class_code = kPciClassSerialBus,
        .subclass = kPciSubclassUsb,
        .prog_if = kPciProgIfXhci,
        .match_flags = static_cast<uint8_t>(kPciMatchClassCode | kPciMatchSubclass | kPciMatchProgIf),
    },
};
constexpr size_t kXhciMaxControllers = 4u;
constexpr size_t kXhciMaxHidDevices = 4u;
constexpr size_t kXhciCommandRingBytes = kPageSize;
constexpr size_t kXhciEventRingBytes = kPageSize;
constexpr size_t kXhciTransferRingBytes = kPageSize;
constexpr size_t kXhciControlBufferBytes = 512u;
constexpr size_t kXhciReportBufferBytes = 8u;
constexpr uint32_t kXhciInitTimeoutSpins = 1000000u;
constexpr uint64_t kXhciMaxBarBytes = 16ull * 1024 * 1024;
constexpr uint16_t kPciCommandMemorySpace = 0x0002u;
constexpr uint32_t kXhciPortTimeoutSpins = 1000000u;
constexpr uint32_t kXhciTransferTimeoutSpins = 1000000u;

constexpr uint8_t kUsbDescriptorDevice = 1u;
constexpr uint8_t kUsbDescriptorConfiguration = 2u;
constexpr uint8_t kUsbDescriptorInterface = 4u;
constexpr uint8_t kUsbDescriptorEndpoint = 5u;

constexpr uint8_t kUsbRequestGetDescriptor = 6u;
constexpr uint8_t kUsbRequestSetConfiguration = 9u;
constexpr uint8_t kUsbHidRequestSetProtocol = 11u;

constexpr uint8_t kUsbRequestTypeStandardInDevice = 0x80u;
constexpr uint8_t kUsbRequestTypeStandardOutDevice = 0x00u;
constexpr uint8_t kUsbRequestTypeClassOutInterface = 0x21u;

constexpr uint8_t kUsbClassHid = 3u;
constexpr uint8_t kUsbSubclassBoot = 1u;
constexpr uint8_t kUsbProtocolKeyboard = 1u;
constexpr uint8_t kUsbProtocolMouse = 2u;
constexpr uint8_t kUsbEndpointDirectionIn = 0x80u;
constexpr uint8_t kUsbEndpointTransferInterrupt = 3u;

constexpr uint32_t kTrbInterruptOnShortPacket = 1u << 2;
constexpr uint32_t kTrbChainBit = 1u << 4;
constexpr uint32_t kTrbInterruptOnCompletion = 1u << 5;
constexpr uint32_t kTrbImmediateData = 1u << 6;
constexpr uint32_t kSetupTransferTypeNoData = 0u << 16;
constexpr uint32_t kSetupTransferTypeOut = 2u << 16;
constexpr uint32_t kSetupTransferTypeIn = 3u << 16;
constexpr uint32_t kDataDirectionIn = 1u << 16;
constexpr uint32_t kStatusDirectionIn = 1u << 16;
constexpr uint8_t kXhciControlEndpointDci = 1u;
constexpr uint8_t kXhciCompletionSuccess = 1u;
constexpr uint8_t kXhciCompletionShortPacket = 13u;

constexpr uint32_t kInputContextAddSlot = 1u << 0;
constexpr uint32_t kInputContextAddEp0 = 1u << 1;

enum class HidBootProtocol : uint8_t
{
    None,
    Keyboard,
    Mouse,
};

struct [[gnu::packed]] UsbSetupPacket
{
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
};

struct [[gnu::packed]] UsbDeviceDescriptor
{
    uint8_t length;
    uint8_t descriptor_type;
    uint16_t usb_version;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t max_packet_size0;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_version;
    uint8_t manufacturer_index;
    uint8_t product_index;
    uint8_t serial_number_index;
    uint8_t configuration_count;
};

struct [[gnu::packed]] UsbConfigurationDescriptor
{
    uint8_t length;
    uint8_t descriptor_type;
    uint16_t total_length;
    uint8_t interface_count;
    uint8_t configuration_value;
    uint8_t configuration_index;
    uint8_t attributes;
    uint8_t max_power;
};

struct [[gnu::packed]] UsbInterfaceDescriptor
{
    uint8_t length;
    uint8_t descriptor_type;
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t endpoint_count;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t interface_index;
};

struct [[gnu::packed]] UsbEndpointDescriptor
{
    uint8_t length;
    uint8_t descriptor_type;
    uint8_t endpoint_address;
    uint8_t attributes;
    uint16_t max_packet_size;
    uint8_t interval;
};

struct HidInterfaceSelection
{
    bool valid = false;
    HidBootProtocol protocol = HidBootProtocol::None;
    uint8_t configuration_value = 0;
    uint8_t interface_number = 0;
    uint8_t endpoint_address = 0;
    uint8_t interval = 0;
    uint16_t max_packet_size = 0;
};

struct XhciHidDeviceState
{
    bool present = false;
    bool configured = false;
    uint8_t port_id = 0;
    uint8_t slot_id = 0;
    uint8_t speed = 0;
    HidBootProtocol protocol = HidBootProtocol::None;
    uint8_t configuration_value = 0;
    uint8_t interface_number = 0;
    uint8_t endpoint_address = 0;
    uint8_t endpoint_dci = 0;
    uint8_t endpoint_interval = 0;
    uint16_t control_max_packet_size = 8;
    uint16_t endpoint_max_packet_size = 8;
    DmaBuffer input_context_dma{};
    DmaBuffer device_context_dma{};
    DmaBuffer control_ring_dma{};
    xhci::CommandRingInfo control_ring{};
    DmaBuffer interrupt_ring_dma{};
    xhci::CommandRingInfo interrupt_ring{};
    DmaBuffer control_buffer_dma{};
    DmaBuffer report_buffer_dma{};
    usb::hid::BootKeyboardState keyboard_state{};
    bool synchronous_transfer_complete = false;
    uint8_t synchronous_transfer_completion_code = 0;
    uint32_t synchronous_transfer_residual = 0;
    uint64_t synchronous_transfer_trb = 0;
    bool interrupt_transfer_armed = false;
    uint64_t interrupt_transfer_trb = 0;
    uint32_t report_count = 0;
};

struct XhciControllerState
{
    bool present = false;
    DeviceId owner{DeviceBus::Platform, 0};
    uint16_t pci_index = 0;
    uint16_t configured_slots = 0;
    uint16_t scratchpad_count = 0;
    uint32_t interrupt_count = 0;
    PciBarResource controller_bar{};
    PciInterruptHandle interrupt{};
    xhci::ControllerLayout controller{};
    xhci::CommandRingInfo command_ring{};
    xhci::EventRingInfo event_ring{};
    uint64_t pending_command_trb = 0;
    bool pending_command_complete = false;
    uint8_t pending_command_completion_code = 0;
    uint8_t pending_command_slot_id = 0;
    DmaBuffer dcbaa_dma{};
    DmaBuffer scratchpad_array_dma{};
    DmaBuffer scratchpad_buffers_dma{};
    DmaBuffer command_ring_dma{};
    DmaBuffer event_ring_dma{};
    DmaBuffer erst_dma{};
    XhciHidDeviceState hid_devices[kXhciMaxHidDevices]{};
};

constinit XhciControllerState g_xhci_controllers[kXhciMaxControllers]{};

[[nodiscard]] bool device_id_equal(DeviceId left, DeviceId right)
{
    return (left.bus == right.bus) && (left.index == right.index);
}

XhciControllerState* find_controller(DeviceId id)
{
    for(auto& controller : g_xhci_controllers)
    {
        if(controller.present && device_id_equal(controller.owner, id))
        {
            return &controller;
        }
    }
    return nullptr;
}

XhciControllerState* allocate_controller()
{
    for(auto& controller : g_xhci_controllers)
    {
        if(!controller.present)
        {
            return &controller;
        }
    }
    return nullptr;
}

void release_dma(PageFrameContainer& frames, DmaBuffer& buffer)
{
    if(buffer.active)
    {
        dma_release_buffer(frames, buffer);
    }
}

[[nodiscard]] bool hid_device_has_resources(const XhciHidDeviceState& device)
{
    return device.input_context_dma.active || device.device_context_dma.active ||
           device.control_ring_dma.active || device.interrupt_ring_dma.active ||
           device.control_buffer_dma.active || device.report_buffer_dma.active;
}

void cpu_pause()
{
#if defined(OS1_HOST_TEST)
    asm volatile("" : : : "memory");
#else
    asm volatile("pause" : : : "memory");
#endif
}

[[nodiscard]] bool transfer_completion_ok(uint8_t completion_code)
{
    return (kXhciCompletionSuccess == completion_code) ||
           (kXhciCompletionShortPacket == completion_code);
}

[[nodiscard]] uint16_t control_max_packet_from_speed(uint8_t speed)
{
    if(speed >= 4u)
    {
        return 512u;
    }
    if(3u == speed)
    {
        return 64u;
    }
    return 8u;
}

[[nodiscard]] uint8_t endpoint_dci_from_address(uint8_t endpoint_address)
{
    const uint8_t endpoint_number = endpoint_address & 0x0Fu;
    const uint8_t direction_in = (endpoint_address & kUsbEndpointDirectionIn) ? 1u : 0u;
    return static_cast<uint8_t>(endpoint_number * 2u + direction_in);
}

[[nodiscard]] size_t input_context_bytes(const XhciControllerState& controller)
{
    return static_cast<size_t>(controller.controller.capability.context_size_bytes) * 33u;
}

[[nodiscard]] size_t device_context_bytes(const XhciControllerState& controller)
{
    return static_cast<size_t>(controller.controller.capability.context_size_bytes) * 32u;
}

[[nodiscard]] uint32_t* input_control_context(XhciHidDeviceState& device)
{
    return static_cast<uint32_t*>(device.input_context_dma.virtual_address);
}

[[nodiscard]] uint32_t* input_slot_context(XhciControllerState& controller, XhciHidDeviceState& device)
{
    auto* base = static_cast<uint8_t*>(device.input_context_dma.virtual_address);
    return reinterpret_cast<uint32_t*>(base + controller.controller.capability.context_size_bytes);
}

[[nodiscard]] uint32_t* input_endpoint_context(XhciControllerState& controller,
                                               XhciHidDeviceState& device,
                                               uint8_t dci)
{
    auto* base = static_cast<uint8_t*>(device.input_context_dma.virtual_address);
    const size_t offset = static_cast<size_t>(controller.controller.capability.context_size_bytes) *
                          static_cast<size_t>(1u + dci);
    return reinterpret_cast<uint32_t*>(base + offset);
}

void set_dcbaa_entry(XhciControllerState& controller, uint8_t slot_id, uint64_t value)
{
    if((0u == slot_id) || (!controller.dcbaa_dma.active) ||
       (static_cast<size_t>(slot_id) >=
        (controller.dcbaa_dma.size_bytes / sizeof(uint64_t))))
    {
        return;
    }

    auto* dcbaa = static_cast<uint64_t*>(controller.dcbaa_dma.virtual_address);
    dcbaa[slot_id] = value;
    dma_sync_for_device(controller.dcbaa_dma);
}

XhciHidDeviceState* allocate_hid_device(XhciControllerState& controller)
{
    for(auto& device : controller.hid_devices)
    {
        if(!device.present && !hid_device_has_resources(device))
        {
            return &device;
        }
    }
    return nullptr;
}

void release_hid_device(PageFrameContainer& frames,
                        XhciControllerState& controller,
                        XhciHidDeviceState& device)
{
    if(device.slot_id != 0u)
    {
        set_dcbaa_entry(controller, device.slot_id, 0u);
    }
    release_dma(frames, device.report_buffer_dma);
    release_dma(frames, device.control_buffer_dma);
    release_dma(frames, device.interrupt_ring_dma);
    release_dma(frames, device.control_ring_dma);
    release_dma(frames, device.device_context_dma);
    release_dma(frames, device.input_context_dma);
    device = {};
}

void release_controller(PageFrameContainer& frames, XhciControllerState& controller)
{
    if(nullptr != controller.controller.operational)
    {
        (void)xhci::controller_stop(controller.controller, kXhciInitTimeoutSpins);
    }
    if((PciInterruptMode::None != controller.interrupt.mode) &&
       (controller.pci_index < kPlatformMaxPciDevices))
    {
        pci_release_interrupt(g_platform.devices[controller.pci_index], controller.interrupt);
    }

    for(auto& device : controller.hid_devices)
    {
        if(device.present || hid_device_has_resources(device))
        {
            release_hid_device(frames, controller, device);
        }
    }

    release_dma(frames, controller.erst_dma);
    release_dma(frames, controller.event_ring_dma);
    release_dma(frames, controller.command_ring_dma);
    release_dma(frames, controller.scratchpad_buffers_dma);
    release_dma(frames, controller.scratchpad_array_dma);
    release_dma(frames, controller.dcbaa_dma);
    release_pci_bars_for_owner(controller.owner);
    platform_release_irq_routes_for_owner(controller.owner);
    controller = {};
}

bool prepare_dcbaa(XhciControllerState& controller)
{
    auto* dcbaa = static_cast<uint64_t*>(controller.dcbaa_dma.virtual_address);
    memset(dcbaa, 0, controller.dcbaa_dma.size_bytes);
    if(0 == controller.scratchpad_count)
    {
        return true;
    }
    if((!controller.scratchpad_array_dma.active) || (!controller.scratchpad_buffers_dma.active))
    {
        return false;
    }

    auto* scratchpad_array = static_cast<uint64_t*>(controller.scratchpad_array_dma.virtual_address);
    memset(scratchpad_array, 0, controller.scratchpad_array_dma.size_bytes);
    for(uint16_t index = 0; index < controller.scratchpad_count; ++index)
    {
        scratchpad_array[index] = controller.scratchpad_buffers_dma.physical_address +
                                  static_cast<uint64_t>(index) * kPageSize;
    }
    dcbaa[0] = controller.scratchpad_array_dma.physical_address;
    dma_sync_for_device(controller.scratchpad_array_dma);
    dma_sync_for_device(controller.scratchpad_buffers_dma);
    dma_sync_for_device(controller.dcbaa_dma);
    return true;
}

bool allocate_controller_buffers(PageFrameContainer& frames, XhciControllerState& controller)
{
    const size_t dcbaa_bytes = static_cast<size_t>(controller.configured_slots + 1u) * sizeof(uint64_t);
    if(!dma_allocate_buffer(frames,
                            controller.owner,
                            dcbaa_bytes,
                            DmaDirection::Bidirectional,
                            controller.dcbaa_dma) ||
       !dma_allocate_buffer(frames,
                            controller.owner,
                            kXhciCommandRingBytes,
                            DmaDirection::Bidirectional,
                            controller.command_ring_dma) ||
       !dma_allocate_buffer(frames,
                            controller.owner,
                            kXhciEventRingBytes,
                            DmaDirection::Bidirectional,
                            controller.event_ring_dma) ||
       !dma_allocate_buffer(frames,
                            controller.owner,
                            sizeof(xhci::EventRingSegmentTableEntry),
                            DmaDirection::Bidirectional,
                            controller.erst_dma))
    {
        return false;
    }

    if(!xhci::initialize_command_ring(controller.command_ring_dma.virtual_address,
                                      controller.command_ring_dma.size_bytes,
                                      controller.command_ring_dma.physical_address,
                                      controller.command_ring))
    {
        return false;
    }

    const uint32_t event_ring_trbs =
        static_cast<uint32_t>(controller.event_ring_dma.size_bytes / xhci::kTrbBytes);
    if(!xhci::initialize_event_ring_segment_table(controller.erst_dma.virtual_address,
                                                  controller.erst_dma.size_bytes,
                                                  controller.event_ring_dma.physical_address,
                                                  event_ring_trbs))
    {
        return false;
    }
    if(!xhci::initialize_event_ring_state(controller.event_ring_dma.physical_address,
                                          event_ring_trbs,
                                          controller.event_ring))
    {
        return false;
    }

    if(0 != controller.scratchpad_count)
    {
        if(!dma_allocate_buffer(frames,
                                controller.owner,
                                static_cast<size_t>(controller.scratchpad_count) * sizeof(uint64_t),
                                DmaDirection::Bidirectional,
                                controller.scratchpad_array_dma) ||
           !dma_allocate_buffer(frames,
                                controller.owner,
                                static_cast<size_t>(controller.scratchpad_count) * kPageSize,
                                DmaDirection::Bidirectional,
                                controller.scratchpad_buffers_dma))
        {
            return false;
        }
    }

    return prepare_dcbaa(controller);
}

void configure_slot_context(XhciControllerState& controller,
                            XhciHidDeviceState& device,
                            uint8_t context_entries)
{
    auto* slot = input_slot_context(controller, device);
    memset(slot, 0, controller.controller.capability.context_size_bytes);
    slot[0] = static_cast<uint32_t>(device.speed) << 20;
    slot[1] = (static_cast<uint32_t>(device.port_id) << 16) |
              (static_cast<uint32_t>(context_entries) << 27);
}

void configure_endpoint_context(XhciControllerState& controller,
                                XhciHidDeviceState& device,
                                uint8_t dci,
                                uint8_t endpoint_type,
                                uint16_t max_packet_size,
                                uint8_t interval,
                                const DmaBuffer& ring_dma,
                                uint16_t average_trb_length)
{
    auto* endpoint = input_endpoint_context(controller, device, dci);
    memset(endpoint, 0, controller.controller.capability.context_size_bytes);
    endpoint[0] = static_cast<uint32_t>(interval) << 16;
    endpoint[1] = (3u << 1) | (static_cast<uint32_t>(endpoint_type) << 3) |
                  (static_cast<uint32_t>(max_packet_size) << 16);
    endpoint[2] = static_cast<uint32_t>((ring_dma.physical_address & ~0xFull) | 1ull);
    endpoint[3] = static_cast<uint32_t>(ring_dma.physical_address >> 32);
    endpoint[4] = average_trb_length;
}

bool allocate_hid_device_resources(PageFrameContainer& frames,
                                   XhciControllerState& controller,
                                   XhciHidDeviceState& device)
{
    if(!dma_allocate_buffer(frames,
                            controller.owner,
                            input_context_bytes(controller),
                            DmaDirection::Bidirectional,
                            device.input_context_dma) ||
       !dma_allocate_buffer(frames,
                            controller.owner,
                            device_context_bytes(controller),
                            DmaDirection::Bidirectional,
                            device.device_context_dma) ||
       !dma_allocate_buffer(frames,
                            controller.owner,
                            kXhciTransferRingBytes,
                            DmaDirection::Bidirectional,
                            device.control_ring_dma) ||
       !dma_allocate_buffer(frames,
                            controller.owner,
                            kXhciTransferRingBytes,
                            DmaDirection::Bidirectional,
                            device.interrupt_ring_dma) ||
       !dma_allocate_buffer(frames,
                            controller.owner,
                            kXhciControlBufferBytes,
                            DmaDirection::Bidirectional,
                            device.control_buffer_dma) ||
       !dma_allocate_buffer(frames,
                            controller.owner,
                            kXhciReportBufferBytes,
                            DmaDirection::Bidirectional,
                            device.report_buffer_dma))
    {
        return false;
    }

    if(!xhci::initialize_command_ring(device.control_ring_dma.virtual_address,
                                      device.control_ring_dma.size_bytes,
                                      device.control_ring_dma.physical_address,
                                      device.control_ring) ||
       !xhci::initialize_command_ring(device.interrupt_ring_dma.virtual_address,
                                      device.interrupt_ring_dma.size_bytes,
                                      device.interrupt_ring_dma.physical_address,
                                      device.interrupt_ring))
    {
        return false;
    }

    memset(device.input_context_dma.virtual_address, 0, device.input_context_dma.size_bytes);
    memset(device.device_context_dma.virtual_address, 0, device.device_context_dma.size_bytes);
    dma_sync_for_device(device.input_context_dma);
    dma_sync_for_device(device.device_context_dma);
    return true;
}

void handle_keyboard_report(XhciHidDeviceState& device)
{
    char output[6]{};
    size_t output_count = 0;
    if(!usb::hid::boot_keyboard_apply_report(
           device.keyboard_state,
           static_cast<const uint8_t*>(device.report_buffer_dma.virtual_address),
           usb::hid::kBootKeyboardReportBytes,
           output,
           sizeof(output),
           output_count))
    {
        return;
    }

    for(size_t index = 0; index < output_count; ++index)
    {
        console_input_on_keyboard_char(output[index]);
    }
}

void handle_mouse_report(XhciHidDeviceState& device)
{
    ++device.report_count;
}

bool arm_interrupt_transfer(XhciControllerState& controller, XhciHidDeviceState& device)
{
    if(!device.present || !device.configured)
    {
        return false;
    }

    memset(device.report_buffer_dma.virtual_address, 0, device.report_buffer_dma.size_bytes);
    dma_sync_for_device(device.report_buffer_dma);

    xhci::TrbFields trb{};
    trb.parameter = device.report_buffer_dma.physical_address;
    trb.status = static_cast<uint32_t>(device.report_buffer_dma.size_bytes);
    trb.control = (xhci::kTrbTypeNormal << 10) | kTrbInterruptOnShortPacket |
                  kTrbInterruptOnCompletion;
    uint64_t trb_physical = 0;
    if(!xhci::enqueue_command_trb(device.interrupt_ring_dma.virtual_address,
                                  device.interrupt_ring_dma.size_bytes,
                                  device.interrupt_ring_dma.physical_address,
                                  device.interrupt_ring,
                                  trb,
                                  &trb_physical))
    {
        return false;
    }

    device.interrupt_transfer_trb = trb_physical;
    device.interrupt_transfer_armed = true;
    dma_sync_for_device(device.interrupt_ring_dma);
    xhci::ring_doorbell(controller.controller, device.slot_id, device.endpoint_dci);
    return true;
}

void process_transfer_event(XhciControllerState& controller, const xhci::EventInfo& event)
{
    for(auto& device : controller.hid_devices)
    {
        if(!device.present)
        {
            continue;
        }

        if((device.synchronous_transfer_trb != 0u) &&
           (device.synchronous_transfer_trb == event.parameter))
        {
            device.synchronous_transfer_complete = true;
            device.synchronous_transfer_completion_code = event.completion_code;
            device.synchronous_transfer_residual = event.transfer_length;
            device.synchronous_transfer_trb = 0u;
            return;
        }

        if(device.interrupt_transfer_armed && (device.interrupt_transfer_trb == event.parameter))
        {
            device.interrupt_transfer_armed = false;
            dma_sync_for_cpu(device.report_buffer_dma);
            if(transfer_completion_ok(event.completion_code))
            {
                if(HidBootProtocol::Keyboard == device.protocol)
                {
                    handle_keyboard_report(device);
                }
                else if(HidBootProtocol::Mouse == device.protocol)
                {
                    handle_mouse_report(device);
                }
            }
            (void)arm_interrupt_transfer(controller, device);
            return;
        }
    }
}

void process_command_event(XhciControllerState& controller, const xhci::EventInfo& event)
{
    if((controller.pending_command_trb != 0u) && (controller.pending_command_trb == event.parameter))
    {
        controller.pending_command_complete = true;
        controller.pending_command_completion_code = event.completion_code;
        controller.pending_command_slot_id = event.slot_id;
        controller.pending_command_trb = 0u;
    }
}

void service_events(XhciControllerState& controller)
{
    bool consumed_event = false;
    dma_sync_for_cpu(controller.event_ring_dma);

    xhci::EventInfo event{};
    while(xhci::dequeue_event_trb(controller.event_ring_dma.virtual_address,
                                  controller.event_ring_dma.size_bytes,
                                  controller.event_ring_dma.physical_address,
                                  controller.event_ring,
                                  event))
    {
        consumed_event = true;
        switch(event.type)
        {
        case xhci::EventType::CommandCompletion:
            process_command_event(controller, event);
            break;
        case xhci::EventType::Transfer:
            process_transfer_event(controller, event);
            break;
        case xhci::EventType::PortStatusChange:
            (void)xhci::clear_port_change_bits(controller.controller, event.port_id);
            break;
        default:
            break;
        }
    }

    if(consumed_event)
    {
        xhci::acknowledge_interrupt(controller.controller, controller.event_ring.dequeue_physical);
    }
}

bool wait_for_command_completion(XhciControllerState& controller, uint32_t timeout_spins)
{
    for(uint32_t iteration = 0; iteration < timeout_spins; ++iteration)
    {
        service_events(controller);
        if(controller.pending_command_complete)
        {
            return true;
        }
        cpu_pause();
    }

    service_events(controller);
    return controller.pending_command_complete;
}

bool submit_command(XhciControllerState& controller,
                    const xhci::TrbFields& trb,
                    uint8_t& completion_code,
                    uint8_t& slot_id,
                    uint32_t timeout_spins)
{
    completion_code = 0u;
    slot_id = 0u;
    controller.pending_command_complete = false;
    controller.pending_command_completion_code = 0u;
    controller.pending_command_slot_id = 0u;

    uint64_t trb_physical = 0;
    if(!xhci::enqueue_command_trb(controller.command_ring_dma.virtual_address,
                                  controller.command_ring_dma.size_bytes,
                                  controller.command_ring_dma.physical_address,
                                  controller.command_ring,
                                  trb,
                                  &trb_physical))
    {
        return false;
    }

    controller.pending_command_trb = trb_physical;
    dma_sync_for_device(controller.command_ring_dma);
    xhci::ring_doorbell(controller.controller, 0u, 0u);
    if(!wait_for_command_completion(controller, timeout_spins))
    {
        controller.pending_command_trb = 0u;
        return false;
    }

    completion_code = controller.pending_command_completion_code;
    slot_id = controller.pending_command_slot_id;
    controller.pending_command_complete = false;
    return kXhciCompletionSuccess == completion_code;
}

bool wait_for_transfer_completion(XhciControllerState& controller,
                                  XhciHidDeviceState& device,
                                  uint32_t timeout_spins,
                                  uint8_t& completion_code,
                                  uint32_t& residual_length)
{
    completion_code = 0u;
    residual_length = 0u;
    for(uint32_t iteration = 0; iteration < timeout_spins; ++iteration)
    {
        service_events(controller);
        if(device.synchronous_transfer_complete)
        {
            completion_code = device.synchronous_transfer_completion_code;
            residual_length = device.synchronous_transfer_residual;
            device.synchronous_transfer_complete = false;
            return true;
        }
        cpu_pause();
    }

    service_events(controller);
    if(!device.synchronous_transfer_complete)
    {
        return false;
    }

    completion_code = device.synchronous_transfer_completion_code;
    residual_length = device.synchronous_transfer_residual;
    device.synchronous_transfer_complete = false;
    return true;
}

bool submit_control_transfer(XhciControllerState& controller,
                             XhciHidDeviceState& device,
                             const UsbSetupPacket& setup,
                             const void* out_buffer,
                             void* in_buffer,
                             size_t buffer_length,
                             size_t& actual_length)
{
    actual_length = 0;
    if(buffer_length > device.control_buffer_dma.size_bytes)
    {
        return false;
    }

    const bool has_data_stage = buffer_length > 0u;
    const bool direction_in = 0 != (setup.request_type & kUsbEndpointDirectionIn);
    if(has_data_stage)
    {
        if(direction_in)
        {
            memset(device.control_buffer_dma.virtual_address, 0, buffer_length);
        }
        else if((nullptr == out_buffer) || (buffer_length > 0u && nullptr == device.control_buffer_dma.virtual_address))
        {
            return false;
        }
        else
        {
            memcpy(device.control_buffer_dma.virtual_address, out_buffer, buffer_length);
        }
        dma_sync_for_device(device.control_buffer_dma);
    }

    device.synchronous_transfer_complete = false;
    device.synchronous_transfer_completion_code = 0u;
    device.synchronous_transfer_residual = 0u;
    device.synchronous_transfer_trb = 0u;

    uint64_t status_trb_physical = 0;
    const uint64_t setup_parameter = static_cast<uint64_t>(setup.request_type) |
                                     (static_cast<uint64_t>(setup.request) << 8) |
                                     (static_cast<uint64_t>(setup.value) << 16) |
                                     (static_cast<uint64_t>(setup.index) << 32) |
                                     (static_cast<uint64_t>(setup.length) << 48);
    const uint32_t setup_transfer_type = !has_data_stage
                                             ? kSetupTransferTypeNoData
                                             : (direction_in ? kSetupTransferTypeIn : kSetupTransferTypeOut);
    xhci::TrbFields setup_trb{
        .parameter = setup_parameter,
        .status = sizeof(UsbSetupPacket),
        .control = (xhci::kTrbTypeSetupStage << 10) | kTrbImmediateData | kTrbChainBit |
                   setup_transfer_type,
    };
    if(!xhci::enqueue_command_trb(device.control_ring_dma.virtual_address,
                                  device.control_ring_dma.size_bytes,
                                  device.control_ring_dma.physical_address,
                                  device.control_ring,
                                  setup_trb))
    {
        return false;
    }

    if(has_data_stage)
    {
        xhci::TrbFields data_trb{
            .parameter = device.control_buffer_dma.physical_address,
            .status = static_cast<uint32_t>(buffer_length),
            .control = (xhci::kTrbTypeDataStage << 10) | kTrbChainBit |
                       (direction_in ? kDataDirectionIn : 0u),
        };
        if(!xhci::enqueue_command_trb(device.control_ring_dma.virtual_address,
                                      device.control_ring_dma.size_bytes,
                                      device.control_ring_dma.physical_address,
                                      device.control_ring,
                                      data_trb))
        {
            return false;
        }
    }

    xhci::TrbFields status_trb{
        .parameter = 0u,
        .status = 0u,
        .control = (xhci::kTrbTypeStatusStage << 10) | kTrbInterruptOnCompletion |
                   ((!has_data_stage || !direction_in) ? kStatusDirectionIn : 0u),
    };
    if(!xhci::enqueue_command_trb(device.control_ring_dma.virtual_address,
                                  device.control_ring_dma.size_bytes,
                                  device.control_ring_dma.physical_address,
                                  device.control_ring,
                                  status_trb,
                                  &status_trb_physical))
    {
        return false;
    }

    device.synchronous_transfer_trb = status_trb_physical;
    dma_sync_for_device(device.control_ring_dma);
    xhci::ring_doorbell(controller.controller, device.slot_id, kXhciControlEndpointDci);

    uint8_t completion_code = 0u;
    uint32_t residual_length = 0u;
    if(!wait_for_transfer_completion(controller,
                                     device,
                                     kXhciTransferTimeoutSpins,
                                     completion_code,
                                     residual_length) ||
       !transfer_completion_ok(completion_code))
    {
        return false;
    }

    if(has_data_stage && direction_in)
    {
        dma_sync_for_cpu(device.control_buffer_dma);
        actual_length = buffer_length - ((residual_length <= buffer_length) ? residual_length : buffer_length);
        if((nullptr != in_buffer) && (actual_length > 0u))
        {
            memcpy(in_buffer, device.control_buffer_dma.virtual_address, actual_length);
        }
    }
    return true;
}

bool usb_get_descriptor(XhciControllerState& controller,
                        XhciHidDeviceState& device,
                        uint8_t descriptor_type,
                        uint8_t descriptor_index,
                        void* buffer,
                        size_t buffer_length,
                        size_t& actual_length)
{
    const UsbSetupPacket setup{
        .request_type = kUsbRequestTypeStandardInDevice,
        .request = kUsbRequestGetDescriptor,
        .value = static_cast<uint16_t>((descriptor_type << 8) | descriptor_index),
        .index = 0u,
        .length = static_cast<uint16_t>(buffer_length),
    };
    return submit_control_transfer(controller,
                                   device,
                                   setup,
                                   nullptr,
                                   buffer,
                                   buffer_length,
                                   actual_length);
}

bool usb_set_configuration(XhciControllerState& controller,
                           XhciHidDeviceState& device,
                           uint8_t configuration_value)
{
    const UsbSetupPacket setup{
        .request_type = kUsbRequestTypeStandardOutDevice,
        .request = kUsbRequestSetConfiguration,
        .value = configuration_value,
        .index = 0u,
        .length = 0u,
    };
    size_t actual_length = 0u;
    return submit_control_transfer(controller,
                                   device,
                                   setup,
                                   nullptr,
                                   nullptr,
                                   0u,
                                   actual_length);
}

bool usb_set_boot_protocol(XhciControllerState& controller, XhciHidDeviceState& device)
{
    const UsbSetupPacket setup{
        .request_type = kUsbRequestTypeClassOutInterface,
        .request = kUsbHidRequestSetProtocol,
        .value = 0u,
        .index = device.interface_number,
        .length = 0u,
    };
    size_t actual_length = 0u;
    return submit_control_transfer(controller,
                                   device,
                                   setup,
                                   nullptr,
                                   nullptr,
                                   0u,
                                   actual_length);
}

bool parse_hid_boot_configuration(const void* buffer, size_t buffer_length, HidInterfaceSelection& out)
{
    out = {};
    if((nullptr == buffer) || (buffer_length < sizeof(UsbConfigurationDescriptor)))
    {
        return false;
    }

    const auto* bytes = static_cast<const uint8_t*>(buffer);
    uint8_t active_configuration = 0u;
    bool active_interface_matches = false;
    HidBootProtocol active_protocol = HidBootProtocol::None;
    uint8_t active_interface_number = 0u;
    for(size_t offset = 0; (offset + 2u) <= buffer_length;)
    {
        const uint8_t length = bytes[offset];
        const uint8_t descriptor_type = bytes[offset + 1u];
        if((length < 2u) || ((offset + length) > buffer_length))
        {
            break;
        }

        switch(descriptor_type)
        {
        case kUsbDescriptorConfiguration: {
            const auto* descriptor = reinterpret_cast<const UsbConfigurationDescriptor*>(bytes + offset);
            active_configuration = descriptor->configuration_value;
            active_interface_matches = false;
            active_protocol = HidBootProtocol::None;
            break;
        }
        case kUsbDescriptorInterface: {
            const auto* descriptor = reinterpret_cast<const UsbInterfaceDescriptor*>(bytes + offset);
            active_interface_number = descriptor->interface_number;
            active_interface_matches = (0u == descriptor->alternate_setting) &&
                                       (kUsbClassHid == descriptor->interface_class) &&
                                       (kUsbSubclassBoot == descriptor->interface_subclass) &&
                                       ((kUsbProtocolKeyboard == descriptor->interface_protocol) ||
                                        (kUsbProtocolMouse == descriptor->interface_protocol));
            active_protocol = (kUsbProtocolKeyboard == descriptor->interface_protocol)
                                  ? HidBootProtocol::Keyboard
                                  : ((kUsbProtocolMouse == descriptor->interface_protocol)
                                         ? HidBootProtocol::Mouse
                                         : HidBootProtocol::None);
            break;
        }
        case kUsbDescriptorEndpoint:
            if(active_interface_matches)
            {
                const auto* descriptor = reinterpret_cast<const UsbEndpointDescriptor*>(bytes + offset);
                if(((descriptor->endpoint_address & kUsbEndpointDirectionIn) != 0u) &&
                   ((descriptor->attributes & 0x03u) == kUsbEndpointTransferInterrupt))
                {
                    out.valid = true;
                    out.protocol = active_protocol;
                    out.configuration_value = active_configuration;
                    out.interface_number = active_interface_number;
                    out.endpoint_address = descriptor->endpoint_address;
                    out.interval = descriptor->interval;
                    out.max_packet_size = static_cast<uint16_t>(descriptor->max_packet_size & 0x07FFu);
                    return true;
                }
            }
            break;
        default:
            break;
        }

        offset += length;
    }

    return false;
}

bool reset_port(XhciControllerState& controller, uint8_t port_id)
{
    xhci::PortStatus status{};
    if(!xhci::read_port_status(controller.controller, port_id, status) || !status.connected)
    {
        return false;
    }

    if(!status.powered)
    {
        (void)xhci::power_on_port(controller.controller, port_id);
    }
    (void)xhci::clear_port_change_bits(controller.controller, port_id);

    const size_t port_offset = xhci::kOperationalPortRegisterBase +
                               (static_cast<size_t>(port_id) - 1u) * xhci::kPortRegisterStride +
                               xhci::kPortSc;
    *reinterpret_cast<volatile uint32_t*>(controller.controller.operational + port_offset) =
        (status.raw & ~xhci::kPortScChangeBits) | xhci::kPortScPortPower | xhci::kPortScPortReset;

    for(uint32_t iteration = 0; iteration < kXhciPortTimeoutSpins; ++iteration)
    {
        if(!xhci::read_port_status(controller.controller, port_id, status))
        {
            return false;
        }
        if(status.enabled && !status.reset)
        {
            (void)xhci::clear_port_change_bits(controller.controller, port_id);
            return true;
        }
        cpu_pause();
    }

    return false;
}

bool address_device(XhciControllerState& controller, XhciHidDeviceState& device)
{
    memset(device.input_context_dma.virtual_address, 0, device.input_context_dma.size_bytes);
    auto* control = input_control_context(device);
    control[0] = 0u;
    control[1] = kInputContextAddSlot | kInputContextAddEp0;
    configure_slot_context(controller, device, kXhciControlEndpointDci);
    configure_endpoint_context(controller,
                               device,
                               kXhciControlEndpointDci,
                               4u,
                               device.control_max_packet_size,
                               0u,
                               device.control_ring_dma,
                               device.control_max_packet_size);
    dma_sync_for_device(device.input_context_dma);

    xhci::TrbFields command{};
    command.parameter = device.input_context_dma.physical_address;
    command.control = (xhci::kTrbTypeAddressDeviceCommand << 10) |
                      (static_cast<uint32_t>(device.slot_id) << 24);
    uint8_t completion_code = 0u;
    uint8_t slot_id = 0u;
    return submit_command(controller, command, completion_code, slot_id, kXhciInitTimeoutSpins) &&
           (slot_id == device.slot_id);
}

bool configure_interrupt_endpoint(XhciControllerState& controller, XhciHidDeviceState& device)
{
    memset(device.input_context_dma.virtual_address, 0, device.input_context_dma.size_bytes);
    auto* control = input_control_context(device);
    control[0] = 0u;
    control[1] = kInputContextAddSlot | (1u << device.endpoint_dci);
    configure_slot_context(controller, device, device.endpoint_dci);
    configure_endpoint_context(controller,
                               device,
                               device.endpoint_dci,
                               7u,
                               device.endpoint_max_packet_size,
                               (device.endpoint_interval > 0u)
                                   ? static_cast<uint8_t>(device.endpoint_interval - 1u)
                                   : 0u,
                               device.interrupt_ring_dma,
                               device.endpoint_max_packet_size);
    dma_sync_for_device(device.input_context_dma);

    xhci::TrbFields command{};
    command.parameter = device.input_context_dma.physical_address;
    command.control = (xhci::kTrbTypeConfigureEndpointCommand << 10) |
                      (static_cast<uint32_t>(device.slot_id) << 24);
    uint8_t completion_code = 0u;
    uint8_t slot_id = 0u;
    return submit_command(controller, command, completion_code, slot_id, kXhciInitTimeoutSpins) &&
           (slot_id == device.slot_id);
}

bool initialize_hid_port(PageFrameContainer& frames, XhciControllerState& controller, uint8_t port_id)
{
    xhci::PortStatus status{};
    if(!xhci::read_port_status(controller.controller, port_id, status) || !status.connected)
    {
        return false;
    }

    if(!status.powered)
    {
        (void)xhci::power_on_port(controller.controller, port_id);
    }
    if(!status.enabled && !reset_port(controller, port_id))
    {
        debug("xhci: port reset failed port=")(port_id)();
        return false;
    }
    if(!xhci::read_port_status(controller.controller, port_id, status))
    {
        return false;
    }

    uint8_t completion_code = 0u;
    uint8_t slot_id = 0u;
    xhci::TrbFields enable_slot{};
    enable_slot.control = xhci::kTrbTypeEnableSlotCommand << 10;
    if(!submit_command(controller, enable_slot, completion_code, slot_id, kXhciInitTimeoutSpins) ||
       (0u == slot_id))
    {
        debug("xhci: enable slot failed port=")(port_id)(" cc=")(completion_code)();
        return false;
    }

    XhciHidDeviceState* device = allocate_hid_device(controller);
    if(nullptr == device)
    {
        debug("xhci: HID device table full")();
        return false;
    }

    *device = {};
    device->present = true;
    device->slot_id = slot_id;
    device->port_id = port_id;
    device->speed = status.speed;
    device->control_max_packet_size = control_max_packet_from_speed(status.speed);
    if(!allocate_hid_device_resources(frames, controller, *device))
    {
        debug("xhci: HID device allocation failed port=")(port_id)();
        release_hid_device(frames, controller, *device);
        return false;
    }
    set_dcbaa_entry(controller, device->slot_id, device->device_context_dma.physical_address);

    if(!address_device(controller, *device))
    {
        debug("xhci: address device failed port=")(port_id)();
        release_hid_device(frames, controller, *device);
        return false;
    }

    UsbDeviceDescriptor descriptor{};
    size_t actual_length = 0u;
    if(!usb_get_descriptor(controller,
                           *device,
                           kUsbDescriptorDevice,
                           0u,
                           &descriptor,
                           sizeof(descriptor),
                           actual_length) ||
       (actual_length < sizeof(descriptor)))
    {
        debug("xhci: device descriptor failed port=")(port_id)();
        release_hid_device(frames, controller, *device);
        return false;
    }

    uint8_t config_header[sizeof(UsbConfigurationDescriptor)]{};
    if(!usb_get_descriptor(controller,
                           *device,
                           kUsbDescriptorConfiguration,
                           0u,
                           config_header,
                           sizeof(config_header),
                           actual_length) ||
       (actual_length < sizeof(UsbConfigurationDescriptor)))
    {
        debug("xhci: config header failed port=")(port_id)();
        release_hid_device(frames, controller, *device);
        return false;
    }

    const auto* configuration = reinterpret_cast<const UsbConfigurationDescriptor*>(config_header);
    if((0u == configuration->total_length) ||
       (configuration->total_length > device->control_buffer_dma.size_bytes))
    {
        debug("xhci: config length unsupported port=")(port_id)(" total=")(configuration->total_length)();
        release_hid_device(frames, controller, *device);
        return false;
    }

    HidInterfaceSelection hid{};
    if(!usb_get_descriptor(controller,
                           *device,
                           kUsbDescriptorConfiguration,
                           0u,
                           device->control_buffer_dma.virtual_address,
                           configuration->total_length,
                           actual_length) ||
       !parse_hid_boot_configuration(device->control_buffer_dma.virtual_address, actual_length, hid))
    {
        debug("xhci: no HID boot interface port=")(port_id)();
        release_hid_device(frames, controller, *device);
        return false;
    }

    device->protocol = hid.protocol;
    device->configuration_value = hid.configuration_value;
    device->interface_number = hid.interface_number;
    device->endpoint_address = hid.endpoint_address;
    device->endpoint_dci = endpoint_dci_from_address(hid.endpoint_address);
    device->endpoint_interval = hid.interval;
    device->endpoint_max_packet_size = (0u != hid.max_packet_size)
                                           ? hid.max_packet_size
                                           : static_cast<uint16_t>(kXhciReportBufferBytes);

    if(!usb_set_configuration(controller, *device, device->configuration_value) ||
       !configure_interrupt_endpoint(controller, *device) ||
       !usb_set_boot_protocol(controller, *device))
    {
        debug("xhci: HID endpoint setup failed port=")(port_id)();
        release_hid_device(frames, controller, *device);
        return false;
    }

    device->configured = true;
    if(!arm_interrupt_transfer(controller, *device))
    {
        debug("xhci: interrupt arm failed port=")(port_id)();
        release_hid_device(frames, controller, *device);
        return false;
    }

    debug("xhci: HID ")((HidBootProtocol::Keyboard == device->protocol) ? "keyboard" : "mouse")(
        " ready port=")(port_id)(" slot=")(device->slot_id)(" ep=0x")(device->endpoint_address,
                                                                      16,
                                                                      2)(" mps=")(
        device->endpoint_max_packet_size)();
    return true;
}

void enumerate_ports(PageFrameContainer& frames, XhciControllerState& controller)
{
    for(uint8_t port_id = 1u; port_id <= controller.controller.capability.max_ports; ++port_id)
    {
        xhci::PortStatus status{};
        if(xhci::read_port_status(controller.controller, port_id, status) && status.connected)
        {
            (void)initialize_hid_port(frames, controller, port_id);
        }
    }
}

void xhci_irq(void* data)
{
    auto& controller = *static_cast<XhciControllerState*>(data);
    ++controller.interrupt_count;
    service_events(controller);
}

bool probe_xhci_controller(VirtualMemory& kernel_vm,
                           PageFrameContainer& frames,
                           const PciDevice& device,
                           size_t device_index,
                           DeviceId id)
{
    XhciControllerState* controller = allocate_controller();
    if(nullptr == controller)
    {
        debug("xhci: controller table full")();
        return false;
    }

    *controller = {};
    controller->owner = id;
    controller->pci_index = static_cast<uint16_t>(device_index);
    debug("xhci: probe pci=")(device.bus, 16, 2)(":")(device.slot, 16, 2)(".")(device.function, 16, 1)(
        " id=")(device.vendor_id, 16, 4)(":")(device.device_id, 16, 4)(" cmd=0x")(pci_read_command(device), 16, 4)();

    if(!claim_pci_bar(id, static_cast<uint16_t>(device_index), 0, controller->controller_bar))
    {
        debug("xhci: BAR0 claim failed")();
        return false;
    }
    debug("xhci: BAR0 base=0x")(controller->controller_bar.base, 16)(" size=0x")(
        controller->controller_bar.size, 16)();
    if((PciBarType::Mmio32 != controller->controller_bar.type) &&
       (PciBarType::Mmio64 != controller->controller_bar.type))
    {
        debug("xhci: BAR0 is not MMIO")();
        release_controller(frames, *controller);
        return false;
    }
    if((0 == controller->controller_bar.base) || (controller->controller_bar.size < 0x1000u) ||
       (controller->controller_bar.size > kXhciMaxBarBytes))
    {
        debug("xhci: BAR0 implausible; skipping controller")();
        release_controller(frames, *controller);
        return false;
    }
    if(!map_mmio_range(kernel_vm, controller->controller_bar.base, controller->controller_bar.size))
    {
        debug("xhci: MMIO map failed")();
        release_controller(frames, *controller);
        return false;
    }

    // Firmware that never used this controller may leave memory decoding off;
    // every register would then read as all ones.
    pci_set_command_bits(device, kPciCommandMemorySpace);
    controller->controller.mmio_base =
        kernel_physical_pointer<volatile uint8_t>(controller->controller_bar.base);
    controller->controller.mmio_size = static_cast<size_t>(controller->controller_bar.size);
    const uint32_t capability_word = *reinterpret_cast<volatile uint32_t*>(controller->controller.mmio_base);
    debug("xhci: capability word=0x")(capability_word, 16, 8)();
    if(0xFFFFFFFFu == capability_word)
    {
        debug("xhci: MMIO reads all ones; skipping controller")();
        release_controller(frames, *controller);
        return false;
    }
    if(!xhci::parse_capability(controller->controller.mmio_base,
                               controller->controller.mmio_size,
                               controller->controller))
    {
        debug("xhci: capability decode failed")();
        release_controller(frames, *controller);
        return false;
    }
    if((0 == controller->controller.capability.max_slots) ||
       (0 == controller->controller.capability.max_ports) ||
       (0 == controller->controller.capability.max_interrupters) ||
       (0 == (xhci::read_page_size_mask(controller->controller) & 0x1u)))
    {
        debug("xhci: controller capability unsupported")();
        release_controller(frames, *controller);
        return false;
    }

    debug("xhci: capability ok ports=")(controller->controller.capability.max_ports)(" slots=")(
        controller->controller.capability.max_slots)(" scratchpad=")(
        controller->controller.capability.scratchpad_buffers)();
    controller->configured_slots = controller->controller.capability.max_slots;
    controller->scratchpad_count = controller->controller.capability.scratchpad_buffers;
    if(!allocate_controller_buffers(frames, *controller))
    {
        debug("xhci: DMA allocation failed")();
        release_controller(frames, *controller);
        return false;
    }

    debug("xhci: buffers ok, binding interrupt")();
    if(!pci_enable_best_interrupt(kernel_vm, id, device, 0, xhci_irq, controller, controller->interrupt))
    {
        debug("xhci: interrupt bind failed")();
        release_controller(frames, *controller);
        return false;
    }
    debug("xhci: interrupt bound vector=0x")(controller->interrupt.vector, 16, 2)(", resetting")();

    if(!xhci::controller_reset(controller->controller, kXhciInitTimeoutSpins))
    {
        debug("xhci: controller reset failed")();
        release_controller(frames, *controller);
        return false;
    }
    pci_enable_mmio_bus_mastering(device);

    const xhci::RingSet rings{
        .dcbaa_physical = controller->dcbaa_dma.physical_address,
        .command_ring_physical = controller->command_ring_dma.physical_address,
        .event_ring_physical = controller->event_ring_dma.physical_address,
        .erst_physical = controller->erst_dma.physical_address,
        .command_ring_cycle_state = controller->command_ring.cycle_state,
        .event_ring_trb_count =
            static_cast<uint32_t>(controller->event_ring_dma.size_bytes / xhci::kTrbBytes),
    };
    if(!xhci::controller_program(controller->controller, rings, controller->configured_slots) ||
       !xhci::controller_run(controller->controller, kXhciInitTimeoutSpins))
    {
        debug("xhci: controller start failed")();
        release_controller(frames, *controller);
        return false;
    }

    enumerate_ports(frames, *controller);

    controller->present = true;
    (void)device_binding_publish(id, static_cast<uint16_t>(device_index), "xhci", controller);
    (void)device_binding_set_state(id, DeviceState::Started);

    debug("xhci: ready pci=")(device.bus, 16, 2)(":")(device.slot, 16, 2)(".")(
        device.function, 16, 1)(" ports=")(controller->controller.capability.max_ports)(" slots=")(
        controller->configured_slots)(" irq=")(controller->interrupt.vector, 16, 2)(" scratch=")(
        controller->scratchpad_count)();
    kernel_event::record(OS1_KERNEL_EVENT_PCI_BIND,
                         OS1_KERNEL_EVENT_FLAG_SUCCESS,
                         static_cast<uint64_t>(device_index),
                         pci_bdf(device),
                         (static_cast<uint64_t>(device.vendor_id) << 16) | device.device_id,
                         controller->controller.capability.max_ports);
    return true;
}
}  // namespace

const PciDriver& xhci_pci_driver()
{
    static const PciDriver driver{
        .name = "xhci",
        .matches = kXhciMatches,
        .match_count = sizeof(kXhciMatches) / sizeof(kXhciMatches[0]),
        .probe = probe_xhci_controller,
        .remove = [](DeviceId id) {
            if(XhciControllerState* controller = find_controller(id))
            {
                release_controller(page_frames, *controller);
                device_binding_remove(id);
            }
        },
    };
    return driver;
}