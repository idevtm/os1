#include <gtest/gtest.h>

#include <array>
#include <cstring>

#include "drivers/usb/xhci_controller.hpp"

namespace
{
template<size_t N>
void write8(std::array<uint8_t, N>& bytes, size_t offset, uint8_t value)
{
    bytes[offset] = value;
}

template<size_t N>
void write16(std::array<uint8_t, N>& bytes, size_t offset, uint16_t value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template<size_t N>
void write32(std::array<uint8_t, N>& bytes, size_t offset, uint32_t value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template<size_t N>
void write64(std::array<uint8_t, N>& bytes, size_t offset, uint64_t value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template<size_t N>
uint32_t read32(const std::array<uint8_t, N>& bytes, size_t offset)
{
    uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

template<size_t N>
uint64_t read64(const std::array<uint8_t, N>& bytes, size_t offset)
{
    uint64_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

template<size_t N>
void write_event_trb(std::array<uint8_t, N>& bytes,
                     size_t offset,
                     uint64_t parameter,
                     uint32_t status,
                     uint32_t control)
{
    write64(bytes, offset + 0u, parameter);
    write32(bytes, offset + 8u, status);
    write32(bytes, offset + 12u, control);
}

template<size_t N>
void populate_capability_block(std::array<uint8_t, N>& mmio)
{
    write8(mmio, 0x00, 0x40);
    write16(mmio, 0x02, 0x0120);
    write32(mmio, 0x04, 0x08000420u);
    write32(mmio, 0x08, 3u << 27);
    write32(mmio, 0x14, 0x100u);
    write32(mmio, 0x18, 0x200u);
}

struct ResetHookState
{
    bool halted_reported = false;
    bool reset_cleared = false;
};

void emulate_reset(volatile uint8_t* operational, void* context, uint32_t)
{
    auto& state = *static_cast<ResetHookState*>(context);
    auto* command = reinterpret_cast<volatile uint32_t*>(operational + xhci::kOperationalUsbcmd);
    auto* status = reinterpret_cast<volatile uint32_t*>(operational + xhci::kOperationalUsbsts);

    if(!state.halted_reported && (0u == (*command & xhci::kUsbcmdRunStop)))
    {
        *status = xhci::kUsbstsHalted;
        state.halted_reported = true;
    }

    if(!state.reset_cleared && (0u != (*command & xhci::kUsbcmdHostControllerReset)))
    {
        *command = static_cast<uint32_t>(*command & ~xhci::kUsbcmdHostControllerReset);
        *status = xhci::kUsbstsHalted;
        state.reset_cleared = true;
    }
}
}  // namespace

TEST(XhciController, ParsesCapabilityAndRegisterWindows)
{
    std::array<uint8_t, 0x400> mmio{};
    populate_capability_block(mmio);

    xhci::ControllerLayout layout{};
    ASSERT_TRUE(xhci::parse_capability(mmio.data(), mmio.size(), layout));
    EXPECT_EQ(0x40u, layout.capability.cap_length);
    EXPECT_EQ(0x0120u, layout.capability.hci_version);
    EXPECT_EQ(0x20u, layout.capability.max_slots);
    EXPECT_EQ(0x004u, layout.capability.max_interrupters);
    EXPECT_EQ(0x08u, layout.capability.max_ports);
    EXPECT_EQ(32u, layout.capability.context_size_bytes);
    EXPECT_EQ(3u, layout.capability.scratchpad_buffers);
    EXPECT_EQ(0x100u, layout.capability.doorbell_offset);
    EXPECT_EQ(0x200u, layout.capability.runtime_offset);
    EXPECT_EQ(mmio.data() + 0x40u, layout.operational);
    EXPECT_EQ(mmio.data() + 0x100u, layout.doorbells);
    EXPECT_EQ(mmio.data() + 0x200u, layout.runtime);
}

TEST(XhciController, InitializesCommandRingWithLinkTrb)
{
    std::array<uint8_t, 4096> ring{};
    xhci::CommandRingInfo info{};
    ASSERT_TRUE(xhci::initialize_command_ring(ring.data(), ring.size(), 0x200000u, info));
    EXPECT_EQ(256u, info.trb_count);
    EXPECT_TRUE(info.cycle_state);

    const size_t last_trb = ring.size() - xhci::kTrbBytes;
    EXPECT_EQ(0x00200000u, read32(ring, last_trb + 0u));
    EXPECT_EQ(0u, read32(ring, last_trb + 4u));
    EXPECT_EQ((xhci::kTrbTypeLink << 10) | 0x3u, read32(ring, last_trb + 12u));
}

TEST(XhciController, EnqueuesCommandTrbsAndWrapsProducerCycle)
{
    std::array<uint8_t, 64> ring{};
    xhci::CommandRingInfo info{};
    ASSERT_TRUE(xhci::initialize_command_ring(ring.data(), ring.size(), 0x200000u, info));

    uint64_t trb_physical = 0;
    const xhci::TrbFields no_op{
        .parameter = 0x1122334455667788ull,
        .status = 0x55aa00u,
        .control = xhci::kTrbTypeNoOpCommand << 10,
    };
    ASSERT_TRUE(
        xhci::enqueue_command_trb(ring.data(), ring.size(), 0x200000u, info, no_op, &trb_physical));
    EXPECT_EQ(0x200000u, trb_physical);
    EXPECT_EQ(1u, info.enqueue_index);
    EXPECT_TRUE(info.cycle_state);
    EXPECT_EQ(0x55667788u, read32(ring, 0u));
    EXPECT_EQ(0x11223344u, read32(ring, 4u));
    EXPECT_EQ(0x55aa00u, read32(ring, 8u));
    EXPECT_EQ((xhci::kTrbTypeNoOpCommand << 10) | xhci::kTrbCycleBit, read32(ring, 12u));

    ASSERT_TRUE(xhci::enqueue_command_trb(ring.data(), ring.size(), 0x200000u, info, no_op));
    ASSERT_TRUE(xhci::enqueue_command_trb(ring.data(), ring.size(), 0x200000u, info, no_op));
    EXPECT_EQ(0u, info.enqueue_index);
    EXPECT_FALSE(info.cycle_state);
    EXPECT_EQ((xhci::kTrbTypeLink << 10) | 0x3u, read32(ring, 60u));

    ASSERT_TRUE(xhci::enqueue_command_trb(ring.data(), ring.size(), 0x200000u, info, no_op));
    EXPECT_EQ(1u, info.enqueue_index);
    EXPECT_FALSE(info.cycle_state);
    EXPECT_EQ(xhci::kTrbTypeNoOpCommand << 10, read32(ring, 12u));
}

TEST(XhciController, InitializesEventRingSegmentTable)
{
    std::array<uint8_t, 64> erst{};
    ASSERT_TRUE(
        xhci::initialize_event_ring_segment_table(erst.data(), erst.size(), 0x345000u, 128u));
    EXPECT_EQ(0x345000u, read64(erst, 0u));
    EXPECT_EQ(128u, read32(erst, 8u));
    EXPECT_EQ(0u, read32(erst, 12u));
}

TEST(XhciController, DequeuesEventsAndTracksCycleWrap)
{
    std::array<uint8_t, 32> ring{};
    xhci::EventRingInfo info{};
    ASSERT_TRUE(xhci::initialize_event_ring_state(0x345000u, 2u, info));

    write_event_trb(ring,
                    0u,
                    0x200040u,
                    1u << 24,
                    (xhci::kTrbTypeCommandCompletionEvent << 10) | xhci::kTrbCycleBit | (5u << 24));
    write_event_trb(ring,
                    16u,
                    static_cast<uint64_t>(3u) << 24,
                    1u << 24,
                    (xhci::kTrbTypePortStatusChangeEvent << 10) | xhci::kTrbCycleBit);

    xhci::EventInfo event{};
    ASSERT_TRUE(xhci::dequeue_event_trb(ring.data(), ring.size(), 0x345000u, info, event));
    EXPECT_EQ(xhci::EventType::CommandCompletion, event.type);
    EXPECT_EQ(1u, event.completion_code);
    EXPECT_EQ(5u, event.slot_id);
    EXPECT_EQ(0x345010u, info.dequeue_physical);

    ASSERT_TRUE(xhci::dequeue_event_trb(ring.data(), ring.size(), 0x345000u, info, event));
    EXPECT_EQ(xhci::EventType::PortStatusChange, event.type);
    EXPECT_EQ(3u, event.port_id);
    EXPECT_EQ(0x345000u, info.dequeue_physical);
    EXPECT_FALSE(info.cycle_state);

    write_event_trb(ring,
                    0u,
                    0x200080u,
                    (8u << 24) | 64u,
                    (xhci::kTrbTypeTransferEvent << 10) | (2u << 16) | (7u << 24));
    ASSERT_TRUE(xhci::dequeue_event_trb(ring.data(), ring.size(), 0x345000u, info, event));
    EXPECT_EQ(xhci::EventType::Transfer, event.type);
    EXPECT_EQ(8u, event.completion_code);
    EXPECT_EQ(64u, event.transfer_length);
    EXPECT_EQ(2u, event.endpoint_id);
    EXPECT_EQ(7u, event.slot_id);
}

TEST(XhciController, ControllerResetStopsAndClearsReset)
{
    std::array<uint8_t, 0x400> mmio{};
    populate_capability_block(mmio);

    xhci::ControllerLayout layout{};
    ASSERT_TRUE(xhci::parse_capability(mmio.data(), mmio.size(), layout));
    write32(mmio, layout.capability.cap_length + xhci::kOperationalUsbcmd, xhci::kUsbcmdRunStop);
    write32(mmio,
            layout.capability.cap_length + xhci::kOperationalUsbsts,
            xhci::kUsbstsControllerNotReady);

    ResetHookState hook_state{};
    ASSERT_TRUE(xhci::controller_reset(layout, 8u, emulate_reset, &hook_state));
    EXPECT_TRUE(hook_state.halted_reported);
    EXPECT_TRUE(hook_state.reset_cleared);
    EXPECT_EQ(xhci::kUsbstsHalted,
              read32(mmio, layout.capability.cap_length + xhci::kOperationalUsbsts));
}

TEST(XhciController, ProgramsOperationalAndInterrupterRegisters)
{
    std::array<uint8_t, 0x400> mmio{};
    populate_capability_block(mmio);

    xhci::ControllerLayout layout{};
    ASSERT_TRUE(xhci::parse_capability(mmio.data(), mmio.size(), layout));

    const xhci::RingSet rings{
        .dcbaa_physical = 0x120000u,
        .command_ring_physical = 0x124000u,
        .event_ring_physical = 0x128000u,
        .erst_physical = 0x12c000u,
        .command_ring_cycle_state = true,
        .event_ring_trb_count = 256u,
    };
    ASSERT_TRUE(xhci::controller_program(layout, rings, 8u));

    EXPECT_EQ(0x124001u, read64(mmio, layout.capability.cap_length + xhci::kOperationalCrcr));
    EXPECT_EQ(0x120000u, read64(mmio, layout.capability.cap_length + xhci::kOperationalDcbaap));
    EXPECT_EQ(8u, read32(mmio, layout.capability.cap_length + xhci::kOperationalConfig));

    const size_t interrupter = layout.capability.runtime_offset + xhci::kRuntimeInterrupter0;
    EXPECT_EQ(0u, read32(mmio, interrupter + xhci::kInterrupterImod));
    EXPECT_EQ(1u, read32(mmio, interrupter + xhci::kInterrupterErstsz));
    EXPECT_EQ(0x12c000u, read64(mmio, interrupter + xhci::kInterrupterErstba));
    EXPECT_EQ(0x128000u, read64(mmio, interrupter + xhci::kInterrupterErdp));
    EXPECT_EQ(xhci::kImanInterruptEnable, read32(mmio, interrupter + xhci::kInterrupterIman));
}

TEST(XhciController, ReadsPortStatusAndPowersPort)
{
    std::array<uint8_t, 0x800> mmio{};
    populate_capability_block(mmio);

    xhci::ControllerLayout layout{};
    ASSERT_TRUE(xhci::parse_capability(mmio.data(), mmio.size(), layout));

    const size_t port2 = layout.capability.cap_length + xhci::kOperationalPortRegisterBase +
                         xhci::kPortRegisterStride;
    write32(mmio,
            port2,
            xhci::kPortScCurrentConnectStatus | xhci::kPortScPortEnabled | (3u << 10) |
                xhci::kPortScConnectStatusChange);

    xhci::PortStatus status{};
    ASSERT_TRUE(xhci::read_port_status(layout, 2u, status));
    EXPECT_TRUE(status.connected);
    EXPECT_TRUE(status.enabled);
    EXPECT_FALSE(status.powered);
    EXPECT_EQ(3u, status.speed);
    EXPECT_TRUE(status.has_changes);

    ASSERT_TRUE(xhci::power_on_port(layout, 2u));
    EXPECT_TRUE(0u != (read32(mmio, port2) & xhci::kPortScPortPower));
}
TEST(XhciController, SlotContextEntriesBelongToDwordZero)
{
    uint32_t slot[8]{};
    xhci::set_slot_context(slot, 4, 3, 5);
    EXPECT_EQ(slot[0], (4u << 20) | (5u << 27));
    EXPECT_EQ(slot[1], 3u << 16);
    EXPECT_EQ(slot[2], 0u);
}

TEST(XhciController, PortWritesDoNotDisableOrResetAnEnabledPort)
{
    std::array<uint8_t, 0x1000> mmio{};
    populate_capability_block(mmio);
    xhci::ControllerLayout layout{};
    ASSERT_TRUE(xhci::parse_capability(mmio.data(), mmio.size(), layout));
    const size_t port = layout.capability.cap_length + xhci::kOperationalPortRegisterBase;
    const uint32_t raw = 0xFFFFFFFFu;
    write32(mmio, port, raw);
    ASSERT_TRUE(xhci::clear_port_change_bits(layout, 1, xhci::kPortScConnectStatusChange));
    EXPECT_EQ(xhci::kPortScPreservedBits | xhci::kPortScConnectStatusChange, read32(mmio, port));
    write32(mmio, port, raw);
    ASSERT_TRUE(xhci::power_on_port(layout, 1));
    EXPECT_EQ(xhci::kPortScPreservedBits, read32(mmio, port));
}

TEST(XhciController, QuiesceMasksControllerAndInterrupterSources)
{
    std::array<uint8_t, 0x1000> mmio{};
    populate_capability_block(mmio);
    xhci::ControllerLayout layout{};
    ASSERT_TRUE(xhci::parse_capability(mmio.data(), mmio.size(), layout));
    write32(mmio,
            0x40 + xhci::kOperationalUsbcmd,
            xhci::kUsbcmdRunStop | xhci::kUsbcmdInterrupterEnable);
    write32(mmio,
            0x200 + xhci::kRuntimeInterrupter0,
            xhci::kImanInterruptEnable | xhci::kImanInterruptPending);
    xhci::disable_interrupts(layout);
    EXPECT_EQ(xhci::kUsbcmdRunStop, read32(mmio, 0x40 + xhci::kOperationalUsbcmd));
    EXPECT_EQ(xhci::kImanInterruptPending, read32(mmio, 0x200 + xhci::kRuntimeInterrupter0));
}
