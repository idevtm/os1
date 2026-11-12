// xHCI capability decoding and controller MMIO programming helpers.
#include "drivers/usb/xhci_controller.hpp"

#include "util/memory.h"

namespace xhci
{
namespace
{
constexpr uint32_t kLinkTrbToggleCycle = 1u << 1;
constexpr uint32_t kTrbTypeShift = 10u;
constexpr uint32_t kTrbTypeMask = 0x3Fu << kTrbTypeShift;
constexpr uint32_t kUsbStsWriteClearMask =
    kUsbstsHostSystemError | kUsbstsEventInterrupt | kUsbstsPortChangeDetect;
constexpr uint32_t kEventCompletionCodeShift = 24u;
constexpr uint32_t kEventTransferLengthMask = 0x00FFFFFFu;

struct [[gnu::packed]] Trb
{
    uint32_t parameter_low;
    uint32_t parameter_high;
    uint32_t status;
    uint32_t control;
};

static_assert(sizeof(Trb) == kTrbBytes);
static_assert(offsetof(Trb, control) % alignof(uint32_t) == 0);

[[nodiscard]] uint32_t read32(volatile const uint8_t* base, size_t offset)
{
    return *reinterpret_cast<volatile const uint32_t*>(base + offset);
}

[[nodiscard]] uint16_t read16(volatile const uint8_t* base, size_t offset)
{
    return *reinterpret_cast<volatile const uint16_t*>(base + offset);
}

[[nodiscard]] uint8_t read8(volatile const uint8_t* base, size_t offset)
{
    return *(base + offset);
}

void write32(volatile uint8_t* base, size_t offset, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t*>(base + offset) = value;
}

void write64(volatile uint8_t* base, size_t offset, uint64_t value)
{
    write32(base, offset, static_cast<uint32_t>(value & 0xFFFFFFFFull));
    write32(base, offset + sizeof(uint32_t), static_cast<uint32_t>(value >> 32));
}

void cpu_pause()
{
#if defined(OS1_HOST_TEST)
    asm volatile("" : : : "memory");
#else
    asm volatile("pause" : : : "memory");
#endif
}

bool wait_for_mask(volatile uint8_t* operational,
                   size_t offset,
                   uint32_t mask,
                   uint32_t expected,
                   PollLimits limits,
                   PollHook poll_hook,
                   void* poll_context)
{
    PollDeadline deadline(limits.clock, limits.duration, limits.stalled_polls);
    for(uint32_t iteration = 0;; ++iteration)
    {
        if(nullptr != poll_hook)
        {
            poll_hook(operational, poll_context, iteration);
        }
        if((read32(operational, offset) & mask) == expected)
        {
            return true;
        }
        if(deadline.expired())
            return false;
        cpu_pause();
    }
}
}  // namespace

void CommandCompletion::record(const EventInfo& event)
{
    if(event.completion_code == kCompletionCommandRingStopped)
    {
        stopped = true;
        stopped_at = event.parameter;
    }
    else if(expected_trb && event.parameter == expected_trb && !complete)
    {
        complete = true;
        code = event.completion_code;
        slot = event.slot_id;
    }
}

bool CommandCompletion::can_reprogram() const
{
    // If no completion preceded the stop, the dequeue pointer must still name the command.
    return expected_trb && stopped && (complete || stopped_at == expected_trb);
}

PollDeadline::PollDeadline(PollClock clock, uint64_t duration, uint32_t stalled_polls)
    : clock_(clock)
    , duration_(duration)
    , start_(clock ? clock() : 0)
    , previous_(start_)
    , stalled_polls_(stalled_polls)
    , remaining_polls_(stalled_polls)
{
}

bool PollDeadline::expired()
{
    if(clock_)
    {
        const uint64_t now = clock_();
        if(now > previous_)
        {
            if(now - start_ >= duration_)
                return true;
            previous_ = now;
            remaining_polls_ = stalled_polls_;
            return false;
        }
    }
    return remaining_polls_ == 0 || --remaining_polls_ == 0;
}

void set_slot_context(uint32_t* words, uint8_t speed, uint8_t port, uint8_t context_entries)
{
    words[0] = (uint32_t(speed) << 20) | (uint32_t(context_entries) << 27);
    words[1] = uint32_t(port) << 16;
}

LegacySupportResult read_legacy_support(const ControllerLayout& layout, LegacySupport& out)
{
    out = {};
    if(!layout.mmio_base || layout.mmio_size < 0x14u)
        return LegacySupportResult::Invalid;
    size_t offset = static_cast<size_t>(read32(layout.mmio_base, 0x10u) >> 16) * 4u;
    // Next pointers are forward DWORD offsets. Limit reads even in hostile tables.
    for(unsigned visited = 0; offset && visited < 256u; ++visited)
    {
        if(offset < 0x20u || offset > layout.mmio_size || layout.mmio_size - offset < 4u)
            return LegacySupportResult::Invalid;
        const uint32_t header = read32(layout.mmio_base, offset);
        if((header & 0xffu) == 1u)
        {
            if(layout.mmio_size - offset < 8u)
                return LegacySupportResult::Invalid;
            out = {static_cast<uint32_t>(offset), header, read32(layout.mmio_base, offset + 4u)};
            return LegacySupportResult::Present;
        }
        const size_t next = ((header >> 8) & 0xffu) * 4u;
        if(!next)
            return LegacySupportResult::Absent;
        offset += next;
    }
    return offset ? LegacySupportResult::Invalid : LegacySupportResult::Absent;
}

bool acquire_legacy_ownership(const ControllerLayout& layout,
                              void (*delay)(uint32_t, void*),
                              void* context)
{
    LegacySupport legacy{};
    const auto found = read_legacy_support(layout, legacy);
    if(found == LegacySupportResult::Absent)
        return true;
    if(found == LegacySupportResult::Invalid || !delay)
        return false;
    constexpr uint32_t bios_owned = 1u << 16;
    constexpr uint32_t os_owned = 1u << 24;
    write32(layout.mmio_base, legacy.offset, legacy.support | os_owned);
    for(unsigned wait = 0;; ++wait)
    {
        const uint32_t support = read32(layout.mmio_base, legacy.offset);
        if(!(support & bios_owned) && (support & os_owned))
            break;
        if(wait == 1000u)
            return false;
        delay(1000u, context);
    }
    // xHCI 7.1.2: clear only SMI enables and acknowledge W1C events.
    // Read-only event/error status can remain set after this write.
    constexpr uint32_t enables = (1u << 0) | (1u << 4) | (7u << 13);
    constexpr uint32_t events = 7u << 29;
    const uint32_t control = read32(layout.mmio_base, legacy.offset + 4u);
    write32(layout.mmio_base, legacy.offset + 4u, (control & ~enables) | events);
    return !(read32(layout.mmio_base, legacy.offset + 4u) & enables);
}

RoutingResult route_intel_ports(uint16_t vendor, uint16_t device, const RoutingIo& io)
{
    // The returned H81M-S1 log qualifies these PCI register offsets for this ID.
    if(vendor != 0x8086u || device != 0x8c31u)
        return RoutingResult::NotApplicable;
    if(!io.read || !io.write)
        return RoutingResult::Failed;
    const uint32_t usb2 = io.read(io.context, 0xd0);
    const uint32_t mask2 = io.read(io.context, 0xd4);
    const uint32_t usb3 = io.read(io.context, 0xd8);
    const uint32_t mask3 = io.read(io.context, 0xdc);
    if(usb2 == 0xffffffffu || usb3 == 0xffffffffu || mask2 == 0xffffffffu || mask3 == 0xffffffffu)
        return RoutingResult::Failed;
    if((usb2 & mask2) == mask2 && (usb3 & mask3) == mask3)
        return RoutingResult::Unchanged;
    io.write(io.context, 0xd8, usb3 | mask3);
    if((io.read(io.context, 0xd8) & mask3) == mask3)
    {
        io.write(io.context, 0xd0, usb2 | mask2);
        if((io.read(io.context, 0xd0) & mask2) == mask2)
            return RoutingResult::Changed;
    }
    // No device has been enumerated on this controller yet. Restore routing
    // before EHCI probes, so a partial switchover cannot publish stale storage.
    io.write(io.context, 0xd0, usb2);
    io.write(io.context, 0xd8, usb3);
    return RoutingResult::Failed;
}

bool parse_capability(volatile void* mmio_base, size_t mmio_size, ControllerLayout& out)
{
    out = {};
    if((nullptr == mmio_base) || (mmio_size < 0x40u))
    {
        return false;
    }

    auto* base = static_cast<volatile uint8_t*>(mmio_base);
    const uint8_t cap_length = read8(base, 0x00u);
    if((cap_length < 0x20u) || (static_cast<size_t>(cap_length) >= mmio_size))
    {
        return false;
    }

    const uint32_t hcsp1 = read32(base, 0x04u);
    const uint32_t hcsp2 = read32(base, 0x08u);
    const uint32_t hccparams1 = read32(base, 0x10u);
    const uint32_t doorbell_offset = read32(base, 0x14u) & ~0x3u;
    const uint32_t runtime_offset = read32(base, 0x18u) & ~0x1Fu;
    if((static_cast<size_t>(cap_length) + kOperationalConfig + sizeof(uint32_t) > mmio_size) ||
       (static_cast<size_t>(doorbell_offset) + sizeof(uint32_t) > mmio_size) ||
       (static_cast<size_t>(runtime_offset) + kRuntimeInterrupter0 + kInterrupterErdp +
            sizeof(uint64_t) >
        mmio_size))
    {
        return false;
    }

    out.mmio_base = base;
    out.mmio_size = mmio_size;
    out.capability.cap_length = cap_length;
    out.capability.hci_version = read16(base, 0x02u);
    out.capability.max_slots = static_cast<uint8_t>(hcsp1 & 0xFFu);
    out.capability.max_interrupters = static_cast<uint16_t>((hcsp1 >> 8) & 0x7FFu);
    out.capability.max_ports = static_cast<uint8_t>((hcsp1 >> 24) & 0xFFu);
    out.capability.context_size_bytes = (0 != (hccparams1 & (1u << 2))) ? 64u : 32u;
    out.capability.address_64 = (hccparams1 & 1u) != 0;
    out.capability.scratchpad_buffers =
        static_cast<uint16_t>(((hcsp2 >> 27) & 0x1Fu) | (((hcsp2 >> 21) & 0x1Fu) << 5));
    out.capability.doorbell_offset = doorbell_offset;
    out.capability.runtime_offset = runtime_offset;
    out.operational = base + cap_length;
    out.runtime = base + runtime_offset;
    out.doorbells = base + doorbell_offset;
    return true;
}

bool dma_range_supported(const Capability& capability, uint64_t address, size_t bytes)
{
    const uint64_t limit = capability.address_64 ? UINT64_MAX : UINT32_MAX;
    return bytes && address <= limit && bytes - 1 <= limit - address;
}

uint32_t read_page_size_mask(const ControllerLayout& controller)
{
    return (nullptr != controller.operational)
               ? read32(controller.operational, kOperationalPagesize)
               : 0u;
}

bool initialize_command_ring(void* ring_base,
                             size_t ring_bytes,
                             uint64_t ring_physical,
                             CommandRingInfo& out)
{
    out = {};
    if((nullptr == ring_base) || (ring_bytes < (2u * kTrbBytes)) ||
       (0 != (ring_bytes % kTrbBytes)) || (0 != (ring_physical & 0x3Full)))
    {
        return false;
    }

    memset(ring_base, 0, ring_bytes);
    const uint32_t trb_count = static_cast<uint32_t>(ring_bytes / kTrbBytes);
    auto* trbs = static_cast<Trb*>(ring_base);
    Trb& link = trbs[trb_count - 1u];
    link.parameter_low = static_cast<uint32_t>(ring_physical & 0xFFFFFFFFull);
    link.parameter_high = static_cast<uint32_t>(ring_physical >> 32);
    link.status = 0;
    link.control = (kTrbTypeLink << kTrbTypeShift) | kLinkTrbToggleCycle | 1u;

    out.trb_count = trb_count;
    out.enqueue_index = 0;
    out.cycle_state = true;
    return true;
}

bool initialize_event_ring_state(uint64_t event_ring_physical,
                                 uint32_t event_ring_trb_count,
                                 EventRingInfo& out)
{
    out = {};
    if((0 == event_ring_physical) || (0 == event_ring_trb_count))
    {
        return false;
    }

    out.trb_count = event_ring_trb_count;
    out.dequeue_index = 0;
    out.dequeue_physical = event_ring_physical;
    out.cycle_state = true;
    return true;
}

bool enqueue_command_trb(void* ring_base,
                         size_t ring_bytes,
                         uint64_t ring_physical,
                         CommandRingInfo& ring,
                         const TrbFields& trb,
                         uint64_t* trb_physical,
                         bool publish)
{
    if((nullptr == ring_base) || (ring.trb_count < 2u) ||
       (ring_bytes != (static_cast<size_t>(ring.trb_count) * kTrbBytes)) ||
       (ring.enqueue_index >= (ring.trb_count - 1u)))
    {
        return false;
    }

    auto* trbs = static_cast<Trb*>(ring_base);
    Trb& entry = trbs[ring.enqueue_index];
    entry.parameter_low = static_cast<uint32_t>(trb.parameter & 0xFFFFFFFFull);
    entry.parameter_high = static_cast<uint32_t>(trb.parameter >> 32);
    entry.status = trb.status;
    entry.control =
        (trb.control & ~kTrbCycleBit) | ((ring.cycle_state == publish) ? kTrbCycleBit : 0u);

    if(nullptr != trb_physical)
    {
        *trb_physical = ring_physical + static_cast<uint64_t>(ring.enqueue_index) * kTrbBytes;
    }

    ++ring.enqueue_index;
    if(ring.enqueue_index == (ring.trb_count - 1u))
    {
        Trb& link = trbs[ring.trb_count - 1u];
        link.control = (link.control & ~kTrbCycleBit) | (ring.cycle_state ? kTrbCycleBit : 0u);
        ring.enqueue_index = 0;
        ring.cycle_state = !ring.cycle_state;
    }

    return true;
}

bool matches_hid_transfer(
    const EventInfo& event, uint8_t slot, uint8_t endpoint, uint64_t trb, bool armed)
{
    return armed && event.slot_id == slot && event.endpoint_id == endpoint &&
           event.parameter == trb;
}

HidCompletion HidRecovery::complete(uint8_t completion)
{
    switch(completion)
    {
        case 1:   // Success.
        case 13:  // Short Packet.
            attempted = false;
            return HidCompletion::Report;
        case 3:  // Babble.
        case 4:  // USB Transaction Error.
        case 6:  // Stall.
            if(attempted)
                return HidCompletion::Retire;
            attempted = true;
            pending = completion;
            return HidCompletion::Recover;
        default:
            return HidCompletion::Rearm;
    }
}

bool queue_control_transfer(void* ring_base,
                            size_t ring_bytes,
                            uint64_t ring_physical,
                            CommandRingInfo& ring,
                            uint64_t setup,
                            uint64_t buffer,
                            ControlTransfer& transfer)
{
    transfer = {};
    const uint32_t length = setup >> 48;
    const bool in = setup & 0x80u;
    // This profile uses one bounded, physically contiguous data TRB.
    if(!ring_base || !ring_physical || length > 4096u ||
       (length && (!buffer || (buffer & 0xffffu) + length > 65536u)) ||
       ring.trb_count < (length ? 4u : 3u) || ring_bytes != size_t(ring.trb_count) * kTrbBytes ||
       ring.enqueue_index >= ring.trb_count - 1u)
        return false;
    const TrbFields stages[3]{
        {setup,
         8u,
         (kTrbTypeSetupStage << 10) | (1u << 6) | (length ? ((in ? 3u : 2u) << 16) : 0u)},
        {buffer, length, (kTrbTypeDataStage << 10) | (in ? ((1u << 16) | (1u << 2)) : 0u)},
        {0, 0, (kTrbTypeStatusStage << 10) | (1u << 5) | ((!length || !in) ? (1u << 16) : 0u)}};
    transfer.requested = length;
    transfer.direction_in = in;
    for(unsigned i = 0; i < 3; ++i)
    {
        if(i == 1 && !length)
            continue;
        // Setup remains unowned until every stage and completion field is ready.
        if(!enqueue_command_trb(
               ring_base, ring_bytes, ring_physical, ring, stages[i], &transfer.stages[i], i != 0))
            return false;
    }
    return true;
}

void publish_control_transfer(void* ring_base,
                              uint64_t ring_physical,
                              const ControlTransfer& transfer)
{
    auto* control = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(ring_base) +
                                                transfer.stages[0] - ring_physical + 12u);
    __atomic_store_n(control, *control ^ kTrbCycleBit, __ATOMIC_RELEASE);
}

