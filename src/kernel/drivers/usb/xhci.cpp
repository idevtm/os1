#include "drivers/usb/diagnostics.hpp"
#include "platform/hardware_catalog.hpp"
#include "proc/thread.hpp"
#include "util/fixed_string.hpp"
// Early xHCI PCI driver. This owns controller reset, MMIO resource claims,
// DMA ring allocation, interrupter 0 setup, and lifecycle teardown.
#include "console/console_input.hpp"
#include "debug/bootlog.hpp"
#include "debug/debug.hpp"
#include "debug/event_ring.hpp"
#include "drivers/bus/device.hpp"
#include "drivers/bus/resource.hpp"
#include "drivers/input/service.hpp"
#include "drivers/timer/tsc_clock.hpp"
#include "drivers/usb/hid_keyboard.hpp"
#include "drivers/usb/mass_storage.hpp"
#include "drivers/usb/xhci.hpp"
#include "drivers/usb/xhci_controller.hpp"
#include "handoff/memory_layout.h"
#include "input/hid_boot.hpp"
#include "mm/boot_mapping.hpp"
#include "mm/dma.hpp"
#include "mm/page_frame.hpp"
#include "platform/irq_registry.hpp"
#include "platform/pci_config.hpp"
#include "platform/pci_msi.hpp"
#include "platform/state.hpp"
#include "storage/block_device.hpp"
#include "sync/smp.hpp"
#include "util/memory.h"

extern PageFrameContainer page_frames;
inline void* operator new(size_t, void* location) noexcept
{
    return location;
}

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
        .match_flags =
            static_cast<uint8_t>(kPciMatchClassCode | kPciMatchSubclass | kPciMatchProgIf),
    },
};
constexpr size_t kXhciMaxControllers = 4u;
constexpr size_t kXhciMaxDevices = 16u;
constexpr size_t kXhciCommandRingBytes = kPageSize;
constexpr size_t kXhciEventRingBytes = kPageSize;
constexpr size_t kXhciTransferRingBytes = kPageSize;
constexpr size_t kXhciControlBufferBytes = 512u;
constexpr uint32_t kXhciInitTimeoutSpins = 1000000u;
constexpr uint64_t kXhciMaxBarBytes = 16ull * 1024 * 1024;
constexpr uint16_t kPciCommandMemorySpace = 0x0002u;
constexpr uint32_t kXhciStalledClockPolls = 1000000u;
constexpr uint32_t kXhciBulkStalledClockPolls = 10000000u;
constexpr uint64_t kXhciCommandTimeout100ns = 10000000u;  // 1 s.
constexpr uint64_t kXhciControlTimeout100ns = 10000000u;  // 1 s.
constexpr uint64_t kXhciBulkTimeout100ns = 20000000u;     // 2 s.
constexpr uint64_t kXhciPortTimeout100ns = 10000000u;     // 1 s.
constexpr uint64_t kXhciAbortTimeout100ns = 50000000u;    // 5 s.

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
constexpr uint32_t kTrbInterruptOnCompletion = 1u << 5;
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

constexpr unsigned kXhciInputInterfaces = 2;
struct XhciInputState
{
    uint64_t input_identity = 0;
    unsigned catalog_id = 0;
    HidBootProtocol protocol = HidBootProtocol::None;
    uint8_t interface_number = 0;
    uint8_t endpoint_address = 0;
    uint8_t endpoint_dci = 0;
    uint8_t endpoint_interval = 0;
    uint16_t endpoint_max_packet_size = 8;
    DmaBuffer interrupt_ring_dma{}, report_buffer_dma{};
    xhci::CommandRingInfo interrupt_ring{};
    usb::hid::BootKeyboardState keyboard_state{};
    xhci::HidRecovery recovery{};
    bool interrupt_transfer_armed = false;
    uint64_t interrupt_transfer_trb = 0;
    uint32_t report_count = 0;
    uint8_t diagnostic_reports = 0;
};

struct XhciControllerState;
struct XhciDeviceState
{
    bool present = false;
    bool configured = false;
    bool failed = false;
    bool storage_published = false;
    bool dma_visible = false;
    XhciControllerState* controller = nullptr;
    usb::storage::Interface storage_interface{};
    usb::storage::Device storage{};
    BlockDevice block{};
    unsigned catalog_id{};
    DmaBuffer bulk_in_ring_dma{}, bulk_out_ring_dma{}, bulk_buffer_dma{};
    xhci::CommandRingInfo bulk_in_ring{}, bulk_out_ring{};
    uint8_t synchronous_endpoint = 1;

    uint8_t port_id = 0;
    uint8_t slot_id = 0;
    uint8_t speed = 0;
    uint8_t configuration_value = 0;
    XhciInputState inputs[kXhciInputInterfaces]{};
    unsigned input_count = 0;
    uint16_t control_max_packet_size = 8;
    DmaBuffer input_context_dma{};
    DmaBuffer device_context_dma{};
    DmaBuffer control_ring_dma{};
    xhci::CommandRingInfo control_ring{};
    DmaBuffer control_buffer_dma{};
    xhci::ControlTransfer control_transfer{};
    bool synchronous_transfer_complete = false;
    uint8_t synchronous_transfer_completion_code = 0;
    uint32_t synchronous_transfer_residual = 0;
    uint64_t synchronous_transfer_trb = 0;
    uint8_t diagnostic_controls = 0;
    uint8_t diagnostic_ep0_events = 0;
};

struct XhciControllerState
{
    bool owned = false;
    bool present = false;
    bool failed = false;
    bool dma_published = false;
    uint8_t storage_busy = 0;
    bool port_changed[256]{};
    unsigned catalog_ports[256]{};
    DeviceId owner{DeviceBus::Platform, 0};
    uint16_t pci_index = 0;
    uint16_t configured_slots = 0;
    uint16_t scratchpad_count = 0;
    uint32_t interrupt_count = 0;
    uint8_t diagnostic_commands = 0;
    PciBarResource controller_bar{};
    PciInterruptHandle interrupt{};
    xhci::ControllerLayout controller{};
    xhci::CommandRingInfo command_ring{};
    xhci::EventRingInfo event_ring{};
    xhci::CommandCompletion command{};
    bool command_busy = false;
    DmaBuffer dcbaa_dma{};
    DmaBuffer scratchpad_array_dma{};
    DmaBuffer scratchpad_buffers_dma{};
    DmaBuffer command_ring_dma{};
    DmaBuffer event_ring_dma{};
    DmaBuffer erst_dma{};
    XhciDeviceState devices[kXhciMaxDevices]{};
};

// Construct only probed controllers. Default DMA fields otherwise turn this
// mostly-zero fixed pool into tens of KiB of initialized BIOS payload.
alignas(XhciControllerState) uint8_t
    g_controller_storage[kXhciMaxControllers][sizeof(XhciControllerState)]{};
XhciControllerState* g_xhci_controllers[kXhciMaxControllers]{};
Spinlock g_event_lock{"xhci-events"};
// IRQ and AP polling enqueue; only the BSP timer dispatches console input.
char g_input_chars[64]{};
size_t g_input_head = 0, g_input_count = 0;
enum class SlotDisableResult : uint8_t
{
    Disabled,
    Aborted,
    Failed,
};
SlotDisableResult disable_slot(XhciControllerState&, uint8_t);
bool initialize_storage(PageFrameContainer&,
                        XhciControllerState&,
                        XhciDeviceState&,
                        const usb::storage::Interface&);
void fail_device(XhciDeviceState& device)
{
    for(auto& input : device.inputs)
        native_input::remove(input.input_identity);
    __atomic_store_n(&device.failed, true, __ATOMIC_RELEASE);
    device.storage.invalidate();
}

[[nodiscard]] bool device_id_equal(DeviceId left, DeviceId right)
{
    return (left.bus == right.bus) && (left.index == right.index);
}

XhciControllerState* find_controller(DeviceId id)
{
    for(auto& controller : g_xhci_controllers)
    {
        if(controller && controller->present && device_id_equal(controller->owner, id))
        {
            return controller;
        }
    }
    return nullptr;
}

