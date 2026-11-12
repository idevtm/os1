#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

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
    uint32_t reset_delay_us = 0;
    volatile uint8_t* operational = nullptr;
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
        EXPECT_EQ(1000u, state.reset_delay_us);
        *command = static_cast<uint32_t>(*command & ~xhci::kUsbcmdHostControllerReset);
        *status = xhci::kUsbstsHalted;
        state.reset_cleared = true;
    }
}
void record_reset_delay(uint32_t microseconds, void* context)
{
    auto& state = *static_cast<ResetHookState*>(context);
    const auto* command =
        reinterpret_cast<volatile const uint32_t*>(state.operational + xhci::kOperationalUsbcmd);
    EXPECT_NE(0u, *command & xhci::kUsbcmdHostControllerReset);
    EXPECT_FALSE(state.reset_cleared);
    state.reset_delay_us = microseconds;
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
    alignas(uint32_t) std::array<uint8_t, 32> ring{};
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

TEST(XhciController, EventControlWordRequiresNaturalAlignment)
{
    alignas(uint32_t) std::array<uint8_t, 32> ring{};
    xhci::EventRingInfo info{};
    ASSERT_TRUE(xhci::initialize_event_ring_state(0x345000u, 1u, info));
    write_event_trb(ring,
                    1u,
                    0x200040u,
                    1u << 24,
                    (xhci::kTrbTypeCommandCompletionEvent << 10) | xhci::kTrbCycleBit);

    xhci::EventInfo event{};
    EXPECT_FALSE(xhci::dequeue_event_trb(ring.data() + 1, 16u, 0x345000u, info, event));
    EXPECT_EQ(0u, info.dequeue_index);
    EXPECT_TRUE(info.cycle_state);
    EXPECT_EQ(0u, event.parameter);
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

    ResetHookState hook_state{.operational = layout.operational};
    ASSERT_TRUE(xhci::controller_reset(layout, 8u, emulate_reset, &hook_state, record_reset_delay));
    EXPECT_TRUE(hook_state.halted_reported);
    EXPECT_TRUE(hook_state.reset_cleared);
    EXPECT_EQ(xhci::kUsbstsHalted,
              read32(mmio, layout.capability.cap_length + xhci::kOperationalUsbsts));
}

TEST(XhciController, RejectsRingsOutside32BitDmaWithoutProgrammingRegisters)
{
    std::array<uint8_t, 0x400> mmio{};
    populate_capability_block(mmio);
    xhci::ControllerLayout layout{};
    ASSERT_TRUE(xhci::parse_capability(mmio.data(), mmio.size(), layout));

    xhci::RingSet rings{
        .dcbaa_physical = 0x120000u,
        .command_ring_physical = 0x124000u,
        .command_ring_bytes = 4096u,
        .event_ring_physical = 0xffffffc0u,
        .erst_physical = 0x12c000u,
        .command_ring_cycle_state = true,
        .event_ring_trb_count = 256u,
    };
    EXPECT_FALSE(xhci::controller_program(layout, rings, 8u));
    EXPECT_EQ(0u, read64(mmio, layout.capability.cap_length + xhci::kOperationalCrcr));
    EXPECT_EQ(0u, read64(mmio, layout.capability.cap_length + xhci::kOperationalDcbaap));
    EXPECT_FALSE(xhci::program_command_ring(layout, 0x1'0000'0000ull, 4096u, true));
    EXPECT_FALSE(xhci::request_command_abort(layout, 0x1'0000'0000ull, 4096u));
    rings.event_ring_physical = 0x128000u;
    rings.command_ring_physical = 0xffffffc0u;
    EXPECT_FALSE(xhci::controller_program(layout, rings, 8u));
    EXPECT_FALSE(xhci::program_command_ring(layout, rings.command_ring_physical, 4096u, true));
    EXPECT_FALSE(xhci::request_command_abort(layout, rings.command_ring_physical, 4096u));
    EXPECT_EQ(0u, read64(mmio, layout.capability.cap_length + xhci::kOperationalCrcr));
    EXPECT_FALSE(layout.capability.address_64);
    EXPECT_TRUE(xhci::dma_range_supported(layout.capability, UINT32_MAX, 1));
    EXPECT_FALSE(xhci::dma_range_supported(layout.capability, UINT32_MAX, 2));

    write32(mmio, 0x10u, 1u);
    ASSERT_TRUE(xhci::parse_capability(mmio.data(), mmio.size(), layout));
    EXPECT_TRUE(layout.capability.address_64);
    rings.command_ring_physical = 0x1'0000'0040ull;
    rings.event_ring_physical = 0xffffffc0u;
    ASSERT_TRUE(xhci::controller_program(layout, rings, 8u));
    EXPECT_EQ(0x1'0000'0041ull,
              read64(mmio, layout.capability.cap_length + xhci::kOperationalCrcr));
    EXPECT_FALSE(xhci::dma_range_supported(layout.capability, UINT64_MAX, 2));
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
        .command_ring_bytes = 4096u,
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

TEST(XhciDiagnostics, LegacySnapshotIsReadOnlyAndFollowsForwardCapabilities)
{
    alignas(4) std::array<uint8_t, 256> bytes{};
    write32(bytes, 0x10, 0x10u << 16);
    write32(bytes, 0x40, 2u | (4u << 8));
    write32(bytes, 0x50, 1u | (1u << 16) | (1u << 24));
    write32(bytes, 0x54, 0x12345678);
    const auto before = bytes;
    xhci::ControllerLayout layout{};
    layout.mmio_base = bytes.data();
    layout.mmio_size = bytes.size();
    xhci::LegacySupport snapshot{};
    ASSERT_EQ(xhci::read_legacy_support(layout, snapshot), xhci::LegacySupportResult::Present);
    EXPECT_EQ(snapshot.offset, 0x50u);
    EXPECT_EQ(snapshot.support, 0x01010001u);
    EXPECT_EQ(snapshot.control, 0x12345678u);
    EXPECT_EQ(bytes, before);
}

TEST(XhciDiagnostics, MissingAndMalformedLegacyTablesAreDistinct)
{
    alignas(4) std::array<uint8_t, 256> bytes{};
    xhci::ControllerLayout layout{};
    layout.mmio_base = bytes.data();
    layout.mmio_size = bytes.size();
    xhci::LegacySupport snapshot{};
    EXPECT_EQ(xhci::read_legacy_support(layout, snapshot), xhci::LegacySupportResult::Absent);
    for(const uint32_t offset : {0x10u, 0x100u, 0x3fffcu})
    {
        write32(bytes, 0x10, (offset / 4u) << 16);
        EXPECT_EQ(xhci::read_legacy_support(layout, snapshot), xhci::LegacySupportResult::Invalid);
    }
    write32(bytes, 0x10, 0x3fu << 16);
    write32(bytes, 0xfc, 1u);  // Missing legacy control register.
    EXPECT_EQ(xhci::read_legacy_support(layout, snapshot), xhci::LegacySupportResult::Invalid);
    write32(bytes, 0xfc, 2u | (1u << 8));  // Next capability outside the BAR.
    EXPECT_EQ(xhci::read_legacy_support(layout, snapshot), xhci::LegacySupportResult::Invalid);
    write32(bytes, 0xfc, 2u);  // A terminating unrelated capability is valid.
    EXPECT_EQ(xhci::read_legacy_support(layout, snapshot), xhci::LegacySupportResult::Absent);
    layout.mmio_size = 0x10;
    EXPECT_EQ(xhci::read_legacy_support(layout, snapshot), xhci::LegacySupportResult::Invalid);
    layout.mmio_base = nullptr;
    EXPECT_EQ(xhci::read_legacy_support(layout, snapshot), xhci::LegacySupportResult::Invalid);
}

TEST(XhciDiagnostics, LegacyTraversalHasAFixedReadBudget)
{
    alignas(4) std::array<uint8_t, 2048> bytes{};
    write32(bytes, 0x10, 0x10u << 16);
    for(size_t offset = 0x40; offset < bytes.size(); offset += 4)
        write32(bytes, offset, 2u | (1u << 8));
    xhci::ControllerLayout layout{};
    layout.mmio_base = bytes.data();
    layout.mmio_size = bytes.size();
    xhci::LegacySupport snapshot{};
    EXPECT_EQ(xhci::read_legacy_support(layout, snapshot), xhci::LegacySupportResult::Invalid);
    EXPECT_EQ(snapshot.offset, 0u);
}

namespace
{
struct HandoffFixture
{
    alignas(4) std::array<uint8_t, 256> bytes{};
    unsigned waits = 0;
    bool release = true;
    xhci::ControllerLayout layout()
    {
        xhci::ControllerLayout value{};
        value.mmio_base = bytes.data();
        value.mmio_size = bytes.size();
        return value;
    }
    static void delay(uint32_t us, void* context)
    {
        auto& f = *static_cast<HandoffFixture*>(context);
        EXPECT_EQ(us, 1000u);
        EXPECT_NE(read32(f.bytes, 0x40) & (1u << 24), 0u);
        if(++f.waits == 2 && f.release)
            write32(f.bytes, 0x40, read32(f.bytes, 0x40) & ~(1u << 16));
    }
    HandoffFixture()
    {
        write32(bytes, 0x10, 0x10u << 16);
        write32(bytes, 0x40, 0x10001u);
        write32(bytes, 0x44, 0xc00fffffu);
    }
};
struct RoutingFixture
{
    std::array<uint32_t, 4> registers{0, 0xfff, 0, 3};
    std::array<uint16_t, 8> writes{};
    unsigned write_count = 0, read_count = 0;
    uint16_t reject = 0;
    static uint32_t read(void* context, uint16_t offset)
    {
        auto& f = *static_cast<RoutingFixture*>(context);
        ++f.read_count;
        EXPECT_GE(offset, 0xd0);
        EXPECT_LE(offset, 0xdc);
        return f.registers[(offset - 0xd0) / 4];
    }
    static void write(void* context, uint16_t offset, uint32_t value)
    {
        auto& f = *static_cast<RoutingFixture*>(context);
        EXPECT_TRUE(offset == 0xd0 || offset == 0xd8);
        if(f.write_count < f.writes.size())
            f.writes[f.write_count++] = offset;
        if(offset != f.reject)
            f.registers[(offset - 0xd0) / 4] = value;
    }
    xhci::RoutingIo io()
    {
        return {this, read, write};
    }
};
}  // namespace

TEST(XhciHandoff, RequestsOwnershipBeforePollingAndDisablesSmiAfterRelease)
{
    HandoffFixture f;
    ASSERT_TRUE(xhci::acquire_legacy_ownership(f.layout(), f.delay, &f));
    EXPECT_EQ(f.waits, 2u);
    EXPECT_EQ(read32(f.bytes, 0x40), 0x01000001u);
    // Preserve reserved fields; write ones to W1C status bits; clear SMI enables.
    EXPECT_EQ(read32(f.bytes, 0x44), 0xe00f1feeu);
}

TEST(XhciHandoff, PendingReadOnlyStatusDoesNotRejectOwnership)
{
    for(const uint32_t status : {1u << 16, 1u << 20, (1u << 16) | (1u << 20)})
    {
        HandoffFixture f;
        write32(f.bytes, 0x44, status | 0xe011u);
        ASSERT_TRUE(xhci::acquire_legacy_ownership(f.layout(), f.delay, &f));
        // RMW retains these RO bits in the fixture, as hardware does on writes.
        EXPECT_EQ(read32(f.bytes, 0x44) & status, status);
        EXPECT_EQ(read32(f.bytes, 0x44) & 0xe011u, 0u);
    }
}

TEST(XhciHandoff, TimeoutNeverForcesBiosReleaseOrChangesSmiControl)
{
    HandoffFixture f;
    f.release = false;
    ASSERT_FALSE(xhci::acquire_legacy_ownership(f.layout(), f.delay, &f));
    EXPECT_EQ(f.waits, 1000u);
    EXPECT_EQ(read32(f.bytes, 0x40), 0x01010001u);
    EXPECT_EQ(read32(f.bytes, 0x44), 0xc00fffffu);
}

TEST(XhciHandoff, AlreadyReleasedAndAbsentCapabilitiesDoNotWait)
{
    HandoffFixture f;
    write32(f.bytes, 0x40, 1u);
    ASSERT_TRUE(xhci::acquire_legacy_ownership(f.layout(), f.delay, &f));
    EXPECT_EQ(f.waits, 0u);
    EXPECT_EQ(read32(f.bytes, 0x40), 0x01000001u);
    write32(f.bytes, 0x10, 0u);
    const auto before = f.bytes;
    ASSERT_TRUE(xhci::acquire_legacy_ownership(f.layout(), nullptr, nullptr));
    EXPECT_EQ(f.bytes, before);
}

TEST(XhciHandoff, InvalidTableAndMissingDelayHaveNoWrites)
{
    HandoffFixture f;
    auto before = f.bytes;
    EXPECT_FALSE(xhci::acquire_legacy_ownership(f.layout(), nullptr, nullptr));
    EXPECT_EQ(f.bytes, before);
    write32(f.bytes, 0x10, 0xffff0000u);
    before = f.bytes;
    EXPECT_FALSE(xhci::acquire_legacy_ownership(f.layout(), f.delay, &f));
    EXPECT_EQ(f.bytes, before);
}

TEST(XhciRouting, EnablesSuperSpeedBeforeUsb2AndPreservesOtherBits)
{
    RoutingFixture f;
    f.registers[0] = 0x80000000u;
    f.registers[2] = 0x40000000u;
    EXPECT_EQ(xhci::route_intel_ports(0x8086, 0x8c31, f.io()), xhci::RoutingResult::Changed);
    EXPECT_EQ(f.write_count, 2u);
    EXPECT_EQ(f.writes[0], 0xd8);
    EXPECT_EQ(f.writes[1], 0xd0);
    EXPECT_EQ(f.registers[0], 0x80000fffu);
    EXPECT_EQ(f.registers[2], 0x40000003u);
    EXPECT_EQ(xhci::route_intel_ports(0x8086, 0x8c31, f.io()), xhci::RoutingResult::Unchanged);
    EXPECT_EQ(f.write_count, 2u);
}

TEST(XhciRouting, OtherDevicesAndZeroMasksDoNotWrite)
{
    RoutingFixture f;
    EXPECT_EQ(xhci::route_intel_ports(0x1b36, 0x000d, f.io()), xhci::RoutingResult::NotApplicable);
    EXPECT_EQ(xhci::route_intel_ports(0x8086, 0xffff, f.io()), xhci::RoutingResult::NotApplicable);
    EXPECT_EQ(f.read_count, 0u);
    EXPECT_EQ(f.write_count, 0u);
    f.registers = {};
    EXPECT_EQ(xhci::route_intel_ports(0x8086, 0x8c31, f.io()), xhci::RoutingResult::Unchanged);
    EXPECT_EQ(f.write_count, 0u);
}

TEST(XhciRouting, InaccessibleRegistersNeverWrite)
{
    for(unsigned index = 0; index < 4; ++index)
    {
        RoutingFixture f;
        f.registers[index] = 0xffffffffu;
        EXPECT_EQ(xhci::route_intel_ports(0x8086, 0x8c31, f.io()), xhci::RoutingResult::Failed);
        EXPECT_EQ(f.write_count, 0u);
    }
}

TEST(XhciRouting, FailedReadbackRestoresOriginalRoutesBeforeReturning)
{
    for(uint16_t reg : {0xd0, 0xd8})
    {
        RoutingFixture f;
        f.reject = reg;
        const auto before = f.registers;
        EXPECT_EQ(xhci::route_intel_ports(0x8086, 0x8c31, f.io()), xhci::RoutingResult::Failed);
        EXPECT_EQ(f.registers, before);
        EXPECT_EQ(f.writes[f.write_count - 2], 0xd0);
        EXPECT_EQ(f.writes[f.write_count - 1], 0xd8);
    }
}

namespace
{
struct ControlFixture
{
    alignas(64) std::array<uint8_t, 128> bytes{};
    xhci::CommandRingInfo ring{};
    xhci::ControlTransfer transfer{};
    ControlFixture()
    {
        EXPECT_TRUE(xhci::initialize_command_ring(bytes.data(), bytes.size(), 0x1000, ring));
    }
    bool queue(uint16_t length = 18, bool in = true)
    {
        const uint64_t setup = (uint64_t(length) << 48) | 0x01000600u | (in ? 0x80u : 0u);
        return xhci::queue_control_transfer(
            bytes.data(), bytes.size(), 0x1000, ring, setup, 0x2000, transfer);
    }
    uint32_t control(unsigned stage)
    {
        return read32(bytes, transfer.stages[stage] - 0x1000 + 12);
    }
    bool event(unsigned stage, uint8_t cc, uint32_t residual = 0)
    {
        xhci::EventInfo event{};
        event.parameter = transfer.stages[stage];
        event.completion_code = cc;
        event.transfer_length = residual;
        return xhci::control_transfer_event(transfer, event);
    }
};
}  // namespace

TEST(XhciControl, InRequestHasSeparateStagesAndPublishesSetupLast)
{
    ControlFixture f;
    ASSERT_TRUE(f.queue());
    EXPECT_EQ(f.control(0), (2u << 10) | (1u << 6) | (3u << 16));  // Cycle initially withheld.
    EXPECT_EQ(f.control(1), (3u << 10) | (1u << 16) | (1u << 2) | 1u);
    EXPECT_EQ(f.control(2), (4u << 10) | (1u << 5) | 1u);
    EXPECT_EQ(read64(f.bytes, 0), (18ull << 48) | 0x01000680u);
    EXPECT_EQ(read32(f.bytes, 8), 8u);
    EXPECT_EQ(read64(f.bytes, 16), 0x2000u);
    EXPECT_EQ(read32(f.bytes, 24), 18u);
    xhci::publish_control_transfer(f.bytes.data(), 0x1000, f.transfer);
    EXPECT_EQ(f.control(0) & 1u, 1u);
    EXPECT_TRUE(f.event(2, 1));
    EXPECT_TRUE(f.transfer.done);
    EXPECT_EQ(f.transfer.completion, 1u);
    EXPECT_EQ(f.transfer.residual, 0u);
}

TEST(XhciControl, OutAndNoDataHaveInStatusWithoutShortPacketInterrupt)
{
    for(uint16_t length : {0u, 8u})
    {
        ControlFixture f;
        ASSERT_TRUE(f.queue(length, false));
        EXPECT_EQ((f.control(0) >> 16) & 3u, length ? 2u : 0u);
        EXPECT_EQ(f.control(2), (4u << 10) | (1u << 16) | (1u << 5) | 1u);
        if(length)
            EXPECT_EQ(f.control(1), (3u << 10) | 1u);
        else
            EXPECT_EQ(f.transfer.stages[1], 0u);
        EXPECT_TRUE(f.event(2, 1));
        EXPECT_TRUE(f.transfer.done);
    }
}

TEST(XhciControl, ShortDataWaitsForStatusAndRetainsActualLength)
{
    for(uint32_t residual : {0u, 10u, 18u})
    {
        ControlFixture f;
        ASSERT_TRUE(f.queue());
        ASSERT_TRUE(f.event(1, 13, residual));
        EXPECT_FALSE(f.transfer.done);
        ASSERT_TRUE(f.event(2, 1));
        EXPECT_TRUE(f.transfer.done);
        EXPECT_EQ(f.transfer.completion, 1u);
        EXPECT_EQ(f.transfer.requested - f.transfer.residual, 18u - residual);
        EXPECT_FALSE(f.event(1, 13, 0));  // Late event cannot overwrite a retired request.
        EXPECT_EQ(f.transfer.residual, residual);
    }
}

TEST(XhciControl, ErrorsAtEveryStageCompleteWithoutWaitingForStatus)
{
    for(unsigned stage = 0; stage < 3; ++stage)
        for(uint8_t cc : {4u, 5u, 6u})
        {
            ControlFixture f;
            ASSERT_TRUE(f.queue());
            EXPECT_TRUE(f.event(stage, cc));
            EXPECT_TRUE(f.transfer.done);
            EXPECT_EQ(f.transfer.completion, cc);
            EXPECT_EQ(f.transfer.residual, 18u);
            EXPECT_FALSE(f.event(2, 1));
        }
}

TEST(XhciControl, MalformedCompletionCannotBecomeSuccessfulData)
{
    for(unsigned stage = 0; stage < 3; ++stage)
    {
        ControlFixture f;
        ASSERT_TRUE(f.queue());
        ASSERT_TRUE(f.event(stage, 13, 19));
        EXPECT_TRUE(f.transfer.done);
        EXPECT_EQ(f.transfer.completion, 5u);
        EXPECT_EQ(f.transfer.residual, 18u);
    }
    ControlFixture f;
    ASSERT_TRUE(f.queue());
    ASSERT_TRUE(f.event(2, 13, 0));  // Captured Intel result must not imply 18 valid bytes.
    EXPECT_EQ(f.transfer.completion, 5u);
    EXPECT_EQ(f.transfer.residual, 18u);
    ASSERT_TRUE(f.queue());
    ASSERT_TRUE(f.event(0, 1));  // Setup has no IOC; success here cannot finish the request.
    EXPECT_EQ(f.transfer.completion, 5u);
    ASSERT_TRUE(f.queue(18, false));
    ASSERT_TRUE(f.event(1, 13, 1));
    EXPECT_EQ(f.transfer.completion, 5u);
    ASSERT_TRUE(f.queue());
    ASSERT_TRUE(f.event(2, 1, 1));
    EXPECT_EQ(f.transfer.completion, 5u);
}

TEST(XhciControl, UnrelatedEventCannotCompleteCurrentRequest)
{
    ControlFixture f;
    ASSERT_TRUE(f.queue());
    xhci::EventInfo event{};
    event.parameter = 0x7770;
    event.completion_code = 1;
    EXPECT_FALSE(xhci::control_transfer_event(f.transfer, event));
    EXPECT_FALSE(f.transfer.done);
    event.parameter = 0;
    EXPECT_FALSE(xhci::control_transfer_event(f.transfer, event));
}

TEST(XhciControl, RingWrapKeepsStageAddressesCyclesAndLinkBoundaries)
{
    for(uint32_t start : {5u, 6u})
    {
        ControlFixture f;
        f.ring.enqueue_index = start;
        ASSERT_TRUE(f.queue());
        EXPECT_EQ(f.transfer.stages[0], 0x1000u + start * 16u);
        EXPECT_EQ(f.transfer.stages[1], 0x1000u + ((start + 1) % 7) * 16u);
        EXPECT_EQ(f.transfer.stages[2], 0x1000u + ((start + 2) % 7) * 16u);
        EXPECT_EQ(f.control(0) & 1u, 0u);
        EXPECT_EQ(f.control(2) & 1u, 0u);  // Wrapped producer cycle.
        EXPECT_EQ(read32(f.bytes, 7 * 16 + 12) & (1u << 4), 0u);
        xhci::publish_control_transfer(f.bytes.data(), 0x1000, f.transfer);
        EXPECT_EQ(f.control(0) & 1u, 1u);
        EXPECT_TRUE(f.event(1, 13, 10));
        EXPECT_TRUE(f.event(2, 1));
        EXPECT_EQ(f.transfer.residual, 10u);
    }
}

TEST(XhciControl, InvalidQueueInputsLeaveRingUnchanged)
{
    ControlFixture f;
    const auto before = f.bytes;
    for(uint64_t buffer : {0ull, 0xfff8ull})
        EXPECT_FALSE(xhci::queue_control_transfer(
            f.bytes.data(), f.bytes.size(), 0x1000, f.ring, 18ull << 48, buffer, f.transfer));
    EXPECT_FALSE(f.queue(4097));
    EXPECT_EQ(f.bytes, before);
    EXPECT_EQ(f.ring.enqueue_index, 0u);
}

TEST(XhciControl, RejectsTruncatedAndMismatchedDescriptorHeaders)
{
    uint8_t descriptor[18]{18, 1};
    EXPECT_TRUE(xhci::valid_descriptor_header(descriptor, 18, 1, 18));
    EXPECT_FALSE(xhci::valid_descriptor_header(descriptor, 8, 1, 18));
    EXPECT_FALSE(xhci::valid_descriptor_header(descriptor, 18, 2, 9));
    descriptor[0] = 0;
    EXPECT_FALSE(xhci::valid_descriptor_header(descriptor, 18, 1, 18));
    descriptor[0] = 19;
    EXPECT_FALSE(xhci::valid_descriptor_header(descriptor, 18, 1, 18));
    EXPECT_FALSE(xhci::valid_descriptor_header(nullptr, 18, 1, 18));
}

TEST(XhciControl, ConfigurationMustContainCompleteBoundedDescriptors)
{
    // A configuration, HID interface, and interrupt endpoint.
    std::array<uint8_t, 25> bytes{9, 2, 25, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0,
                                  1, 3, 1,  1, 0, 7, 5, 0x81, 3,  8, 0, 10};
    EXPECT_TRUE(xhci::valid_configuration_descriptors(bytes.data(), bytes.size()));
    EXPECT_FALSE(xhci::valid_configuration_descriptors(bytes.data(), 18));
    for(size_t offset : {0u, 9u, 18u})
    {
        auto corrupt = bytes;
        corrupt[offset] = 2;  // Header exists but required fields are missing.
        EXPECT_FALSE(xhci::valid_configuration_descriptors(corrupt.data(), corrupt.size()));
        corrupt[offset] = 0;
        EXPECT_FALSE(xhci::valid_configuration_descriptors(corrupt.data(), corrupt.size()));
    }
    bytes[18] = 8;
    EXPECT_FALSE(xhci::valid_configuration_descriptors(bytes.data(), bytes.size()));
}

TEST(XhciController, NegotiatesFullSpeedReceiverControlPacketBeforeLongDescriptor)
{
    uint8_t descriptor[8] = {18, 1, 0, 2, 0, 0, 0, 32};  // Logitech 046d:c539 prefix.
    uint16_t packet = 0;
    ASSERT_TRUE(xhci::device_control_packet(1, descriptor, sizeof(descriptor), packet));
    EXPECT_EQ(packet, 32);
    for(unsigned value : {8, 16, 32, 64})
    {
        descriptor[7] = value;
        EXPECT_TRUE(xhci::device_control_packet(1, descriptor, 8, packet));
        EXPECT_EQ(packet, value);
    }
    descriptor[7] = 32;
    EXPECT_FALSE(xhci::device_control_packet(2, descriptor, 8, packet));
    EXPECT_FALSE(xhci::device_control_packet(3, descriptor, 8, packet));
    EXPECT_FALSE(xhci::device_control_packet(1, descriptor, 7, packet));
    descriptor[7] = 9;
    EXPECT_TRUE(xhci::device_control_packet(4, descriptor, 8, packet));
    EXPECT_EQ(packet, 512);
    EXPECT_FALSE(xhci::device_control_packet(1, descriptor, 8, packet));
    descriptor[1] = 2;
    EXPECT_FALSE(xhci::device_control_packet(4, descriptor, 8, packet));
}

TEST(XhciController, EvaluateContextChangesOnlyPacketSizeAndPreservesEp0Dequeue)
{
    for(unsigned width : {32, 64})
    {
        std::array<uint8_t, 192> input{};
        std::array<uint8_t, 128> output{};
        for(unsigned i = 0; i < width; ++i)
            output[width + i] = i + 1;
        input.fill(0xff);
        ASSERT_TRUE(xhci::prepare_control_packet_context(
            input.data(), input.size(), output.data(), output.size(), width, 32));
        EXPECT_EQ(read32(input, 0), 0);
        EXPECT_EQ(read32(input, 4), 2);
        for(unsigned i = 0; i < width; ++i)
        {
            unsigned expected = i == 6 ? 32 : i == 7 ? 0 : i + 1;
            EXPECT_EQ(input[width * 2 + i], expected);
            EXPECT_EQ(input[width + i], 0);
        }
        EXPECT_FALSE(xhci::prepare_control_packet_context(
            input.data(), width * 2, output.data(), output.size(), width, 32));
    }
}

TEST(XhciController, ConvertsUsbInterruptIntervalsToMicroframeExponents)
{
    EXPECT_EQ(xhci::interrupt_interval(1, 1), 3);   // Full speed, 1 ms.
    EXPECT_EQ(xhci::interrupt_interval(2, 10), 6);  // Low speed, 8 ms <= 10 ms.
    EXPECT_EQ(xhci::interrupt_interval(1, 255), 10);
    EXPECT_EQ(xhci::interrupt_interval(3, 4), 3);  // High speed already logarithmic.
}

TEST(XhciController, HaltedHidCompletionQueuesOneWorkerRecovery)
{
    for(uint8_t completion : {3, 4, 6})
    {
        xhci::EventInfo event{};
        event.slot_id = 5;
        event.endpoint_id = 3;
        event.parameter = 0x4000;
        event.completion_code = completion;
        EXPECT_FALSE(xhci::matches_hid_transfer(event, 4, 3, 0x4000, true));
        EXPECT_FALSE(xhci::matches_hid_transfer(event, 5, 2, 0x4000, true));
        EXPECT_FALSE(xhci::matches_hid_transfer(event, 5, 3, 0x4010, true));
        EXPECT_FALSE(xhci::matches_hid_transfer(event, 5, 3, 0x4000, false));
        ASSERT_TRUE(xhci::matches_hid_transfer(event, 5, 3, 0x4000, true));
        xhci::HidRecovery recovery{};
        EXPECT_EQ(recovery.complete(event.completion_code), xhci::HidCompletion::Recover);
        EXPECT_EQ(recovery.pending, completion);
        recovery.pending = 0;  // The worker claims the request before it submits commands.
        EXPECT_EQ(recovery.complete(completion), xhci::HidCompletion::Retire);
        EXPECT_EQ(recovery.pending, 0u);
    }
}

TEST(XhciController, HidReportRestoresRecoveryAllowance)
{
    xhci::HidRecovery recovery{};
    EXPECT_EQ(recovery.complete(4), xhci::HidCompletion::Recover);
    recovery.pending = 0;
    EXPECT_EQ(recovery.complete(13), xhci::HidCompletion::Report);
    EXPECT_EQ(recovery.complete(6), xhci::HidCompletion::Recover);
    EXPECT_EQ(recovery.pending, 6u);
    recovery.pending = 0;
    EXPECT_EQ(recovery.complete(1), xhci::HidCompletion::Report);
    EXPECT_EQ(recovery.complete(2), xhci::HidCompletion::Rearm);
    EXPECT_EQ(recovery.pending, 0u);
}

TEST(XhciController, HidRestartUsesEventBeforeOutputContextState)
{
    constexpr uint8_t endpoint = 3;
    std::vector<int> steps;
    auto reset = [&](uint8_t dci) {
        steps.push_back(dci);
        return uint8_t(1);
    };
    auto clear = [&] {
        steps.push_back(20);
        return xhci::HidClearHalt::Cleared;
    };
    auto set_dequeue = [&](uint8_t dci) {
        steps.push_back(30 + dci);
        return true;
    };
    auto running = [&](uint8_t) {
        ADD_FAILURE() << "Output state must not bypass a halt event";
        return true;
    };
    EXPECT_EQ(xhci::restart_hid_transfer(4, endpoint, reset, clear, set_dequeue, running),
              xhci::HidRestart::Recovered);
    EXPECT_EQ(steps, (std::vector<int>{3, 20, 33}));
}

TEST(XhciController, HidClearHaltProtocolStallRestoresEp0AndRetiresOnlyInput)
{
    constexpr uint8_t endpoint = 3;
    std::vector<int> steps;
    auto reset = [&](uint8_t dci) {
        steps.push_back(dci);
        return uint8_t(1);
    };
    auto clear = [&] {
        steps.push_back(20);
        return xhci::HidClearHalt::ControlStall;
    };
    auto set_dequeue = [&](uint8_t dci) {
        steps.push_back(30 + dci);
        return true;
    };
    auto running = [](uint8_t) { return false; };
    EXPECT_EQ(xhci::restart_hid_transfer(6, endpoint, reset, clear, set_dequeue, running),
              xhci::HidRestart::RetireInput);
    EXPECT_EQ(steps, (std::vector<int>{3, 20, 1, 31}));
    EXPECT_EQ(xhci::restart_hid_transfer(
                  6, endpoint, reset, clear, [](uint8_t) { return false; }, running),
              xhci::HidRestart::RetireDevice);
}

TEST(XhciController, OnlyRunningTransactionErrorCanRecoverAfterResetStateError)
{
    auto reset = [](uint8_t) { return uint8_t(19); };
    auto clear = [] {
        ADD_FAILURE() << "Clear halt needs a successful reset";
        return xhci::HidClearHalt::Cleared;
    };
    auto set_dequeue = [](uint8_t) { return false; };
    auto running = [](uint8_t dci) { return dci == 3; };
    EXPECT_EQ(xhci::restart_hid_transfer(4, 3, reset, clear, set_dequeue, running),
              xhci::HidRestart::Recovered);
    EXPECT_EQ(xhci::restart_hid_transfer(6, 3, reset, clear, set_dequeue, running),
              xhci::HidRestart::RetireInput);
    EXPECT_EQ(xhci::restart_hid_transfer(4, 2, reset, clear, set_dequeue, running),
              xhci::HidRestart::RetireInput);
}

namespace
{
uint64_t fake_xhci_ticks = 0;

uint64_t read_fake_xhci_clock()
{
    return fake_xhci_ticks;
}
}  // namespace

TEST(XhciTimeout, ADelayedCommandCanOutlastTheOldPollBudget)
{
    fake_xhci_ticks = 10;
    xhci::PollDeadline deadline(read_fake_xhci_clock, 10, 3);
    xhci::CommandCompletion command{.expected_trb = 0x4000};
    for(unsigned tick = 1; tick < 9; ++tick)
    {
        ++fake_xhci_ticks;
        ASSERT_FALSE(deadline.expired());
        EXPECT_FALSE(command.complete);
    }
    ++fake_xhci_ticks;
    command.record({.parameter = 0x4000, .completion_code = 1, .slot_id = 2});
    EXPECT_FALSE(deadline.expired());
    EXPECT_TRUE(command.complete);
    EXPECT_EQ(2, command.slot);

    ++fake_xhci_ticks;
    EXPECT_TRUE(deadline.expired());
}

TEST(XhciTimeout, UnavailableOrStalledClockStillHasABound)
{
    fake_xhci_ticks = 0;
    xhci::PollDeadline stalled(read_fake_xhci_clock, 100, 3);
    xhci::PollDeadline unavailable(nullptr, 100, 3);
    for(unsigned poll = 0; poll < 2; ++poll)
    {
        EXPECT_FALSE(stalled.expired());
        EXPECT_FALSE(unavailable.expired());
    }
    EXPECT_TRUE(stalled.expired());
    EXPECT_TRUE(unavailable.expired());
}

TEST(XhciTimeout, ABackwardClockSampleDoesNotUnderflowTheDeadline)
{
    fake_xhci_ticks = 100;
    xhci::PollDeadline deadline(read_fake_xhci_clock, 50, 3);
    fake_xhci_ticks = 95;
    EXPECT_FALSE(deadline.expired());
    fake_xhci_ticks = 149;
    EXPECT_FALSE(deadline.expired());
    fake_xhci_ticks = 150;
    EXPECT_TRUE(deadline.expired());
}

TEST(XhciCommandRecovery, RequiresAStoppedRingAndKnownCommandOutcome)
{
    xhci::CommandCompletion command{.expected_trb = 0x4000};
    command.record({.parameter = 0x4010, .completion_code = 1});
    EXPECT_FALSE(command.complete);
    EXPECT_FALSE(command.can_reprogram());

    command.record({.parameter = 0x4010, .completion_code = xhci::kCompletionCommandRingStopped});
    EXPECT_FALSE(command.can_reprogram());
    command.record({.parameter = 0x4000, .completion_code = xhci::kCompletionCommandAborted});
    EXPECT_TRUE(command.can_reprogram());
    EXPECT_EQ(xhci::kCompletionCommandAborted, command.code);

    command = {.expected_trb = 0x4000};
    command.record({.parameter = 0x4000, .completion_code = xhci::kCompletionCommandRingStopped});
    EXPECT_TRUE(command.can_reprogram());
    EXPECT_FALSE(command.complete);

    command = {.expected_trb = 0x4000};
    command.record({.parameter = 0x4000, .completion_code = 1, .slot_id = 3});
    EXPECT_FALSE(command.can_reprogram());
    command.record({.parameter = 0x4010, .completion_code = xhci::kCompletionCommandRingStopped});
    EXPECT_TRUE(command.can_reprogram());
    EXPECT_EQ(1, command.code);
    EXPECT_EQ(3, command.slot);
}

TEST(XhciCommandRecovery, WritesAbortAndProgramsOnlyAStoppedRing)
{
    alignas(8) std::array<uint8_t, 64> operational{};
    xhci::ControllerLayout controller{};
    controller.operational = operational.data();
    controller.capability.address_64 = true;
    constexpr uint64_t ring = 0x1'1234'5000ull;
    const auto reg = xhci::kOperationalCrcr;
    write32(operational, reg, xhci::kCrcrCommandRingRunning);

    EXPECT_TRUE(xhci::command_ring_running(controller));
    EXPECT_FALSE(xhci::program_command_ring(controller, ring, 4096u, true));
    EXPECT_TRUE(xhci::request_command_abort(controller, ring, 4096u));
    EXPECT_EQ(ring | xhci::kCrcrCommandAbort, read64(operational, reg));

    write32(operational, reg, 0);
    EXPECT_FALSE(xhci::command_ring_running(controller));
    EXPECT_TRUE(xhci::request_command_abort(controller, ring, 4096u));
    EXPECT_EQ(ring | xhci::kCrcrCommandAbort, read64(operational, reg));
    EXPECT_FALSE(xhci::request_command_abort(controller, ring + 1, 4096u));
    EXPECT_TRUE(xhci::program_command_ring(controller, ring, 4096u, true));
    EXPECT_EQ(ring | xhci::kCrcrRingCycleState, read64(operational, reg));
}

TEST(XhciController, PortRecoveryRejectsAcknowledgedConnectionChange)
{
    xhci::PortStatus status{};
    status.connected = true;
    status.enabled = true;
    status.speed = 4;
    EXPECT_TRUE(xhci::port_reset_recovered(status, 4, false));
    EXPECT_FALSE(xhci::port_reset_recovered(status, 4, true));
    status.raw = xhci::kPortScConnectStatusChange;
    EXPECT_FALSE(xhci::port_reset_recovered(status, 4, false));
    status.raw = 0;
    EXPECT_FALSE(xhci::port_reset_recovered(status, 3, false));
    status.reset = true;
    EXPECT_FALSE(xhci::port_reset_recovered(status, 4, false));
}

TEST(XhciController, DrainsConnectionChangeArrivingDuringResetAcknowledgment)
{
    uint32_t changes = xhci::kPortScPortResetChange;
    unsigned writes = 0;
    const bool changed = xhci::drain_port_changes(
        [&](xhci::PortStatus& status) {
            status.connected = true;
            status.raw = changes;
            return true;
        },
        [&](uint32_t mask) {
            if(writes++ == 0)
                changes |= xhci::kPortScConnectStatusChange;
            changes &= ~mask;
        });

    EXPECT_TRUE(changed);
    EXPECT_EQ(0u, changes);
    EXPECT_EQ(2u, writes);
}