bool control_transfer_event(ControlTransfer& transfer, const EventInfo& event)
{
    if(transfer.done || !event.parameter || !transfer.stages[0])
        return false;
    unsigned stage = 0;
    while(stage < 3 && transfer.stages[stage] != event.parameter)
        ++stage;
    if(stage == 3)
        return false;
    if(stage == 1 && transfer.direction_in && event.completion_code == 13u &&
       event.transfer_length <= transfer.requested)
    {
        transfer.residual = event.transfer_length;
        return true;  // Short data is not completion of the status stage.
    }
    transfer.done = true;
    if(stage == 2 && event.completion_code == 1u && !event.transfer_length)
        transfer.completion = 1u;
    else
    {
        transfer.completion = (event.completion_code == 0u || event.completion_code == 1u ||
                               event.completion_code == 13u)
                                  ? 5u
                                  : event.completion_code;
        transfer.residual = transfer.requested;
    }
    return true;
}

bool device_control_packet(uint8_t speed, const void* prefix, size_t actual, uint16_t& packet)
{
    packet = 0;
    const auto* bytes = static_cast<const uint8_t*>(prefix);
    if(!bytes || actual != 8 || bytes[0] != 18 || bytes[1] != 1)
        return false;
    unsigned value = bytes[7];
    if(speed == 2 && value != 8)
        return false;
    if(speed == 3 && value != 64)
        return false;
    if(speed >= 4 && speed <= 5)
    {
        if(value != 9)
            return false;
        value = 512;
    }
    else if(speed != 1 && speed != 2 && speed != 3)
        return false;
    if(value != 8 && value != 16 && value != 32 && value != 64 && value != 512)
        return false;
    packet = value;
    return true;
}