XhciControllerState* allocate_controller()
{
    for(size_t i = 0; i < kXhciMaxControllers; ++i)
    {
        auto*& controller = g_xhci_controllers[i];
        if(!controller)
            controller = new(g_controller_storage[i]) XhciControllerState{};
        if(!controller->present)
            return controller;
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

bool allocate_controller_dma(PageFrameContainer& frames,
                             const XhciControllerState& controller,
                             size_t bytes,
                             DmaBuffer& buffer)
{
    if(!dma_allocate_buffer(frames, controller.owner, bytes, DmaDirection::Bidirectional, buffer))
        return false;
    if(xhci::dma_range_supported(controller.controller.capability, buffer.physical_address, bytes))
        return true;
    release_dma(frames, buffer);
    debug("xhci: DMA range exceeds controller address width")();
    return false;
}

[[nodiscard]] bool device_has_resources(const XhciDeviceState& device)
{
    for(const auto& input : device.inputs)
        if(input.interrupt_ring_dma.active || input.report_buffer_dma.active)
            return true;
    return device.input_context_dma.active || device.device_context_dma.active ||
           device.control_ring_dma.active || device.control_buffer_dma.active ||
           device.bulk_in_ring_dma.active || device.bulk_out_ring_dma.active ||
           device.bulk_buffer_dma.active;
}

void cpu_pause()
{
#if defined(OS1_HOST_TEST)
    asm volatile("" : : : "memory");
#else
    ipi_poll_tlb_shootdown();
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

[[nodiscard]] uint32_t* input_control_context(XhciDeviceState& device)
{
    return static_cast<uint32_t*>(device.input_context_dma.virtual_address);
}

[[nodiscard]] uint32_t* input_slot_context(XhciControllerState& controller, XhciDeviceState& device)
{
    auto* base = static_cast<uint8_t*>(device.input_context_dma.virtual_address);
    return reinterpret_cast<uint32_t*>(base + controller.controller.capability.context_size_bytes);
}

[[nodiscard]] uint32_t* input_endpoint_context(XhciControllerState& controller,
                                               XhciDeviceState& device,
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
       (static_cast<size_t>(slot_id) >= (controller.dcbaa_dma.size_bytes / sizeof(uint64_t))))
    {
        return;
    }

    auto* dcbaa = static_cast<uint64_t*>(controller.dcbaa_dma.virtual_address);
    dcbaa[slot_id] = value;
    dma_sync_for_device(controller.dcbaa_dma);
}

XhciDeviceState* allocate_device(XhciControllerState& controller)
{
    for(auto& device : controller.devices)
    {
        if(!device.present && !device_has_resources(device))
        {
            return &device;
        }
    }
    return nullptr;
}

void release_device(PageFrameContainer& frames,
                    XhciControllerState& controller,
                    XhciDeviceState& device)
{
    fail_device(device);
    // Once Address Device can observe the contexts, retain all DMA until reboot.
    // Disable Slot alone does not retire an endpoint with an uncertain transfer.
    if(device.dma_visible || device.storage_published ||
       __atomic_load_n(&controller.failed, __ATOMIC_ACQUIRE) ||
       (device.slot_id && SlotDisableResult::Disabled != disable_slot(controller, device.slot_id)))
    {
        debug("xhci: device DMA quarantined port=")(device.port_id)();
        return;
    }
    set_dcbaa_entry(controller, device.slot_id, 0u);
    release_dma(frames, device.bulk_buffer_dma);
    release_dma(frames, device.bulk_out_ring_dma);
    release_dma(frames, device.bulk_in_ring_dma);
    for(auto& input : device.inputs)
    {
        release_dma(frames, input.report_buffer_dma);
        release_dma(frames, input.interrupt_ring_dma);
    }
    release_dma(frames, device.control_buffer_dma);
    release_dma(frames, device.control_ring_dma);
    release_dma(frames, device.device_context_dma);
    release_dma(frames, device.input_context_dma);
    device = {};
}

void release_controller(PageFrameContainer& frames, XhciControllerState& controller)
{
    if(controller.dma_published)
    {
        // Keep MMIO claims, rings and block-object addresses stable after publication.
        __atomic_store_n(&controller.failed, true, __ATOMIC_RELEASE);
        for(auto& device : controller.devices)
        {
            fail_device(device);
            hardware_catalog::unavailable(device.catalog_id);
        }
        (void)xhci::controller_stop(controller.controller, kXhciInitTimeoutSpins);
        if(controller.pci_index < kPlatformMaxPciDevices)
        {
            const auto& pci = g_platform.devices[controller.pci_index];
            pci_set_command_bits(pci, 0, 4);  // Stop bus mastering even if halt timed out.
            pci_release_interrupt(pci, controller.interrupt);
        }
        controller.present = true;  // Reserve the quarantined slot; never recycle handles.
        return;
    }
    if(controller.owned && nullptr != controller.controller.operational)
    {
        (void)xhci::controller_stop(controller.controller, kXhciInitTimeoutSpins);
    }
    if((PciInterruptMode::None != controller.interrupt.mode) &&
       (controller.pci_index < kPlatformMaxPciDevices))
    {
        pci_release_interrupt(g_platform.devices[controller.pci_index], controller.interrupt);
    }

    for(auto& device : controller.devices)
    {
        if(device.present || device_has_resources(device))
        {
            release_device(frames, controller, device);
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

    auto* scratchpad_array =
        static_cast<uint64_t*>(controller.scratchpad_array_dma.virtual_address);
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
    const size_t dcbaa_bytes =
        static_cast<size_t>(controller.configured_slots + 1u) * sizeof(uint64_t);
    if(!allocate_controller_dma(frames, controller, dcbaa_bytes, controller.dcbaa_dma) ||
       !allocate_controller_dma(
           frames, controller, kXhciCommandRingBytes, controller.command_ring_dma) ||
       !allocate_controller_dma(
           frames, controller, kXhciEventRingBytes, controller.event_ring_dma) ||
       !allocate_controller_dma(
           frames, controller, sizeof(xhci::EventRingSegmentTableEntry), controller.erst_dma))
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
    if(!xhci::initialize_event_ring_state(
           controller.event_ring_dma.physical_address, event_ring_trbs, controller.event_ring))
    {
        return false;
    }

    if(0 != controller.scratchpad_count)
    {
        if(!allocate_controller_dma(
               frames,
               controller,
               static_cast<size_t>(controller.scratchpad_count) * sizeof(uint64_t),
               controller.scratchpad_array_dma) ||
           !allocate_controller_dma(frames,
                                    controller,
                                    static_cast<size_t>(controller.scratchpad_count) * kPageSize,
                                    controller.scratchpad_buffers_dma))
        {
            return false;
        }
    }

    return prepare_dcbaa(controller);
}

void configure_slot_context(XhciControllerState& controller,
                            XhciDeviceState& device,
                            uint8_t context_entries)
{
    auto* slot = input_slot_context(controller, device);
    memset(slot, 0, controller.controller.capability.context_size_bytes);
    xhci::set_slot_context(slot, device.speed, device.port_id, context_entries);
}

void configure_endpoint_context(XhciControllerState& controller,
                                XhciDeviceState& device,
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
    endpoint[4] = average_trb_length | (endpoint_type == 7 ? uint32_t(max_packet_size) << 16 : 0);
}

bool allocate_device_resources(PageFrameContainer& frames,
                               XhciControllerState& controller,
                               XhciDeviceState& device)
{
    if(!allocate_controller_dma(
           frames, controller, input_context_bytes(controller), device.input_context_dma) ||
       !allocate_controller_dma(
           frames, controller, device_context_bytes(controller), device.device_context_dma) ||
       !allocate_controller_dma(
           frames, controller, kXhciTransferRingBytes, device.control_ring_dma) ||
       !allocate_controller_dma(
           frames, controller, kXhciControlBufferBytes, device.control_buffer_dma))
    {
        return false;
    }

    if(!xhci::initialize_command_ring(device.control_ring_dma.virtual_address,
                                      device.control_ring_dma.size_bytes,
                                      device.control_ring_dma.physical_address,
                                      device.control_ring))
    {
        return false;
    }

    memset(device.input_context_dma.virtual_address, 0, device.input_context_dma.size_bytes);
    memset(device.device_context_dma.virtual_address, 0, device.device_context_dma.size_bytes);
    dma_sync_for_device(device.input_context_dma);
    dma_sync_for_device(device.device_context_dma);
    return true;
}

void handle_keyboard_report(XhciDeviceState& device, XhciInputState& input)
{
    native_input::boot_keyboard(
        input.input_identity,
        static_cast<const uint8_t*>(input.report_buffer_dma.virtual_address),
        8);
    char output[6]{};
    size_t output_count = 0;
    if(!usb::hid::boot_keyboard_apply_report(
           input.keyboard_state,
           static_cast<const uint8_t*>(input.report_buffer_dma.virtual_address),
           usb::hid::kBootKeyboardReportBytes,
           output,
           sizeof(output),
           output_count))
    {
        return;
    }

    if(bootlog_diagnostics_enabled() && input.diagnostic_reports <= 8u)
        debug("usbdiag: decoded port=")(device.port_id)(" chars=")(output_count)(" queued=")(
            g_input_count)();
    for(size_t index = 0; index < output_count; ++index)
    {
        if(g_input_count < sizeof(g_input_chars))
        {
            g_input_chars[(g_input_head + g_input_count) % sizeof(g_input_chars)] = output[index];
            ++g_input_count;
        }  // Drop new input on overflow; never block a hardware event consumer.
    }
}

void handle_mouse_report(XhciInputState& input)
{
    native_input::boot_pointer(input.input_identity,
                               static_cast<const uint8_t*>(input.report_buffer_dma.virtual_address),
                               3);
    ++input.report_count;
}

bool arm_interrupt_transfer(XhciControllerState& controller,
                            XhciDeviceState& device,
                            XhciInputState& input)
{
    if(!device.present || !device.configured || __atomic_load_n(&device.failed, __ATOMIC_ACQUIRE))
    {
        return false;
    }

    memset(input.report_buffer_dma.virtual_address, 0, input.report_buffer_dma.size_bytes);
    dma_sync_for_device(input.report_buffer_dma);

    xhci::TrbFields trb{};
    trb.parameter = input.report_buffer_dma.physical_address;
    trb.status = input.endpoint_max_packet_size;
    trb.control =
        (xhci::kTrbTypeNormal << 10) | kTrbInterruptOnShortPacket | kTrbInterruptOnCompletion;
    uint64_t trb_physical = 0;
    if(!xhci::enqueue_command_trb(input.interrupt_ring_dma.virtual_address,
                                  input.interrupt_ring_dma.size_bytes,
                                  input.interrupt_ring_dma.physical_address,
                                  input.interrupt_ring,
                                  trb,
                                  &trb_physical))
    {
        return false;
    }

    input.interrupt_transfer_trb = trb_physical;
    input.interrupt_transfer_armed = true;
    dma_sync_for_device(input.interrupt_ring_dma);
    xhci::ring_doorbell(controller.controller, device.slot_id, input.endpoint_dci);
    return true;
}

void retire_input(XhciDeviceState& device, XhciInputState& input, uint8_t completion)
{
    debug("xhci: input retired port=")(device.port_id)(" dci=")(input.endpoint_dci)(" cc=")(
        completion)();
    native_input::remove(input.input_identity);
    input.interrupt_transfer_armed = false;
    input.recovery.pending = 0;
    hardware_catalog::unavailable(input.catalog_id);
}

void process_transfer_event(XhciControllerState& controller, const xhci::EventInfo& event)
{
    for(auto& device : controller.devices)
    {
        if(!device.present || event.slot_id != device.slot_id)
        {
            continue;
        }

        if(event.endpoint_id == kXhciControlEndpointDci &&
           xhci::control_transfer_event(device.control_transfer, event))
        {
            const auto& transfer = device.control_transfer;
            if(bootlog_diagnostics_enabled() && device.diagnostic_ep0_events < 64u)
            {
                ++device.diagnostic_ep0_events;
                debug("usbdiag: ep0 port=")(device.port_id)(" stage=")(
                    event.parameter == transfer.stages[0]
                        ? 0
                        : (event.parameter == transfer.stages[1] ? 1 : 2))(" cc=")(
                    event.completion_code)(" residual=")(event.transfer_length)();
            }
            if(transfer.done)
            {
                device.synchronous_transfer_complete = true;
                device.synchronous_transfer_completion_code = transfer.completion;
                device.synchronous_transfer_residual = transfer.residual;
            }
            return;
        }

        if(event.endpoint_id == device.synchronous_endpoint &&
           (device.synchronous_transfer_trb != 0u) &&
           (device.synchronous_transfer_trb == event.parameter))
        {
            device.synchronous_transfer_complete = true;
            device.synchronous_transfer_completion_code = event.completion_code;
            device.synchronous_transfer_residual = event.transfer_length;
            device.synchronous_transfer_trb = 0u;
            return;
        }

        for(auto& input : device.inputs)
        {
            if(xhci::matches_hid_transfer(event,
                                          device.slot_id,
                                          input.endpoint_dci,
                                          input.interrupt_transfer_trb,
                                          input.interrupt_transfer_armed))
            {
                if(bootlog_diagnostics_enabled() && input.diagnostic_reports < 8u)
                {
                    ++input.diagnostic_reports;
                    debug("usbdiag: input pci=")(controller.pci_index)(" port=")(device.port_id)(
                        " cc=")(event.completion_code)(" residual=")(event.transfer_length)();
                }
                else if(bootlog_diagnostics_enabled() && input.diagnostic_reports == 8u)
                    ++input.diagnostic_reports;
                input.interrupt_transfer_armed = false;
                const auto action = input.recovery.complete(event.completion_code);
                if(action == xhci::HidCompletion::Recover)
                    return;
                if(action == xhci::HidCompletion::Retire)
                {
                    retire_input(device, input, event.completion_code);
                    return;
                }
                if(action == xhci::HidCompletion::Report)
                {
                    dma_sync_for_cpu(input.report_buffer_dma);
                    if(HidBootProtocol::Keyboard == input.protocol &&
                       event.transfer_length <= input.endpoint_max_packet_size &&
                       input.endpoint_max_packet_size - event.transfer_length == 8)
                    {
                        handle_keyboard_report(device, input);
                    }
                    else if(HidBootProtocol::Mouse == input.protocol &&
                            event.transfer_length <= input.endpoint_max_packet_size &&
                            input.endpoint_max_packet_size - event.transfer_length >= 3)
                    {
                        handle_mouse_report(input);
                    }
                }
                if(!arm_interrupt_transfer(controller, device, input))
                    retire_input(device, input, event.completion_code);
                return;
            }
        }
    }
}

bool drain_controller_port_changes(XhciControllerState& controller, uint8_t port)
{
    return xhci::drain_port_changes(
        [&](xhci::PortStatus& status) {
            return xhci::read_port_status(controller.controller, port, status);
        },
        [&](uint32_t mask) {
            (void)xhci::clear_port_change_bits(controller.controller, port, mask);
        });
}

void service_events_locked(XhciControllerState& controller)
{
    if(__atomic_load_n(&controller.failed, __ATOMIC_ACQUIRE))
        return;
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
                if(controller.command_busy)
                    controller.command.record(event);
                break;
            case xhci::EventType::Transfer:
                process_transfer_event(controller, event);
                break;
            case xhci::EventType::PortStatusChange: {
                const auto port = event.port_id;
                if(drain_controller_port_changes(controller, port))
                {
                    controller.port_changed[port] = true;
                    for(auto& device : controller.devices)
                        if(device.present && device.port_id == port)
                            fail_device(device);
                }
                break;
            }
            default:
                break;
        }
    }

    if(consumed_event)
    {
        xhci::acknowledge_interrupt(controller.controller, controller.event_ring.dequeue_physical);
    }
}

void service_events(XhciControllerState& controller)
{
    IrqSpinGuard guard(g_event_lock);
    service_events_locked(controller);
}

xhci::PollDeadline operation_deadline(uint64_t duration,
                                      uint32_t stalled_polls = kXhciStalledClockPolls)
{
    return {tsc_clock_hz() ? tsc_clock_now_100ns : nullptr, duration, stalled_polls};
}

bool wait_for_command_completion(XhciControllerState& controller)
{
    auto deadline = operation_deadline(kXhciCommandTimeout100ns);
    for(;;)
    {
        {
            IrqSpinGuard guard(g_event_lock);
            service_events_locked(controller);
            if(__atomic_load_n(&controller.failed, __ATOMIC_ACQUIRE))
                return false;
            if(controller.command.complete)
                return true;
            if(deadline.expired())
                return false;
        }
        cpu_pause();
    }
}

bool recover_command_ring(XhciControllerState& controller)
{
    const auto& dma = controller.command_ring_dma;
    {
        IrqSpinGuard guard(g_event_lock);
        service_events_locked(controller);
        if(__atomic_load_n(&controller.failed, __ATOMIC_ACQUIRE))
            return false;
        if(controller.command.complete)
            return true;
        if(!xhci::request_command_abort(
               controller.controller, dma.physical_address, dma.size_bytes))
            return false;
    }

    auto deadline = operation_deadline(kXhciAbortTimeout100ns);
    for(;;)
    {
        {
            IrqSpinGuard guard(g_event_lock);
            service_events_locked(controller);
            if(__atomic_load_n(&controller.failed, __ATOMIC_ACQUIRE))
                return false;
            if(controller.command.stopped && !xhci::command_ring_running(controller.controller))
            {
                if(!controller.command.can_reprogram() ||
                   !xhci::initialize_command_ring(dma.virtual_address,
                                                  dma.size_bytes,
                                                  dma.physical_address,
                                                  controller.command_ring))
                    return false;
                dma_sync_for_device(dma);
                return xhci::program_command_ring(controller.controller,
                                                  dma.physical_address,
                                                  dma.size_bytes,
                                                  controller.command_ring.cycle_state);
            }
            if(deadline.expired())
                return false;
        }
        cpu_pause();
    }
}

bool submit_command(XhciControllerState& controller,
                    const xhci::TrbFields& trb,
                    uint8_t& completion_code,
                    uint8_t& slot_id)
{
    completion_code = 0u;
    slot_id = 0u;
    {
        IrqSpinGuard guard(g_event_lock);
        if(__atomic_load_n(&controller.failed, __ATOMIC_ACQUIRE) || controller.command_busy)
            return false;

        uint64_t trb_physical = 0;
        if(!xhci::enqueue_command_trb(controller.command_ring_dma.virtual_address,
                                      controller.command_ring_dma.size_bytes,
                                      controller.command_ring_dma.physical_address,
                                      controller.command_ring,
                                      trb,
                                      &trb_physical))
            return false;

        controller.command = {.expected_trb = trb_physical};
        controller.command_busy = true;
        dma_sync_for_device(controller.command_ring_dma);
        xhci::ring_doorbell(controller.controller, 0u, 0u);
    }

    const bool ring_usable =
        wait_for_command_completion(controller) || recover_command_ring(controller);
    bool completed = false;
    bool log_command = false;
    {
        IrqSpinGuard guard(g_event_lock);
        if(!ring_usable)
        {
            xhci::disable_interrupts(controller.controller);
            __atomic_store_n(&controller.failed, true, __ATOMIC_RELEASE);
            for(auto& device : controller.devices)
            {
                fail_device(device);
                hardware_catalog::unavailable(device.catalog_id);
            }
        }
        else if(controller.command.complete)
        {
            completed = true;
            completion_code = controller.command.code;
            slot_id = controller.command.slot;
        }
        else
        {
            // The stop event left the unconsumed command at the dequeue pointer.
            completion_code = xhci::kCompletionCommandAborted;
        }
        if(bootlog_diagnostics_enabled() && controller.diagnostic_commands < 32u)
        {
            ++controller.diagnostic_commands;
            log_command = true;
        }
        controller.command_busy = false;
    }
    if(log_command)
    {
        debug("usbdiag: command pci=")(controller.pci_index)(" type=")((trb.control >> 10) & 63u)(
            " completed=")(completed)(" cc=")(completion_code)();
    }
    return completed && kXhciCompletionSuccess == completion_code;
}

bool wait_for_transfer_completion(XhciControllerState& controller,
                                  XhciDeviceState& device,
                                  xhci::PollDeadline deadline,
                                  uint8_t& completion_code,
                                  uint32_t& residual_length)
{
    for(;;)
    {
        {
            IrqSpinGuard guard(g_event_lock);
            service_events_locked(controller);
            if(__atomic_load_n(&device.failed, __ATOMIC_ACQUIRE) ||
               __atomic_load_n(&controller.failed, __ATOMIC_ACQUIRE))
                return false;
            if(device.synchronous_transfer_complete)
            {
                completion_code = device.synchronous_transfer_completion_code;
                residual_length = device.synchronous_transfer_residual;
                device.synchronous_transfer_complete = false;
                return true;
            }
            if(deadline.expired())
            {
                debug("xhci: transfer timeout port=")(device.port_id)();
                fail_device(device);
                return false;
            }
        }
        cpu_pause();
    }
}

bool submit_control_transfer(XhciControllerState& controller,
                             XhciDeviceState& device,
                             const UsbSetupPacket& setup,
                             const void* out_buffer,
                             void* in_buffer,
                             size_t buffer_length,
                             size_t& actual_length,
                             bool* protocol_stall = nullptr)
{
    actual_length = 0;
    if(protocol_stall)
        *protocol_stall = false;
    if(buffer_length > device.control_buffer_dma.size_bytes || setup.length != buffer_length)
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
        else if((nullptr == out_buffer) ||
                (buffer_length > 0u && nullptr == device.control_buffer_dma.virtual_address))
        {
            return false;
        }
        else
        {
            memcpy(device.control_buffer_dma.virtual_address, out_buffer, buffer_length);
        }
        dma_sync_for_device(device.control_buffer_dma);
    }

    {
        IrqSpinGuard guard(g_event_lock);
        if(__atomic_load_n(&device.failed, __ATOMIC_ACQUIRE))
            return false;
        device.synchronous_endpoint = kXhciControlEndpointDci;
        device.synchronous_transfer_complete = false;
        device.synchronous_transfer_completion_code = 0u;
        device.synchronous_transfer_residual = 0u;
        device.synchronous_transfer_trb = 0u;

        const uint64_t setup_parameter = static_cast<uint64_t>(setup.request_type) |
                                         (static_cast<uint64_t>(setup.request) << 8) |
                                         (static_cast<uint64_t>(setup.value) << 16) |
                                         (static_cast<uint64_t>(setup.index) << 32) |
                                         (static_cast<uint64_t>(setup.length) << 48);
        if(!xhci::queue_control_transfer(device.control_ring_dma.virtual_address,
                                         device.control_ring_dma.size_bytes,
                                         device.control_ring_dma.physical_address,
                                         device.control_ring,
                                         setup_parameter,
                                         device.control_buffer_dma.physical_address,
                                         device.control_transfer))
            return false;
        dma_sync_for_device(device.control_ring_dma);
        xhci::publish_control_transfer(device.control_ring_dma.virtual_address,
                                       device.control_ring_dma.physical_address,
                                       device.control_transfer);
        xhci::ring_doorbell(controller.controller, device.slot_id, kXhciControlEndpointDci);
    }
    uint8_t completion_code = 0u;
    uint32_t residual_length = 0u;
    const bool completed =
        wait_for_transfer_completion(controller,
                                     device,
                                     operation_deadline(kXhciControlTimeout100ns),
                                     completion_code,
                                     residual_length);
    if(bootlog_diagnostics_enabled() && device.diagnostic_controls < 32u)
    {
        ++device.diagnostic_controls;
        debug("usbdiag: control pci=")(controller.pci_index)(" port=")(device.port_id)(" request=")(
            setup.request)(" value=0x")(setup.value, 16)(" index=")(setup.index)(" completed=")(
            completed)(" cc=")(completion_code)(" residual=")(residual_length)();
    }
    if(!completed || completion_code != kXhciCompletionSuccess || residual_length > buffer_length)
    {
        debug("xhci: control failed port=")(device.port_id)(" request=")(setup.request)(
            " value=0x")(setup.value, 16)(" requested=")(buffer_length)(" completed=")(
            completed)(" cc=")(completion_code)(" residual=")(residual_length)();
        if(protocol_stall && completed && completion_code == 6)
            *protocol_stall = true;
        else
            fail_device(device);
        return false;
    }

    if(has_data_stage && direction_in)
    {
        dma_sync_for_cpu(device.control_buffer_dma);
        actual_length = buffer_length - residual_length;
        if((nullptr != in_buffer) && (actual_length > 0u))
        {
            memcpy(in_buffer, device.control_buffer_dma.virtual_address, actual_length);
        }
    }
    return true;
}

bool usb_get_descriptor(XhciControllerState& controller,
                        XhciDeviceState& device,
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
    return submit_control_transfer(
        controller, device, setup, nullptr, buffer, buffer_length, actual_length);
}

bool usb_set_configuration(XhciControllerState& controller,
                           XhciDeviceState& device,
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
    return submit_control_transfer(controller, device, setup, nullptr, nullptr, 0u, actual_length);
}

bool usb_set_boot_protocol(XhciControllerState& controller,
                           XhciDeviceState& device,
                           uint8_t interface_number)
{
    const UsbSetupPacket setup{
        .request_type = kUsbRequestTypeClassOutInterface,
        .request = kUsbHidRequestSetProtocol,
        .value = 0u,
        .index = interface_number,
        .length = 0u,
    };
    size_t actual_length = 0u;
    return submit_control_transfer(controller, device, setup, nullptr, nullptr, 0u, actual_length);
}

bool reset_port(XhciControllerState& controller, uint8_t port_id, xhci::PortStatus& status)
{
    {
        IrqSpinGuard guard(g_event_lock);
        if(!xhci::read_port_status(controller.controller, port_id, status) || !status.connected ||
           controller.port_changed[port_id] || (status.raw & xhci::kPortScConnectStatusChange))
            return false;

        const size_t port_offset = xhci::kOperationalPortRegisterBase +
                                   (static_cast<size_t>(port_id) - 1u) * xhci::kPortRegisterStride +
                                   xhci::kPortSc;
        *reinterpret_cast<volatile uint32_t*>(controller.controller.operational + port_offset) =
            (status.raw & (xhci::kPortScPreservedBits | xhci::kPortScChangeBits)) |
            xhci::kPortScPortPower | xhci::kPortScPortReset;
    }

    auto deadline = operation_deadline(kXhciPortTimeout100ns);
    for(;;)
    {
        if(!xhci::read_port_status(controller.controller, port_id, status) || !status.connected)
        {
            return false;
        }
        if(status.enabled && !status.reset)
        {
            const auto speed = status.speed;
            tsc_clock_delay_us(10'000);
            IrqSpinGuard guard(g_event_lock);
            return xhci::read_port_status(controller.controller, port_id, status) &&
                   xhci::port_reset_recovered(status, speed, controller.port_changed[port_id]);
        }
        if(deadline.expired())
            return false;
        cpu_pause();
    }
}

bool address_device(XhciControllerState& controller, XhciDeviceState& device)
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
    command.control =
        (xhci::kTrbTypeAddressDeviceCommand << 10) | (static_cast<uint32_t>(device.slot_id) << 24);
    uint8_t completion_code = 0u;
    uint8_t slot_id = 0u;
    return submit_command(controller, command, completion_code, slot_id) &&
           (slot_id == device.slot_id);
}

bool update_control_packet(XhciControllerState& controller,
                           XhciDeviceState& device,
                           uint16_t packet)
{
    if(packet == device.control_max_packet_size)
        return true;
    dma_sync_for_cpu(device.device_context_dma);
    if(!xhci::prepare_control_packet_context(device.input_context_dma.virtual_address,
                                             device.input_context_dma.size_bytes,
                                             device.device_context_dma.virtual_address,
                                             device.device_context_dma.size_bytes,
                                             controller.controller.capability.context_size_bytes,
                                             packet))
        return false;
    dma_sync_for_device(device.input_context_dma);
    xhci::TrbFields command{};
    command.parameter = device.input_context_dma.physical_address;
    command.control =
        (xhci::kTrbTypeEvaluateContextCommand << 10) | (uint32_t(device.slot_id) << 24);
    uint8_t completion = 0, slot = 0;
    if(!submit_command(controller, command, completion, slot) || slot != device.slot_id)
        return false;
    debug("xhci: control packet port=")(device.port_id)(" old=")(device.control_max_packet_size)(
        " new=")(packet)();
    device.control_max_packet_size = packet;
    return true;
}

bool configure_interrupt_endpoint(XhciControllerState& controller, XhciDeviceState& device)
{
    memset(device.input_context_dma.virtual_address, 0, device.input_context_dma.size_bytes);
    auto* control = input_control_context(device);
    control[0] = 0u;
    control[1] = kInputContextAddSlot;
    uint8_t last = 1;
    for(unsigned i = 0; i < device.input_count; ++i)
    {
        auto& input = device.inputs[i];
        control[1] |= 1u << input.endpoint_dci;
        if(input.endpoint_dci > last)
            last = input.endpoint_dci;
        configure_endpoint_context(controller,
                                   device,
                                   input.endpoint_dci,
                                   7u,
                                   input.endpoint_max_packet_size,
                                   xhci::interrupt_interval(device.speed, input.endpoint_interval),
                                   input.interrupt_ring_dma,
                                   input.endpoint_max_packet_size);
    }
    configure_slot_context(controller, device, last);
    dma_sync_for_device(device.input_context_dma);

    xhci::TrbFields command{};
    command.parameter = device.input_context_dma.physical_address;
    command.control = (xhci::kTrbTypeConfigureEndpointCommand << 10) |
                      (static_cast<uint32_t>(device.slot_id) << 24);
    uint8_t completion_code = 0u;
    uint8_t slot_id = 0u;
    return submit_command(controller, command, completion_code, slot_id) &&
           (slot_id == device.slot_id);
}

SlotDisableResult disable_slot(XhciControllerState& controller, uint8_t slot)
{
    xhci::TrbFields trb{};
    trb.control = (xhci::kTrbTypeDisableSlotCommand << 10) | (uint32_t(slot) << 24);
    uint8_t completion = 0, returned_slot = 0;
    if(submit_command(controller, trb, completion, returned_slot))
        return SlotDisableResult::Disabled;
    return completion == xhci::kCompletionCommandAborted ? SlotDisableResult::Aborted
                                                         : SlotDisableResult::Failed;
}

bool control_without_data(
    XhciDeviceState& device, uint8_t type, uint8_t request, uint16_t index, bool* stall = nullptr)
{
    UsbSetupPacket setup{type, request, 0, index, 0};
    size_t actual = 0;
    return submit_control_transfer(
        *device.controller, device, setup, nullptr, nullptr, 0, actual, stall);
}

xhci::HidRestart recover_hid_endpoint(XhciControllerState& controller,
                                      XhciDeviceState& device,
                                      XhciInputState& input,
                                      uint8_t completion)
{
    auto endpoint_command = [&](uint8_t type, uint8_t dci, uint64_t parameter = 0) {
        xhci::TrbFields command{};
        command.parameter = parameter;
        command.control =
            (uint32_t(type) << 10) | (uint32_t(dci) << 16) | (uint32_t(device.slot_id) << 24);
        uint8_t code = 0, slot = 0;
        (void)submit_command(controller, command, code, slot);
        return slot == device.slot_id ? code : uint8_t(0);
    };
    return xhci::restart_hid_transfer(
        completion,
        input.endpoint_dci,
        [&](uint8_t dci) { return endpoint_command(xhci::kTrbTypeResetEndpointCommand, dci); },
        [&] {
            bool stall = false;
            if(control_without_data(device, 2, 1, input.endpoint_address, &stall))
                return xhci::HidClearHalt::Cleared;
            return stall ? xhci::HidClearHalt::ControlStall : xhci::HidClearHalt::Failed;
        },
        [&](uint8_t dci) {
            const bool control = dci == kXhciControlEndpointDci;
            const auto& ring = control ? device.control_ring : input.interrupt_ring;
            const auto& dma = control ? device.control_ring_dma : input.interrupt_ring_dma;
            const auto dequeue = (dma.physical_address + uint64_t(ring.enqueue_index) * 16) |
                                 uint64_t(ring.cycle_state);
            return endpoint_command(xhci::kTrbTypeSetTrDequeueCommand, dci, dequeue) ==
                   kXhciCompletionSuccess;
        },
        [&](uint8_t dci) {
            dma_sync_for_cpu(device.device_context_dma);
            const size_t offset = size_t(controller.controller.capability.context_size_bytes) * dci;
            const auto* context =
                static_cast<const uint8_t*>(device.device_context_dma.virtual_address);
            return (*reinterpret_cast<const volatile uint32_t*>(context + offset) & 7u) == 1;
        });
}

void recover_hid_inputs(XhciControllerState& controller)
{
    for(auto& device : controller.devices)
        for(auto& input : device.inputs)
        {
            uint8_t completion = 0;
            {
                IrqSpinGuard guard(g_event_lock);
                if(!device.present || !device.configured ||
                   __atomic_load_n(&device.failed, __ATOMIC_ACQUIRE) ||
                   controller.port_changed[device.port_id])
                    continue;
                completion = input.recovery.pending;
                input.recovery.pending = 0;
            }
            if(!completion)
                continue;
            debug("xhci: input recovery port=")(device.port_id)(" dci=")(input.endpoint_dci)(
                " cc=")(completion)();
            const auto recovery = recover_hid_endpoint(controller, device, input, completion);
            IrqSpinGuard guard(g_event_lock);
            if(recovery == xhci::HidRestart::RetireDevice)
            {
                fail_device(device);
                hardware_catalog::unavailable(device.catalog_id);
                continue;
            }
            if(__atomic_load_n(&device.failed, __ATOMIC_ACQUIRE) ||
               controller.port_changed[device.port_id])
                continue;
            if(recovery != xhci::HidRestart::Recovered ||
               !arm_interrupt_transfer(controller, device, input))
                retire_input(device, input, completion);
        }
}

bool storage_clear_halt(void* context, bool in)
{
    auto& device = *static_cast<XhciDeviceState*>(context);
    auto& controller = *device.controller;
    uint8_t address = in ? device.storage_interface.in : device.storage_interface.out;
    uint8_t endpoint = endpoint_dci_from_address(address);
    if(!control_without_data(device, 2, 1, address))
        return false;
    xhci::TrbFields command{};
    command.control = (xhci::kTrbTypeResetEndpointCommand << 10) | (uint32_t(endpoint) << 16) |
                      (uint32_t(device.slot_id) << 24);
    uint8_t completion = 0, slot = 0;
    if(!submit_command(controller, command, completion, slot))
        return false;
    auto& ring = in ? device.bulk_in_ring : device.bulk_out_ring;
    auto& dma = in ? device.bulk_in_ring_dma : device.bulk_out_ring_dma;
    command.parameter =
        (dma.physical_address + uint64_t(ring.enqueue_index) * 16) | (ring.cycle_state ? 1 : 0);
    command.control = (xhci::kTrbTypeSetTrDequeueCommand << 10) | (uint32_t(endpoint) << 16) |
                      (uint32_t(device.slot_id) << 24);
    return submit_command(controller, command, completion, slot);
}

bool storage_reset(void* context)
{
    auto& device = *static_cast<XhciDeviceState*>(context);
    // BOT reset recovery in wire order. This path retires the device, not its buffers.
    bool reset = control_without_data(device, 0x21, 0xff, device.storage_interface.number);
    bool in = control_without_data(device, 2, 1, device.storage_interface.in);
    bool out = control_without_data(device, 2, 1, device.storage_interface.out);
    return reset && in && out;
}

usb::storage::Transfer storage_bulk(void* context, bool in, void* data, uint32_t bytes)
{
    using namespace usb::storage;
    auto& device = *static_cast<XhciDeviceState*>(context);
    auto& controller = *device.controller;
    if(!data || !bytes || bytes > 4096 || !device.bulk_buffer_dma.active)
        return {TransferStatus::Failed, 0};
    uint8_t endpoint =
        endpoint_dci_from_address(in ? device.storage_interface.in : device.storage_interface.out);
    {
        IrqSpinGuard guard(g_event_lock);
        service_events_locked(controller);
        xhci::PortStatus port{};
        if(__atomic_load_n(&device.failed, __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&controller.failed, __ATOMIC_ACQUIRE) ||
           !xhci::read_port_status(controller.controller, device.port_id, port) || !port.connected)
        {
            fail_device(device);
            return {TransferStatus::Failed, 0};
        }
        if(!in)
            memcpy(device.bulk_buffer_dma.virtual_address, data, bytes);
        dma_sync_for_device(device.bulk_buffer_dma);
        auto& dma = in ? device.bulk_in_ring_dma : device.bulk_out_ring_dma;
        auto& ring = in ? device.bulk_in_ring : device.bulk_out_ring;
        device.synchronous_transfer_complete = false;
        device.synchronous_endpoint = endpoint;
        xhci::TrbFields trb{
            device.bulk_buffer_dma.physical_address,
            bytes,
            (xhci::kTrbTypeNormal << 10) | kTrbInterruptOnShortPacket | kTrbInterruptOnCompletion};
        if(!xhci::enqueue_command_trb(dma.virtual_address,
                                      dma.size_bytes,
                                      dma.physical_address,
                                      ring,
                                      trb,
                                      &device.synchronous_transfer_trb))
        {
            fail_device(device);
            return {TransferStatus::Failed, 0};
        }
        dma_sync_for_device(dma);
        xhci::ring_doorbell(controller.controller, device.slot_id, endpoint);
    }
    uint8_t completion = 0;
    uint32_t residual = 0;
    if(!wait_for_transfer_completion(
           controller,
           device,
           operation_deadline(kXhciBulkTimeout100ns, kXhciBulkStalledClockPolls),
           completion,
           residual))
        return {TransferStatus::Failed, 0};
    if(completion == 6)
        return {TransferStatus::Stall, 0};
    if(!transfer_completion_ok(completion) || residual > bytes)
    {
        fail_device(device);
        return {TransferStatus::Failed, 0};
    }
    uint32_t actual = bytes - residual;
    if(in)
    {
        dma_sync_for_cpu(device.bulk_buffer_dma);
        memcpy(data, device.bulk_buffer_dma.virtual_address, actual);
    }
    return {TransferStatus::Ok, actual};
}

bool storage_submit(BlockDevice& block, BlockRequest& request)
{
    auto& device = *static_cast<XhciDeviceState*>(block.driver_state);
    auto& controller = *device.controller;
    bool flush = request.operation == BlockOperation::Flush;
    if((!flush && (request.operation != BlockOperation::Read &&
                   request.operation != BlockOperation::Write)) ||
       (!flush && (!request.buffer || !request.sector_count || request.sector_count > 8 ||
                   request.sector >= block.sector_count ||
                   request.sector_count > block.sector_count - request.sector)))
    {
        block_request_complete(request, BlockRequestStatus::Invalid);
        return false;
    }
    SchedulingGuard scheduling;
    if(__atomic_exchange_n(&controller.storage_busy, 1, __ATOMIC_ACQUIRE))
    {
        request.status = BlockRequestStatus::Busy;
        return false;
    }
    bool ok = flush ? device.storage.flush()
                    : device.storage.read_write(request.operation == BlockOperation::Write,
                                                request.sector,
                                                request.buffer,
                                                request.sector_count);
    if(!ok)
        fail_device(device);
    __atomic_store_n(&controller.storage_busy, 0, __ATOMIC_RELEASE);
    block_request_complete(request,
                           ok ? BlockRequestStatus::Success : BlockRequestStatus::DeviceError,
                           ok && !flush ? request.sector_count * 512 : 0);
    return ok;
}

bool initialize_storage(PageFrameContainer& frames,
                        XhciControllerState& controller,
                        XhciDeviceState& device,
                        const usb::storage::Interface& interface)
{
    // First physical profile: USB 2 high-speed and USB 3 SuperSpeed, no streams/hubs.
    if((device.speed != 3 && device.speed != 4) ||
       interface.in_packet != (device.speed == 3 ? 512 : 1024) ||
       interface.out_packet != interface.in_packet ||
       (device.speed == 4 && (!interface.in_companion || !interface.out_companion)))
        return false;
    device.storage_interface = interface;
    if(!allocate_controller_dma(frames, controller, 4096, device.bulk_in_ring_dma) ||
       !allocate_controller_dma(frames, controller, 4096, device.bulk_out_ring_dma) ||
       !allocate_controller_dma(frames, controller, 4096, device.bulk_buffer_dma))
        return false;
    if(!xhci::initialize_command_ring(device.bulk_in_ring_dma.virtual_address,
                                      4096,
                                      device.bulk_in_ring_dma.physical_address,
                                      device.bulk_in_ring) ||
       !xhci::initialize_command_ring(device.bulk_out_ring_dma.virtual_address,
                                      4096,
                                      device.bulk_out_ring_dma.physical_address,
                                      device.bulk_out_ring))
        return false;
    if(!usb_set_configuration(controller, device, interface.configuration))
        return false;
    uint8_t in = endpoint_dci_from_address(interface.in),
            out = endpoint_dci_from_address(interface.out);
    memset(device.input_context_dma.virtual_address, 0, device.input_context_dma.size_bytes);
    input_control_context(device)[1] = 1u | (1u << in) | (1u << out);
    configure_slot_context(controller, device, in > out ? in : out);
    configure_endpoint_context(
        controller, device, in, 6, interface.in_packet, 0, device.bulk_in_ring_dma, 4096);
    configure_endpoint_context(
        controller, device, out, 2, interface.out_packet, 0, device.bulk_out_ring_dma, 4096);
    input_endpoint_context(controller, device, in)[1] |= uint32_t(interface.in_burst) << 8;
    input_endpoint_context(controller, device, out)[1] |= uint32_t(interface.out_burst) << 8;
    dma_sync_for_device(device.input_context_dma);
    xhci::TrbFields command{};
    command.parameter = device.input_context_dma.physical_address;
    command.control =
        (xhci::kTrbTypeConfigureEndpointCommand << 10) | (uint32_t(device.slot_id) << 24);
    uint8_t completion = 0, slot = 0;
    if(!submit_command(controller, command, completion, slot))
        return false;
    if(!device.storage.initialize({&device, storage_bulk, storage_clear_halt, storage_reset}))
        return false;
    device.block = {
        "usb-bot", device.storage.sectors, 512, 8, 1, &device, storage_submit, storage_submit};
    device.storage_published = true;
    device.configured = true;
    debug("usb-storage: ready port=")(device.port_id)(" sectors=")(device.storage.sectors)(
        " flush=1")();
    return true;
}

bool initialize_port(PageFrameContainer& frames,
                     XhciControllerState& controller,
                     uint8_t port_id,
                     unsigned catalog_port)
{
    xhci::PortStatus status{};
    {
        IrqSpinGuard guard(g_event_lock);
        if(!xhci::read_port_status(controller.controller, port_id, status) || !status.connected)
            return false;

        // Consume the attachment being enumerated, even if hardware enabled the port.
        controller.port_changed[port_id] = false;
        if(status.has_changes)
        {
            (void)xhci::clear_port_change_bits(controller.controller, port_id, status.raw);
            if(drain_controller_port_changes(controller, port_id))
            {
                controller.port_changed[port_id] = true;
                return false;
            }
        }
    }

    // Retain outcomes in normal boots as well as diagnostic boots.
    struct Observation
    {
        Os1ObserveUsbRecord record{};
        const char* stage = "enumeration-failed";
        ~Observation()
        {
            copy_fixed_string(record.stage, sizeof(record.stage), stage);
            usb::diagnostics::append(record);
        }
    } observation;
    observation.record.kind = OS1_OBSERVE_USB_DEVICE;
    observation.record.pci_index = controller.pci_index;
    observation.record.root_port = observation.record.port = port_id;
    observation.record.status = status.raw;
    const unsigned speeds[] = {0, 120, 15, 4800, 50000};
    unsigned catalog_device = hardware_catalog::usb_device(
        catalog_port, port_id, port_id, status.speed < 5 ? speeds[status.speed] : 0);

    if(!status.powered)
    {
        (void)xhci::power_on_port(controller.controller, port_id);
    }
    if(!status.enabled && !reset_port(controller, port_id, status))
    {
        debug("xhci: port reset failed port=")(port_id)();
        return false;
    }
    XhciDeviceState* device = allocate_device(controller);
    if(nullptr == device)
    {
        debug("xhci: HID device table full")();
        return false;
    }

    uint8_t completion_code = 0u;
    uint8_t slot_id = 0u;
    xhci::TrbFields enable_slot{};
    enable_slot.control = xhci::kTrbTypeEnableSlotCommand << 10;
    if(!submit_command(controller, enable_slot, completion_code, slot_id) || (0u == slot_id))
    {
        debug("xhci: enable slot failed port=")(port_id)(" cc=")(completion_code)();
        return false;
    }

    *device = {};
    device->catalog_id = catalog_device;
    device->present = true;
    device->controller = &controller;
    device->slot_id = slot_id;
    device->port_id = port_id;
    device->speed = status.speed;
    device->control_max_packet_size = control_max_packet_from_speed(status.speed);
    if(!allocate_device_resources(frames, controller, *device))
    {
        debug("xhci: HID device allocation failed port=")(port_id)();
        release_device(frames, controller, *device);
        return false;
    }
    device->dma_visible = true;
    set_dcbaa_entry(controller, device->slot_id, device->device_context_dma.physical_address);

    if(!address_device(controller, *device))
    {
        debug("xhci: address device failed port=")(port_id)();
        release_device(frames, controller, *device);
        return false;
    }

    observation.stage = "device-descriptor-failed";
    UsbDeviceDescriptor descriptor{};
    size_t actual_length = 0u;
    uint16_t packet = 0;
    if(!usb_get_descriptor(
           controller, *device, kUsbDescriptorDevice, 0, &descriptor, 8, actual_length) ||
       !xhci::device_control_packet(device->speed, &descriptor, actual_length, packet) ||
       !update_control_packet(controller, *device, packet))
    {
        debug("xhci: control packet negotiation failed port=")(port_id)();
        release_device(frames, controller, *device);
        return false;
    }
    if(!usb_get_descriptor(controller,
                           *device,
                           kUsbDescriptorDevice,
                           0u,
                           &descriptor,
                           sizeof(descriptor),
                           actual_length) ||
       !xhci::valid_descriptor_header(
           &descriptor, actual_length, kUsbDescriptorDevice, sizeof(descriptor)))
    {
        debug("xhci: device descriptor failed port=")(port_id)();
        release_device(frames, controller, *device);
        return false;
    }

    if(bootlog_diagnostics_enabled())
        debug("usbdiag: device pci=")(controller.pci_index)(" port=")(port_id)(" id=")(
            descriptor.vendor_id, 16, 4)(":")(
            descriptor.product_id, 16, 4)(" speed=")(device->speed)();
    hardware_catalog::usb_descriptor(catalog_device, &descriptor, sizeof(descriptor));
    observation.record.detail = uint32_t(descriptor.vendor_id) << 16 | descriptor.product_id;
    observation.stage = "configuration-failed";
    uint8_t config_header[sizeof(UsbConfigurationDescriptor)]{};
    if(!usb_get_descriptor(controller,
                           *device,
                           kUsbDescriptorConfiguration,
                           0u,
                           config_header,
                           sizeof(config_header),
                           actual_length) ||
       !xhci::valid_descriptor_header(config_header,
                                      actual_length,
                                      kUsbDescriptorConfiguration,
                                      sizeof(UsbConfigurationDescriptor)))
    {
        debug("xhci: config header failed port=")(port_id)();
        release_device(frames, controller, *device);
        return false;
    }

    const auto* configuration = reinterpret_cast<const UsbConfigurationDescriptor*>(config_header);
    if((sizeof(UsbConfigurationDescriptor) > configuration->total_length) ||
       (configuration->total_length > device->control_buffer_dma.size_bytes))
    {
        debug("xhci: config length unsupported port=")(port_id)(" total=")(
            configuration->total_length)();
        release_device(frames, controller, *device);
        return false;
    }

    if(!usb_get_descriptor(controller,
                           *device,
                           kUsbDescriptorConfiguration,
                           0u,
                           device->control_buffer_dma.virtual_address,
                           configuration->total_length,
                           actual_length))
    {
        release_device(frames, controller, *device);
        return false;
    }
    bool descriptor_valid = xhci::valid_configuration_descriptors(
        device->control_buffer_dma.virtual_address, actual_length);
    hardware_catalog::usb_configuration(catalog_device,
                                        device->control_buffer_dma.virtual_address,
                                        actual_length,
                                        descriptor_valid);
    if(!descriptor_valid)
    {
        release_device(frames, controller, *device);
        return false;
    }
    usb::storage::Interface storage{};
    if(usb::storage::parse_configuration(
           device->control_buffer_dma.virtual_address, actual_length, storage))
    {
        if(initialize_storage(frames, controller, *device, storage))
        {
            auto interface_id = hardware_catalog::usb_bind(
                catalog_device, storage.configuration, storage.number, "usb-bot", true);
            hardware_catalog::block(device->block, interface_id);
            observation.stage = "storage-ready";
            observation.record.sectors = device->storage.sectors;
            return true;
        }
        hardware_catalog::usb_bind(
            catalog_device, storage.configuration, storage.number, "usb-bot", false);
        observation.stage = "storage-failed";
        debug("usb-storage: unsupported or failed device port=")(port_id)();
        release_device(frames, controller, *device);
        return false;
    }
    observation.stage = "unsupported-interface";
    os1::input::HidBoot selections[kXhciInputInterfaces]{};
    for(unsigned i = 0; i < kXhciInputInterfaces; ++i)
    {
        auto& selected = selections[i];
        if(!os1::input::hid_boot(
               static_cast<const uint8_t*>(device->control_buffer_dma.virtual_address),
               actual_length,
               selected,
               i,
               device->speed == 2 ? 8 : 64))
            break;
        auto& input = device->inputs[i];
        input.protocol =
            selected.protocol == 1 ? HidBootProtocol::Keyboard : HidBootProtocol::Mouse;
        input.interface_number = selected.interface;
        input.endpoint_address = selected.endpoint;
        input.endpoint_dci = endpoint_dci_from_address(selected.endpoint);
        input.endpoint_interval = selected.interval;
        input.endpoint_max_packet_size = selected.packet;
        for(unsigned prior = 0; prior < i; ++prior)
            if(device->inputs[prior].endpoint_dci == input.endpoint_dci ||
               device->inputs[prior].interface_number == input.interface_number)
            {
                release_device(frames, controller, *device);
                return false;
            }
        if(!allocate_controller_dma(
               frames, controller, kXhciTransferRingBytes, input.interrupt_ring_dma) ||
           !allocate_controller_dma(frames, controller, selected.packet, input.report_buffer_dma) ||
           !xhci::initialize_command_ring(input.interrupt_ring_dma.virtual_address,
                                          input.interrupt_ring_dma.size_bytes,
                                          input.interrupt_ring_dma.physical_address,
                                          input.interrupt_ring))
        {
            release_device(frames, controller, *device);
            return false;
        }
        ++device->input_count;
        device->configuration_value = selected.configuration;
    }
    if(!device->input_count)
    {
        debug("xhci: no supported interface port=")(port_id)();
        release_device(frames, controller, *device);
        return false;
    }
    observation.stage = "hid-configuration-failed";
    if(!usb_set_configuration(controller, *device, device->configuration_value) ||
       !configure_interrupt_endpoint(controller, *device))
    {
        release_device(frames, controller, *device);
        return false;
    }
    for(unsigned i = 0; i < device->input_count; ++i)
    {
        auto& input = device->inputs[i];
        input.catalog_id = hardware_catalog::usb_bind(
            catalog_device, device->configuration_value, input.interface_number, "usb-hid", false);
        if(!usb_set_boot_protocol(controller, *device, input.interface_number))
        {
            release_device(frames, controller, *device);
            return false;
        }
        input.input_identity = native_input::attach(
            input.protocol == HidBootProtocol::Keyboard ? OS1_INPUT_KEYBOARD : OS1_INPUT_POINTER,
            input.protocol == HidBootProtocol::Keyboard ? "USB keyboard" : "USB pointer",
            catalog_device);
        if(!input.input_identity)
        {
            observation.stage = "input-capacity";
            release_device(frames, controller, *device);
            return false;
        }
        // Legacy device-level identity names the first function. Each native object
        // retains its own identity and the shared physical catalog association.
        if(i == 0)
            hardware_catalog::usb_input(catalog_device, input.input_identity);
    }
    device->configured = true;
    {
        IrqSpinGuard guard(g_event_lock);
        for(unsigned i = 0; i < device->input_count; ++i)
            if(!arm_interrupt_transfer(controller, *device, device->inputs[i]))
            {
                fail_device(*device);
                return false;
            }
        for(unsigned i = 0; i < device->input_count; ++i)
            hardware_catalog::usb_bind(catalog_device,
                                       device->configuration_value,
                                       device->inputs[i].interface_number,
                                       "usb-hid",
                                       true);
    }
    for(unsigned i = 0; i < device->input_count; ++i)
    {
        auto& input = device->inputs[i];
        debug("xhci: HID ")((HidBootProtocol::Keyboard == input.protocol) ? "keyboard" : "mouse")(
            " ready port=")(port_id)(" slot=")(device->slot_id)(" ep=0x")(
            input.endpoint_address, 16, 2)(" mps=")(input.endpoint_max_packet_size)();
    }

    observation.stage = "hid-ready";
    return true;
}

void enumerate_ports(PageFrameContainer& frames, XhciControllerState& controller)
{
    for(unsigned port_id = 1u; port_id <= controller.controller.capability.max_ports; ++port_id)
    {
        xhci::PortStatus status{};
        if(!xhci::read_port_status(controller.controller, port_id, status))
            continue;
        Os1ObserveUsbRecord port_record{};
        port_record.kind = OS1_OBSERVE_USB_ROOT_PORT;
        port_record.pci_index = controller.pci_index;
        port_record.root_port = port_id;
        port_record.status = status.raw;
        copy_fixed_string(port_record.stage,
                          sizeof(port_record.stage),
                          status.connected ? "connected" : "disconnected");
        usb::diagnostics::append(port_record);
        unsigned catalog_port =
            hardware_catalog::usb_port(controller.pci_index, port_id, status.connected);
        controller.catalog_ports[port_id] = catalog_port;
        if(status.connected)
        {
            if(bootlog_diagnostics_enabled())
                debug("usbdiag: enumerate pci=")(controller.pci_index)(" port=")(
                    port_id)(" raw=0x")(status.raw, 16)(" speed=")(status.speed)();
            const bool initialized = initialize_port(frames, controller, port_id, catalog_port);
            if(bootlog_diagnostics_enabled())
                debug("usbdiag: enumeration pci=")(controller.pci_index)(" port=")(port_id)(" ok=")(
                    initialized)();
        }
    }
}

void xhci_irq(void* data)
{
    auto& controller = *static_cast<XhciControllerState*>(data);
    ++controller.interrupt_count;
    service_events(controller);
}

void handoff_delay(uint32_t us, void*)
{
    tsc_clock_delay_us(us);
}
uint32_t routing_read(void* context, uint16_t offset)
{
    return pci_config_read32(*static_cast<const PciDevice*>(context), offset);
}
void routing_write(void* context, uint16_t offset, uint32_t value)
{
    pci_config_write32(*static_cast<const PciDevice*>(context), offset, value);
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
    debug("xhci: probe pci=")(device.bus, 16, 2)(":")(device.slot, 16, 2)(".")(
        device.function, 16, 1)(" id=")(device.vendor_id, 16, 4)(":")(
        device.device_id, 16, 4)(" cmd=0x")(pci_read_command(device), 16, 4)();

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
    const uint32_t capability_word =
        *reinterpret_cast<volatile uint32_t*>(controller->controller.mmio_base);
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

    if(bootlog_diagnostics_enabled())
    {
        // Observe firmware state before reset; never change ownership or routing here.
        xhci::LegacySupport legacy{};
        const auto result = xhci::read_legacy_support(controller->controller, legacy);
        debug("usbdiag: legacy pci=")(device_index)(" result=")(static_cast<uint8_t>(result))(
            " offset=0x")(legacy.offset, 16)(" support=0x")(legacy.support, 16)(" control=0x")(
            legacy.control, 16)();
        // These chipset-specific offsets are qualified only for the issue #27 controller.
        if(device.vendor_id == 0x8086u && device.device_id == 0x8c31u)
            debug("usbdiag: Intel 8c31 USB2 route=0x")(pci_config_read32(device, 0xd0), 16)(
                " mask=0x")(pci_config_read32(device, 0xd4), 16)(" USB3 enable=0x")(
                pci_config_read32(device, 0xd8), 16)(" mask=0x")(pci_config_read32(device, 0xdc),
                                                                 16)();
        for(unsigned port = 1; port <= controller->controller.capability.max_ports; ++port)
        {
            xhci::PortStatus status{};
            if(xhci::read_port_status(controller->controller, port, status))
                debug("usbdiag: before-reset pci=")(device_index)(" port=")(port)(" raw=0x")(
                    status.raw, 16)(" speed=")(status.speed)();
        }
    }

    if(!tsc_clock_calibrate())
    {
        debug("xhci: calibrated reset timing unavailable")();
        release_controller(frames, *controller);
        return false;
    }
    if(!xhci::acquire_legacy_ownership(controller->controller, handoff_delay, nullptr))
    {
        debug("xhci: firmware handoff failed")();
        release_controller(frames, *controller);
        return false;
    }
    controller->owned = true;
    debug("xhci: firmware handoff complete")();
    if(!xhci::controller_reset(controller->controller,
                               kXhciInitTimeoutSpins,
                               nullptr,
                               nullptr,
                               device.vendor_id == 0x8086 ? handoff_delay : nullptr))
    {
        debug("xhci: controller reset failed")();
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
    if(!pci_enable_best_interrupt(
           kernel_vm, id, device, 0, xhci_irq, controller, controller->interrupt))
    {
        debug("xhci: interrupt bind failed")();
        release_controller(frames, *controller);
        return false;
    }
    debug("xhci: interrupt bound vector=0x")(controller->interrupt.vector, 16, 2)();

    pci_enable_mmio_bus_mastering(device);

    const xhci::RingSet rings{
        .dcbaa_physical = controller->dcbaa_dma.physical_address,
        .command_ring_physical = controller->command_ring_dma.physical_address,
        .command_ring_bytes = controller->command_ring_dma.size_bytes,
        .event_ring_physical = controller->event_ring_dma.physical_address,
        .erst_physical = controller->erst_dma.physical_address,
        .command_ring_cycle_state = controller->command_ring.cycle_state,
        .event_ring_trb_count =
            static_cast<uint32_t>(controller->event_ring_dma.size_bytes / xhci::kTrbBytes),
    };
    controller->dma_published = true;
    if(!xhci::controller_program(controller->controller, rings, controller->configured_slots) ||
       !xhci::controller_run(controller->controller, kXhciInitTimeoutSpins))
    {
        debug("xhci: controller start failed")();
        release_controller(frames, *controller);
        return false;
    }

    const xhci::RoutingIo routing_io{const_cast<PciDevice*>(&device), routing_read, routing_write};
    const auto routing = xhci::route_intel_ports(device.vendor_id, device.device_id, routing_io);
    if(routing != xhci::RoutingResult::NotApplicable)
        debug("xhci: Intel routing result=")(static_cast<uint8_t>(routing))(" USB2=0x")(
            pci_config_read32(device, 0xd0), 16)(" USB3=0x")(pci_config_read32(device, 0xd8), 16)();
    if(routing == xhci::RoutingResult::Failed)
    {
        release_controller(frames, *controller);
        return false;
    }
    if(routing == xhci::RoutingResult::Changed)
        tsc_clock_delay_us(100000u);
    enumerate_ports(frames, *controller);

    controller->present = true;
    (void)device_binding_publish(id, static_cast<uint16_t>(device_index), "xhci", controller);
    (void)device_binding_set_state(id, DeviceState::Started);

    debug("xhci: ready pci=")(device.bus, 16, 2)(":")(device.slot, 16, 2)(".")(
        device.function, 16, 1)(" ports=")(controller->controller.capability.max_ports)(" slots=")(
        controller->configured_slots)(" irq=")(
        controller->interrupt.vector, 16, 2)(" scratch=")(controller->scratchpad_count)();
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
        .remove =
            [](DeviceId id) {
                if(XhciControllerState* controller = find_controller(id))
                {
                    release_controller(page_frames, *controller);
                    device_binding_remove(id);
                }
            },
        .probe_early = true,
    };
    return driver;
}
BlockDevice* xhci_storage_block_device(size_t index)
{
    for(auto* controller : g_xhci_controllers)
    {
        if(!controller)
            continue;
        for(auto& device : controller->devices)
            if(device.storage_published)
            {
                if(!index)
                    return &device.block;
                --index;
            }
    }
    return nullptr;
}

void xhci_capture_catalog_state()
{
    // Boot thread only. IRQ handlers publish failure flags, never catalog records.
    IrqSpinGuard guard(g_event_lock);
    for(auto* controller : g_xhci_controllers)
    {
        if(!controller)
            continue;
        for(auto& device : controller->devices)
            if((device.configured || device.storage_published) &&
               (__atomic_load_n(&device.failed, __ATOMIC_ACQUIRE) ||
                __atomic_load_n(&controller->failed, __ATOMIC_ACQUIRE)))
                hardware_catalog::unavailable(device.catalog_id);
    }
}

void xhci_poll_input()
{
    char pending[64];
    size_t count = 0;
    {
        IrqSpinGuard guard(g_event_lock);
        while(g_input_count)
        {
            pending[count++] = g_input_chars[g_input_head];
            g_input_head = (g_input_head + 1) % sizeof(g_input_chars);
            --g_input_count;
        }
    }
    static unsigned diagnostic_batches = 0;
    if(bootlog_diagnostics_enabled() && count && diagnostic_batches < 8u)
    {
        ++diagnostic_batches;
        debug("usbdiag: console dispatch chars=")(count)();
    }
    // No controller lock may be held across console callbacks.
    for(size_t index = 0; index < count; ++index)
        console_input_on_keyboard_char(pending[index]);
}

void xhci_diagnostic_summary()
{
    if(!bootlog_diagnostics_enabled())
        return;
    IrqSpinGuard guard(g_event_lock);
    debug("USB diagnostic summary (details retained in boot log)")();
    unsigned ready = 0;
    for(const auto* controller : g_xhci_controllers)
    {
        if(!controller || !controller->present)
            continue;
        debug("usbdiag: controller pci=")(controller->pci_index)(" failed=")(controller->failed)(
            " irq=")(controller->interrupt_count)();
        for(const auto& device : controller->devices)
        {
            if(!device.present)
                continue;
            for(const auto& input : device.inputs)
            {
                const bool keyboard = input.protocol == HidBootProtocol::Keyboard;
                debug("usbdiag: port=")(device.port_id)(" speed=")(device.speed)(" keyboard=")(
                    keyboard)(" configured=")(device.configured)(" failed=")(device.failed)(
                    " armed=")(input.interrupt_transfer_armed)();
                if(keyboard && device.configured && !device.failed)
                    ++ready;
            }
        }
    }
    debug("usbdiag: configured keyboards=")(ready)();
}

void xhci_poll_devices(PageFrameContainer& frames)
{
    for(auto* c : g_xhci_controllers)
    {
        SchedulingGuard scheduling;
        if(!c || __atomic_load_n(&c->failed, __ATOMIC_ACQUIRE) ||
           __atomic_exchange_n(&c->storage_busy, 1, __ATOMIC_ACQUIRE))
            continue;
        for(unsigned port = 1; port <= c->controller.capability.max_ports; ++port)
        {
            bool changed = false;
            {
                IrqSpinGuard guard(g_event_lock);
                changed = c->port_changed[port];
                c->port_changed[port] = false;
            }
            if(!changed)
                continue;
            auto disabled = SlotDisableResult::Disabled;
            for(auto& d : c->devices)
            {
                uint8_t slot = 0;
                {
                    IrqSpinGuard guard(g_event_lock);
                    if(d.present && d.port_id == port)
                    {
                        fail_device(d);
                        slot = d.slot_id;
                    }
                }
                if(!slot)
                    continue;
                hardware_catalog::unavailable(d.catalog_id);
                disabled = disable_slot(*c, slot);
                if(disabled != SlotDisableResult::Disabled)
                    break;
                {
                    IrqSpinGuard guard(g_event_lock);
                    d.slot_id = 0;  // A later slot reuse cannot match this retired attachment.
                }
                // Keep every published DMA buffer and block address stable until reboot.
            }
            if(disabled == SlotDisableResult::Failed)
            {
                release_controller(frames, *c);
                break;
            }
            if(disabled == SlotDisableResult::Aborted)
                debug("xhci: slot disable aborted; device quarantined port=")(port)();
            else
                tsc_clock_delay_us(100000);
            xhci::PortStatus status{};
            if(!xhci::read_port_status(c->controller, port, status))
                continue;
            hardware_catalog::port_state(c->catalog_ports[port], status.connected);
            if(status.connected && disabled == SlotDisableResult::Disabled)
                (void)initialize_port(frames, *c, uint8_t(port), c->catalog_ports[port]);
        }
        if(!__atomic_load_n(&c->failed, __ATOMIC_ACQUIRE))
            recover_hid_inputs(*c);
        __atomic_store_n(&c->storage_busy, 0, __ATOMIC_RELEASE);
    }
}
