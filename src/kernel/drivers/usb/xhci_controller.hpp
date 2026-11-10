// xHCI capability decoding and controller MMIO programming helpers.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace xhci
{
constexpr size_t kTrbBytes = 16u;
constexpr size_t kDoorbellBytes = 4u;
constexpr uint32_t kTrbTypeLink = 6u;
constexpr uint32_t kTrbTypeNormal = 1u;
constexpr uint32_t kTrbTypeSetupStage = 2u;
constexpr uint32_t kTrbTypeDataStage = 3u;
constexpr uint32_t kTrbTypeStatusStage = 4u;
constexpr uint32_t kTrbTypeEnableSlotCommand = 9u;
constexpr uint32_t kTrbTypeDisableSlotCommand = 10u;
constexpr uint32_t kTrbTypeAddressDeviceCommand = 11u;
constexpr uint32_t kTrbTypeResetEndpointCommand = 14u;
constexpr uint32_t kTrbTypeSetTrDequeueCommand = 16u;
constexpr uint32_t kTrbTypeConfigureEndpointCommand = 12u;
constexpr uint32_t kTrbTypeEvaluateContextCommand = 13u;
constexpr uint32_t kTrbTypeNoOpCommand = 23u;
constexpr uint32_t kTrbTypeTransferEvent = 32u;
constexpr uint32_t kTrbTypeCommandCompletionEvent = 33u;
constexpr uint32_t kTrbTypePortStatusChangeEvent = 34u;

constexpr uint32_t kTrbCycleBit = 1u << 0;

constexpr size_t kOperationalPortRegisterBase = 0x400u;
constexpr size_t kPortRegisterStride = 0x10u;
constexpr size_t kPortSc = 0x00u;

constexpr uint32_t kPortScCurrentConnectStatus = 1u << 0;
constexpr uint32_t kPortScPortEnabled = 1u << 1;
constexpr uint32_t kPortScPortReset = 1u << 4;
constexpr uint32_t kPortScPortPower = 1u << 9;
constexpr uint32_t kPortScPortSpeedMask = 0xFu << 10;
constexpr uint32_t kPortScConnectStatusChange = 1u << 17;
constexpr uint32_t kPortScPortEnableChange = 1u << 18;
constexpr uint32_t kPortScWarmPortResetChange = 1u << 19;
constexpr uint32_t kPortScOverCurrentChange = 1u << 20;
constexpr uint32_t kPortScPortResetChange = 1u << 21;
constexpr uint32_t kPortScPortLinkStateChange = 1u << 22;
constexpr uint32_t kPortScPortConfigErrorChange = 1u << 23;
// Preserve only PP, PIC and wake enables. Never replay RW1C/RW1S bits.
constexpr uint32_t kPortScPreservedBits = (1u << 9) | (3u << 14) | (7u << 25);
constexpr uint32_t kPortScChangeBits = kPortScConnectStatusChange | kPortScPortEnableChange |
                                       kPortScWarmPortResetChange | kPortScOverCurrentChange |
                                       kPortScPortResetChange | kPortScPortLinkStateChange |
                                       kPortScPortConfigErrorChange;

constexpr size_t kOperationalUsbcmd = 0x00u;
constexpr size_t kOperationalUsbsts = 0x04u;
constexpr size_t kOperationalPagesize = 0x08u;
constexpr size_t kOperationalDnctrl = 0x14u;
constexpr size_t kOperationalCrcr = 0x18u;
constexpr size_t kOperationalDcbaap = 0x30u;
constexpr size_t kOperationalConfig = 0x38u;

constexpr size_t kRuntimeInterrupter0 = 0x20u;
constexpr size_t kInterrupterIman = 0x00u;
constexpr size_t kInterrupterImod = 0x04u;
constexpr size_t kInterrupterErstsz = 0x08u;
constexpr size_t kInterrupterErstba = 0x10u;
constexpr size_t kInterrupterErdp = 0x18u;

constexpr uint32_t kUsbcmdRunStop = 1u << 0;
constexpr uint32_t kUsbcmdHostControllerReset = 1u << 1;
constexpr uint32_t kUsbcmdInterrupterEnable = 1u << 2;

constexpr uint32_t kUsbstsHalted = 1u << 0;
constexpr uint32_t kUsbstsHostSystemError = 1u << 2;
constexpr uint32_t kUsbstsEventInterrupt = 1u << 3;
constexpr uint32_t kUsbstsPortChangeDetect = 1u << 4;
constexpr uint32_t kUsbstsControllerNotReady = 1u << 11;

constexpr uint32_t kImanInterruptPending = 1u << 0;
constexpr uint32_t kImanInterruptEnable = 1u << 1;
constexpr uint64_t kErdpEventHandlerBusy = 1u << 3;

struct Capability
{
    uint8_t cap_length = 0;
    uint16_t hci_version = 0;
    uint8_t max_slots = 0;
    uint16_t max_interrupters = 0;
    uint8_t max_ports = 0;
    uint8_t context_size_bytes = 32;
    uint16_t scratchpad_buffers = 0;
    uint32_t doorbell_offset = 0;
    uint32_t runtime_offset = 0;
};

struct ControllerLayout
{
    volatile uint8_t* mmio_base = nullptr;
    size_t mmio_size = 0;
    Capability capability{};
    volatile uint8_t* operational = nullptr;
    volatile uint8_t* runtime = nullptr;
    volatile uint8_t* doorbells = nullptr;
};

// Read-only firmware ownership snapshot. No handoff or SMI writes.
enum class LegacySupportResult : uint8_t
{
    Absent,
    Present,
    Invalid
};
struct LegacySupport
{
    uint32_t offset = 0;
    uint32_t support = 0;
    uint32_t control = 0;
};
LegacySupportResult read_legacy_support(const ControllerLayout&, LegacySupport&);
// Delay callback receives microseconds. A BIOS timeout fails without forcing ownership.
bool acquire_legacy_ownership(const ControllerLayout&,
                              void (*delay)(uint32_t, void*),
                              void* context);

struct RoutingIo
{
    void* context;
    uint32_t (*read)(void*, uint16_t);
    void (*write)(void*, uint16_t, uint32_t);
};
enum class RoutingResult : uint8_t
{
    NotApplicable,
    Unchanged,
    Changed,
    Failed
};
RoutingResult route_intel_ports(uint16_t vendor, uint16_t device, const RoutingIo&);

struct CommandRingInfo
{
    uint32_t trb_count = 0;
    uint32_t enqueue_index = 0;
    bool cycle_state = false;
};

struct TrbFields
{
    uint64_t parameter = 0;
    uint32_t status = 0;
    uint32_t control = 0;
};

struct EventRingInfo
{
    uint32_t trb_count = 0;
    uint32_t dequeue_index = 0;
    uint64_t dequeue_physical = 0;
    bool cycle_state = false;
};

enum class EventType : uint8_t
{
    None,
    Transfer,
    CommandCompletion,
    PortStatusChange,
    Unknown,
};

struct EventInfo
{
    EventType type = EventType::None;
    uint64_t parameter = 0;
    uint32_t status = 0;
    uint32_t control = 0;
    uint32_t transfer_length = 0;
    uint8_t completion_code = 0;
    uint8_t slot_id = 0;
    uint8_t endpoint_id = 0;
    uint8_t port_id = 0;
    uint8_t trb_type = 0;
};

// One serialized endpoint-zero request; stage addresses identify completion events.
struct ControlTransfer
{
    uint64_t stages[3]{};  // setup, optional data, status
    uint32_t requested = 0;
    uint32_t residual = 0;
    uint8_t completion = 0;
    bool done = false;
    bool direction_in = false;
};
bool queue_control_transfer(void* ring_base,
                            size_t ring_bytes,
                            uint64_t ring_physical,
                            CommandRingInfo& ring,
                            uint64_t setup,
                            uint64_t buffer,
                            ControlTransfer& transfer);
// Call once, after queueing succeeds and DMA/state writes are complete.
void publish_control_transfer(void* ring_base,
                              uint64_t ring_physical,
                              const ControlTransfer& transfer);
// Caller matches slot and endpoint under the event lock. Returns true for a matched event.
bool control_transfer_event(ControlTransfer& transfer, const EventInfo& event);
// Validate the initial eight bytes before a full descriptor transfer.
bool device_control_packet(uint8_t speed, const void* prefix, size_t actual, uint16_t& packet);
bool prepare_control_packet_context(void* input,
                                    size_t input_bytes,
                                    const void* output,
                                    size_t output_bytes,
                                    unsigned context_bytes,
                                    uint16_t packet);
uint8_t interrupt_interval(uint8_t speed, uint8_t interval);
bool valid_descriptor_header(const void* bytes, size_t actual, uint8_t type, uint8_t minimum);
bool valid_configuration_descriptors(const void* bytes, size_t actual);

struct PortStatus
{
    uint32_t raw = 0;
    uint8_t speed = 0;
    bool connected = false;
    bool enabled = false;
    bool powered = false;
    bool reset = false;
    bool has_changes = false;
};

struct [[gnu::packed]] EventRingSegmentTableEntry
{
    uint64_t ring_segment_base = 0;
    uint32_t ring_segment_size = 0;
    uint32_t reserved = 0;
};

struct RingSet
{
    uint64_t dcbaa_physical = 0;
    uint64_t command_ring_physical = 0;
    uint64_t event_ring_physical = 0;
    uint64_t erst_physical = 0;
    bool command_ring_cycle_state = false;
    uint32_t event_ring_trb_count = 0;
};

using PollHook = void (*)(volatile uint8_t* operational, void* context, uint32_t iteration);

void set_slot_context(uint32_t* words, uint8_t speed, uint8_t port, uint8_t context_entries);
bool parse_capability(volatile void* mmio_base, size_t mmio_size, ControllerLayout& out);
uint32_t read_page_size_mask(const ControllerLayout& controller);
bool initialize_command_ring(void* ring_base,
                             size_t ring_bytes,
                             uint64_t ring_physical,
                             CommandRingInfo& out);
bool initialize_event_ring_state(uint64_t event_ring_physical,
                                 uint32_t event_ring_trb_count,
                                 EventRingInfo& out);
bool enqueue_command_trb(void* ring_base,
                         size_t ring_bytes,
                         uint64_t ring_physical,
                         CommandRingInfo& ring,
                         const TrbFields& trb,
                         uint64_t* trb_physical = nullptr,
                         bool publish = true);
bool initialize_event_ring_segment_table(void* erst_base,
                                         size_t erst_bytes,
                                         uint64_t event_ring_physical,
                                         uint32_t event_ring_trb_count);
bool dequeue_event_trb(void* ring_base,
                       size_t ring_bytes,
                       uint64_t ring_physical,
                       EventRingInfo& ring,
                       EventInfo& out);
void ring_doorbell(const ControllerLayout& controller,
                   uint8_t doorbell_index,
                   uint8_t target,
                   uint16_t stream_id = 0u);
bool read_port_status(const ControllerLayout& controller, uint8_t port_id, PortStatus& out);
bool clear_port_change_bits(const ControllerLayout& controller,
                            uint8_t port_id,
                            uint32_t clear_mask = kPortScChangeBits);
bool power_on_port(const ControllerLayout& controller, uint8_t port_id);
bool controller_stop(const ControllerLayout& controller,
                     uint32_t timeout_spins,
                     PollHook poll_hook = nullptr,
                     void* poll_context = nullptr);
bool controller_reset(const ControllerLayout& controller,
                      uint32_t timeout_spins,
                      PollHook poll_hook = nullptr,
                      void* poll_context = nullptr);
bool controller_program(const ControllerLayout& controller,
                        const RingSet& rings,
                        uint8_t max_device_slots);
bool controller_run(const ControllerLayout& controller,
                    uint32_t timeout_spins,
                    PollHook poll_hook = nullptr,
                    void* poll_context = nullptr);
void acknowledge_interrupt(const ControllerLayout& controller,
                           uint64_t event_ring_dequeue_physical);
}  // namespace xhci
namespace xhci
{
void disable_interrupts(const ControllerLayout& controller);
}