bool prepare_control_packet_context(void* input,
                                    size_t input_bytes,
                                    const void* output,
                                    size_t output_bytes,
                                    unsigned context_bytes,
                                    uint16_t packet)
{
    if(!input || !output || (context_bytes != 32 && context_bytes != 64) ||
       input_bytes < 3 * context_bytes || output_bytes < 2 * context_bytes ||
       (packet != 8 && packet != 16 && packet != 32 && packet != 64 && packet != 512))
        return false;
    auto* in = static_cast<uint8_t*>(input);
    const auto* out = static_cast<const uint8_t*>(output);
    for(size_t i = 0; i < input_bytes; ++i)
        in[i] = 0;
    for(unsigned i = 0; i < context_bytes; ++i)
        in[2 * context_bytes + i] = out[context_bytes + i];
    // Add EP0 only; preserve the controller's dequeue position and lower fields.
    in[4] = 2;
    in[2 * context_bytes + 6] = packet & 255;
    in[2 * context_bytes + 7] = packet >> 8;
    return true;
}

uint8_t interrupt_interval(uint8_t speed, uint8_t interval)
{
    if(speed >= 3)
        return interval ? (interval > 16 ? 15 : interval - 1) : 0;
    // USB low/full-speed bInterval is milliseconds; xHCI uses powers of microframes.
    uint8_t exponent = 3;
    while(interval > 1)
    {
        interval >>= 1;
        ++exponent;
    }
    return exponent;
}

