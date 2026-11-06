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
                   uint32_t timeout_spins,
                   PollHook poll_hook,
                   void* poll_context)
{
    for(uint32_t iteration = 0; iteration < timeout_spins; ++iteration)
    {
        if(nullptr != poll_hook)
        {
            poll_hook(operational, poll_context, iteration);
        }
        if((read32(operational, offset) & mask) == expected)
        {
            return true;
        }
        cpu_pause();
    }

    return (read32(operational, offset) & mask) == expected;
}
}  // namespace

void set_slot_context(uint32_t* words, uint8_t speed, uint8_t port, uint8_t context_entries)
{
    words[0] = (uint32_t(speed) << 20) | (uint32_t(context_entries) << 27);
    words[1] = uint32_t(port) << 16;
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
    out.capability.scratchpad_buffers =
        static_cast<uint16_t>(((hcsp2 >> 27) & 0x1Fu) | (((hcsp2 >> 21) & 0x1Fu) << 5));
    out.capability.doorbell_offset = doorbell_offset;
    out.capability.runtime_offset = runtime_offset;
    out.operational = base + cap_length;
    out.runtime = base + runtime_offset;
    out.doorbells = base + doorbell_offset;
    return true;
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
                         uint64_t* trb_physical)
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
    entry.control = (trb.control & ~kTrbCycleBit) | (ring.cycle_state ? kTrbCycleBit : 0u);

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
    if((nullptr == ring_base) || (0 == ring.trb_count) ||
       (ring_bytes != (static_cast<size_t>(ring.trb_count) * kTrbBytes)) ||
       (ring.dequeue_index >= ring.trb_count))
    {
        return false;
    }

    const auto* trbs = static_cast<const Trb*>(ring_base);
    const Trb& entry = trbs[ring.dequeue_index];
    const bool cycle = 0 != (entry.control & kTrbCycleBit);
    if(cycle != ring.cycle_state)
    {
        return false;
    }

    out.parameter = (static_cast<uint64_t>(entry.parameter_high) << 32) | entry.parameter_low;
    out.status = entry.status;
    out.control = entry.control;
    out.transfer_length = entry.status & kEventTransferLengthMask;
    out.completion_code = static_cast<uint8_t>(entry.status >> kEventCompletionCodeShift);
    out.trb_type = static_cast<uint8_t>((entry.control & kTrbTypeMask) >> kTrbTypeShift);

    switch(out.trb_type)
    {
        case kTrbTypeTransferEvent:
            out.type = EventType::Transfer;
            out.slot_id = static_cast<uint8_t>(entry.control >> 24);
            out.endpoint_id = static_cast<uint8_t>((entry.control >> 16) & 0x1Fu);
            break;
        case kTrbTypeCommandCompletionEvent:
            out.type = EventType::CommandCompletion;
            out.slot_id = static_cast<uint8_t>(entry.control >> 24);
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

bool controller_stop(const ControllerLayout& controller,
                     uint32_t timeout_spins,
                     PollHook poll_hook,
                     void* poll_context)
{
    if(nullptr == controller.operational)
    {
        return false;
    }

    const uint32_t command = read32(controller.operational, kOperationalUsbcmd);
    if(0 != (command & kUsbcmdRunStop))
    {
        write32(controller.operational,
                kOperationalUsbcmd,
                static_cast<uint32_t>(command & ~kUsbcmdRunStop));
    }
    return wait_for_mask(controller.operational,
                         kOperationalUsbsts,
                         kUsbstsHalted,
                         kUsbstsHalted,
                         timeout_spins,
                         poll_hook,
                         poll_context);
}

bool controller_reset(const ControllerLayout& controller,
                      uint32_t timeout_spins,
                      PollHook poll_hook,
                      void* poll_context)
{
    if(!controller_stop(controller, timeout_spins, poll_hook, poll_context))
    {
        return false;
    }

    const uint32_t command = read32(controller.operational, kOperationalUsbcmd);
    write32(controller.operational,
            kOperationalUsbcmd,
            static_cast<uint32_t>(command | kUsbcmdHostControllerReset));
    if(!wait_for_mask(controller.operational,
                      kOperationalUsbcmd,
                      kUsbcmdHostControllerReset,
                      0u,
                      timeout_spins,
                      poll_hook,
                      poll_context))
    {
        return false;
    }

    return wait_for_mask(controller.operational,
                         kOperationalUsbsts,
                         kUsbstsControllerNotReady,
                         0u,
                         timeout_spins,
                         poll_hook,
                         poll_context);
}

bool controller_program(const ControllerLayout& controller,
                        const RingSet& rings,
                        uint8_t max_device_slots)
{
    if((nullptr == controller.operational) || (nullptr == controller.runtime) ||
       (0 == rings.dcbaa_physical) || (0 == rings.command_ring_physical) ||
       (0 == rings.event_ring_physical) || (0 == rings.erst_physical) ||
       (0 == rings.event_ring_trb_count) || (0 == max_device_slots) ||
       (max_device_slots > controller.capability.max_slots))
    {
        return false;
    }

    write32(controller.operational, kOperationalDnctrl, 0u);
    write64(
        controller.operational,
        kOperationalCrcr,
        (rings.command_ring_physical & ~0x3Full) | (rings.command_ring_cycle_state ? 1ull : 0ull));
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
                    uint32_t timeout_spins,
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
                      timeout_spins,
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
                         timeout_spins,
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