bool valid_descriptor_header(const void* bytes, size_t actual, uint8_t type, uint8_t minimum)
{
    const auto* header = static_cast<const uint8_t*>(bytes);
    return header && actual >= minimum && minimum >= 2 && header[0] >= minimum &&
           header[0] <= actual && header[1] == type;
}

bool valid_configuration_descriptors(const void* bytes, size_t actual)
{
    if(!valid_descriptor_header(bytes, actual, 2, 9))
        return false;
    const auto* data = static_cast<const uint8_t*>(bytes);
    if((uint32_t(data[2]) | (uint32_t(data[3]) << 8)) != actual)
        return false;
    for(size_t offset = 0; offset < actual;)
    {
        if(actual - offset < 2)
            return false;
        const uint8_t type = data[offset + 1];
        const uint8_t minimum = (type == 2 || type == 4) ? 9 : (type == 5 ? 7 : 2);
        if(!valid_descriptor_header(data + offset, actual - offset, type, minimum))
            return false;
        offset += data[offset];
    }
    return true;
}

bool initialize_event_ring_segment_table(void* erst_base,
                                         size_t erst_bytes,
                                         uint64_t event_ring_physical,
                                         uint32_t event_ring_trb_count)
{
    if((nullptr == erst_base) || (erst_bytes < sizeof(EventRingSegmentTableEntry)) ||
       (0 == event_ring_physical) || (0 == event_ring_trb_count))
    {
        return false;
    }

    memset(erst_base, 0, erst_bytes);
    auto* erst = static_cast<EventRingSegmentTableEntry*>(erst_base);
    erst[0].ring_segment_base = event_ring_physical;
    erst[0].ring_segment_size = event_ring_trb_count;
    erst[0].reserved = 0;
    return true;
}

bool dequeue_event_trb(
    void* ring_base, size_t ring_bytes, uint64_t ring_physical, EventRingInfo& ring, EventInfo& out)
{
    out = {};
    if((nullptr == ring_base) || (reinterpret_cast<uintptr_t>(ring_base) % alignof(uint32_t)) ||
       (0 == ring.trb_count) || (ring_bytes != (static_cast<size_t>(ring.trb_count) * kTrbBytes)) ||
       (ring.dequeue_index >= ring.trb_count))
    {
        return false;
    }

    const auto* trbs = static_cast<const Trb*>(ring_base);
    const Trb& entry = trbs[ring.dequeue_index];
    const auto* control_word = reinterpret_cast<const uint32_t*>(
        reinterpret_cast<const uint8_t*>(&entry) + offsetof(Trb, control));
    // The controller publishes ownership through the cycle bit on coherent DMA memory.
    const uint32_t control = __atomic_load_n(control_word, __ATOMIC_ACQUIRE);
    const bool cycle = 0 != (control & kTrbCycleBit);
    if(cycle != ring.cycle_state)
    {
        return false;
    }

    out.parameter = (static_cast<uint64_t>(entry.parameter_high) << 32) | entry.parameter_low;
    out.status = entry.status;
    out.control = control;
    out.transfer_length = out.status & kEventTransferLengthMask;
    out.completion_code = static_cast<uint8_t>(out.status >> kEventCompletionCodeShift);
    out.trb_type = static_cast<uint8_t>((control & kTrbTypeMask) >> kTrbTypeShift);

    switch(out.trb_type)
    {
        case kTrbTypeTransferEvent:
            out.type = EventType::Transfer;
            out.slot_id = static_cast<uint8_t>(control >> 24);
            out.endpoint_id = static_cast<uint8_t>((control >> 16) & 0x1Fu);
            break;
        case kTrbTypeCommandCompletionEvent:
            out.type = EventType::CommandCompletion;
            out.slot_id = static_cast<uint8_t>(control >> 24);
            break;
        case kTrbTypePortStatusChangeEvent:
            out.type = EventType::PortStatusChange;
            out.port_id = static_cast<uint8_t>(out.parameter >> 24);
            break;
        default:
            out.type = EventType::Unknown;
            break;
    }

    ++ring.dequeue_index;
    if(ring.dequeue_index == ring.trb_count)
    {
        ring.dequeue_index = 0;
        ring.cycle_state = !ring.cycle_state;
    }
    ring.dequeue_physical = ring_physical + static_cast<uint64_t>(ring.dequeue_index) * kTrbBytes;
    return true;
}

void ring_doorbell(const ControllerLayout& controller,
                   uint8_t doorbell_index,
                   uint8_t target,
                   uint16_t stream_id)
{
    if(nullptr == controller.doorbells)
    {
        return;
    }

    write32(controller.doorbells,
            static_cast<size_t>(doorbell_index) * kDoorbellBytes,
            static_cast<uint32_t>(target) | (static_cast<uint32_t>(stream_id) << 16));
}

bool port_reset_recovered(const PortStatus& status, uint8_t speed, bool connection_changed)
{
    return !connection_changed && !(status.raw & kPortScConnectStatusChange) && status.connected &&
           status.enabled && !status.reset && status.speed == speed;
}

void RuntimePortReset::start(uint64_t now)
{
    deadline_ = now + 10'000'000;  // One second for reset completion.
    previous_time_ = now;
    remaining_polls_ = kStalledPollLimit;
    recovering_ = false;
}

PortResetResult RuntimePortReset::poll(const PortStatus& status, uint64_t now)
{
    if(!status.connected || (status.raw & kPortScConnectStatusChange))
        return PortResetResult::Failed;
    if(now > previous_time_)
    {
        previous_time_ = now;
        remaining_polls_ = kStalledPollLimit;
    }
    else if(!remaining_polls_ || --remaining_polls_ == 0)
        return PortResetResult::Failed;
    if(recovering_)
    {
        if(!port_reset_recovered(status, speed_, false))
            return PortResetResult::Failed;
        return now >= deadline_ ? PortResetResult::Ready : PortResetResult::Pending;
    }
    if(now >= deadline_)
        return PortResetResult::Failed;
    if(status.enabled && !status.reset)
    {
        speed_ = status.speed;
        recovering_ = true;
        remaining_polls_ = kStalledPollLimit;
        // A backward sample cannot shorten the recovery interval.
        deadline_ = previous_time_ + 100'000;
    }
    return PortResetResult::Pending;
}

bool read_port_status(const ControllerLayout& controller, uint8_t port_id, PortStatus& out)
{
    out = {};
    if((nullptr == controller.operational) || (0u == port_id) ||
       (port_id > controller.capability.max_ports))
    {
        return false;
    }

    const size_t offset = kOperationalPortRegisterBase +
                          (static_cast<size_t>(port_id) - 1u) * kPortRegisterStride + kPortSc;
    if((static_cast<size_t>(controller.capability.cap_length) + offset + sizeof(uint32_t)) >
       controller.mmio_size)
    {
        return false;
    }

    const uint32_t raw = read32(controller.operational, offset);
    out.raw = raw;
    out.speed = static_cast<uint8_t>((raw & kPortScPortSpeedMask) >> 10);
    out.connected = 0 != (raw & kPortScCurrentConnectStatus);
    out.enabled = 0 != (raw & kPortScPortEnabled);
    out.powered = 0 != (raw & kPortScPortPower);
    out.reset = 0 != (raw & kPortScPortReset);
    out.has_changes = 0 != (raw & kPortScChangeBits);
    return true;
}

bool clear_port_change_bits(const ControllerLayout& controller,
                            uint8_t port_id,
                            uint32_t clear_mask)
{
    PortStatus status{};
    if(!read_port_status(controller, port_id, status))
    {
        return false;
    }

    const size_t offset = kOperationalPortRegisterBase +
                          (static_cast<size_t>(port_id) - 1u) * kPortRegisterStride + kPortSc;
    write32(controller.operational,
            offset,
            (status.raw & kPortScPreservedBits) | (clear_mask & kPortScChangeBits));
    return true;
}

bool power_on_port(const ControllerLayout& controller, uint8_t port_id)
{
    PortStatus status{};
    if(!read_port_status(controller, port_id, status))
    {
        return false;
    }

    const size_t offset = kOperationalPortRegisterBase +
                          (static_cast<size_t>(port_id) - 1u) * kPortRegisterStride + kPortSc;
    write32(controller.operational, offset, (status.raw & kPortScPreservedBits) | kPortScPortPower);
    return true;
}

void request_controller_stop(const ControllerLayout& controller)
{
    if(!controller.operational)
        return;
    const uint32_t command = read32(controller.operational, kOperationalUsbcmd);
    if(0 != (command & kUsbcmdRunStop))
    {
        write32(controller.operational,
                kOperationalUsbcmd,
                static_cast<uint32_t>(command & ~kUsbcmdRunStop));
    }
}

bool controller_stop(const ControllerLayout& controller,
                     PollLimits limits,
                     PollHook poll_hook,
                     void* poll_context)
{
    if(nullptr == controller.operational)
    {
        return false;
    }

    request_controller_stop(controller);
    return wait_for_mask(controller.operational,
                         kOperationalUsbsts,
                         kUsbstsHalted,
                         kUsbstsHalted,
                         limits,
                         poll_hook,
                         poll_context);
}

bool controller_reset(const ControllerLayout& controller,
                      PollLimits limits,
                      PollHook poll_hook,
                      void* poll_context,
                      void (*intel_reset_delay)(uint32_t, void*))
{
    if(!controller_stop(controller, limits, poll_hook, poll_context))
    {
        return false;
    }

    const uint32_t command = read32(controller.operational, kOperationalUsbcmd);
    write32(controller.operational,
            kOperationalUsbcmd,
            static_cast<uint32_t>(command | kUsbcmdHostControllerReset));
    // Intel controllers need this delay before the next register access.
    if(intel_reset_delay)
        intel_reset_delay(1000u, poll_context);
    if(!wait_for_mask(controller.operational,
                      kOperationalUsbcmd,
                      kUsbcmdHostControllerReset,
                      0u,
                      limits,
                      poll_hook,
                      poll_context))
    {
        return false;
    }

    return wait_for_mask(controller.operational,
                         kOperationalUsbsts,
                         kUsbstsControllerNotReady,
                         0u,
                         limits,
                         poll_hook,
                         poll_context);
}

bool command_ring_running(const ControllerLayout& controller)
{
    return controller.operational &&
           (read32(controller.operational, kOperationalCrcr) & kCrcrCommandRingRunning);
}

bool request_command_abort(const ControllerLayout& controller,
                           uint64_t ring_physical,
                           size_t ring_bytes)
{
    if(!controller.operational || !ring_physical || (ring_physical & 0x3full) ||
       !dma_range_supported(controller.capability, ring_physical, ring_bytes))
        return false;
    // Confirm the abort with its stop event and a post-write CRR read.
    write64(controller.operational, kOperationalCrcr, ring_physical | kCrcrCommandAbort);
    return true;
}

bool program_command_ring(const ControllerLayout& controller,
                          uint64_t ring_physical,
                          size_t ring_bytes,
                          bool cycle)
{
    if(!controller.operational || !ring_physical || (ring_physical & 0x3full) ||
       !dma_range_supported(controller.capability, ring_physical, ring_bytes) ||
       command_ring_running(controller))
        return false;
    write64(controller.operational,
            kOperationalCrcr,
            ring_physical | (cycle ? kCrcrRingCycleState : 0u));
    return true;
}

bool controller_program(const ControllerLayout& controller,
                        const RingSet& rings,
                        uint8_t max_device_slots)
{
    if((nullptr == controller.operational) || (nullptr == controller.runtime) ||
       (0 == rings.dcbaa_physical) || (0 == rings.command_ring_physical) ||
       (0 == rings.event_ring_physical) || (0 == rings.erst_physical) ||
       (0 == rings.event_ring_trb_count) || (0 == max_device_slots) ||
       (max_device_slots > controller.capability.max_slots) ||
       !dma_range_supported(controller.capability,
                            rings.dcbaa_physical,
                            (size_t(max_device_slots) + 1) * sizeof(uint64_t)) ||
       !dma_range_supported(controller.capability,
                            rings.event_ring_physical,
                            size_t(rings.event_ring_trb_count) * kTrbBytes) ||
       !dma_range_supported(
           controller.capability, rings.erst_physical, sizeof(EventRingSegmentTableEntry)))
    {
        return false;
    }

    if(!program_command_ring(controller,
                             rings.command_ring_physical,
                             rings.command_ring_bytes,
                             rings.command_ring_cycle_state))
        return false;
    write32(controller.operational, kOperationalDnctrl, 0u);
    write64(controller.operational, kOperationalDcbaap, rings.dcbaa_physical);
    write32(controller.operational, kOperationalConfig, max_device_slots);

    volatile uint8_t* interrupter = controller.runtime + kRuntimeInterrupter0;
    write32(interrupter, kInterrupterImod, 0u);
    write32(interrupter, kInterrupterErstsz, 1u);
    write64(interrupter, kInterrupterErstba, rings.erst_physical);
    write64(interrupter, kInterrupterErdp, rings.event_ring_physical);
    write32(interrupter, kInterrupterIman, kImanInterruptEnable);
    return true;
}

bool controller_run(const ControllerLayout& controller,
                    PollLimits limits,
                    PollHook poll_hook,
                    void* poll_context)
{
    if(nullptr == controller.operational)
    {
        return false;
    }

    if(!wait_for_mask(controller.operational,
                      kOperationalUsbsts,
                      kUsbstsControllerNotReady,
                      0u,
                      limits,
                      poll_hook,
                      poll_context))
    {
        return false;
    }

    const uint32_t command = read32(controller.operational, kOperationalUsbcmd);
    write32(controller.operational,
            kOperationalUsbcmd,
            static_cast<uint32_t>(command | kUsbcmdRunStop | kUsbcmdInterrupterEnable));
    return wait_for_mask(controller.operational,
                         kOperationalUsbsts,
                         kUsbstsHalted,
                         0u,
                         limits,
                         poll_hook,
                         poll_context);
}

void acknowledge_interrupt(const ControllerLayout& controller, uint64_t event_ring_dequeue_physical)
{
    if((nullptr == controller.operational) || (nullptr == controller.runtime))
    {
        return;
    }

    const uint32_t status = read32(controller.operational, kOperationalUsbsts);
    const uint32_t clear_bits = status & kUsbStsWriteClearMask;
    if(0 != clear_bits)
    {
        write32(controller.operational, kOperationalUsbsts, clear_bits);
    }

    volatile uint8_t* interrupter = controller.runtime + kRuntimeInterrupter0;
    const uint32_t iman = read32(interrupter, kInterrupterIman);
    const uint32_t iman_value =
        static_cast<uint32_t>(kImanInterruptEnable |
                              ((0 != (iman & kImanInterruptPending)) ? kImanInterruptPending : 0u));
    write32(interrupter, kInterrupterIman, iman_value);
    if(0 != event_ring_dequeue_physical)
    {
        write64(interrupter,
                kInterrupterErdp,
                (event_ring_dequeue_physical & ~0xFull) | kErdpEventHandlerBusy);
    }
}
}  // namespace xhci
void xhci::disable_interrupts(const ControllerLayout& controller)
{
    if(controller.operational)
        write32(controller.operational,
                kOperationalUsbcmd,
                read32(controller.operational, kOperationalUsbcmd) & ~kUsbcmdInterrupterEnable);
    if(controller.runtime)
        write32(controller.runtime + kRuntimeInterrupter0, kInterrupterIman, kImanInterruptPending);
}
