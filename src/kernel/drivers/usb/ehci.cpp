// Bounded, polling EHCI storage transport. Schedules are stopped before reuse.
#include "drivers/usb/ehci.hpp"

#include "debug/debug.hpp"
#include "drivers/bus/device.hpp"
#include "drivers/bus/resource.hpp"
#include "drivers/timer/tsc_clock.hpp"
#include "drivers/usb/diagnostics.hpp"
#include "drivers/usb/ehci_controller.hpp"
#include "drivers/usb/hub.hpp"
#include "drivers/usb/mass_storage.hpp"
#include "handoff/memory_layout.h"
#include "mm/boot_mapping.hpp"
#include "mm/dma.hpp"
#include "platform/pci_config.hpp"
#include "platform/state.hpp"
#include "storage/block_device.hpp"
#include "sync/smp.hpp"
#include "util/fixed_string.hpp"
#include "util/memory.h"

inline void* operator new(size_t, void* location) noexcept
{
    return location;
}
namespace
{
constexpr unsigned max_controllers = 4, max_devices = 16, max_depth = 5;
constexpr uint32_t run_bit = 1, async_bit = 1u << 5, async_status = 1u << 15;
constexpr uint32_t port_changes = 0x2a;
struct Controller;
struct Device
{
    Controller* controller{};
    Device* parent{};
    uint8_t root{}, port{}, address{}, depth{};
    bool hub{}, failed{}, published{}, in_toggle{}, out_toggle{};
    usb::storage::Interface interface{};
    usb::storage::Device storage{};
    BlockDevice block{};
    const char* stage = "allocated";
    uint32_t last_status{}, last_detail{};
};
struct Controller
{
    DeviceId owner{DeviceBus::Platform, 0};
    const PciDevice* pci{};
    PciBarResource bar{};
    volatile uint32_t* op{};
    DmaBuffer schedule{}, data{};
    bool failed{}, exposed{};
    uint8_t busy{}, ports{}, count{};
    Device devices[max_devices]{};
    uint8_t configuration[4096]{};
    uint32_t root_before[15]{}, root_after[15]{};
    const char* root_stage[15]{};
    bool probing = true;
};
alignas(Controller) uint8_t pool[max_controllers][sizeof(Controller)]{};
Controller* controllers[max_controllers]{};

void delay_us(unsigned us)
{
    // Keep individual waits short so remote page-table retirement can progress.
    while(us)
    {
        unsigned step = us > 100 ? 100 : us;
        ipi_poll_tlb_shootdown();
        tsc_clock_delay_us(step);
        us -= step;
    }
}
void delay_ms(void*, unsigned ms)
{
    while(ms--)
        delay_us(1000);
}
bool wait_register(volatile uint32_t& reg, uint32_t mask, uint32_t value)
{
    for(unsigned i = 0; i < 10000; ++i)
    {
        if((reg & mask) == value)
            return true;
        delay_us(100);
    }
    return false;
}
void retire(Controller& c)
{
    c.failed = true;
    for(auto& d : c.devices)
    {
        d.failed = true;
        d.storage.invalidate();
    }
    if(c.pci)
        pci_set_command_bits(*c.pci, 0, 4);
    if(c.op)
    {
        c.op[2] = 0;
        c.op[0] = c.op[0] & ~run_bit;
    }
    debug("ehci: controller retired; DMA quarantined")();
}
void schedule_poll(void*)
{
    delay_us(100);
}
bool schedule(Controller& c, bool enabled)
{
    if(ehci::set_async(c.op[0], c.op[1], enabled, schedule_poll, nullptr, 10000))
        return true;
    retire(c);
    return false;
}
bool stop_schedule(Controller& c)
{
    return schedule(c, false);
}
uint32_t port_register(const Device& d)
{
    return d.controller->op[17 + d.root - 1];
}
bool root_connected(const Device& d)
{
    return ehci::root_usable(port_register(d));
}
ehci::Qtd* descriptors(Controller& c)
{
    return reinterpret_cast<ehci::Qtd*>(static_cast<uint8_t*>(c.schedule.virtual_address) + 128);
}
uint32_t td_address(const Controller& c, unsigned index)
{
    return uint32_t(c.schedule.physical_address + 128 + index * sizeof(ehci::Qtd));
}
using Status = usb::storage::TransferStatus;
using Transfer = usb::storage::Transfer;
Transfer transfer(Device& d,
                  uint8_t endpoint,
                  uint16_t packet,
                  bool in,
                  void* data,
                  uint16_t bytes,
                  bool& toggle,
                  const uint8_t* setup = nullptr)
{
    Controller& c = *d.controller;
    if(c.failed || d.failed || bytes > 4096 || (bytes && !data) || !root_connected(d))
        return {Status::Failed, 0};
    if(!stop_schedule(c))
        return {Status::Failed, 0};
    auto* qh = static_cast<ehci::QueueHead*>(c.schedule.virtual_address);
    auto* td = descriptors(c);
    memset(c.schedule.virtual_address, 0, c.schedule.size_bytes);
    if(!in && bytes)
        memcpy(c.data.virtual_address, data, bytes);
    unsigned last = 0, payload = 0;
    if(setup)
    {
        memcpy(static_cast<uint8_t*>(c.schedule.virtual_address) + 512, setup, 8);
        last = bytes ? 2 : 1;
        ehci::prepare_qtd(td[0], c.schedule.physical_address + 512, 8, 2, false, td_address(c, 1));
        if(bytes)
        {
            payload = 1;
            ehci::prepare_qtd(td[1],
                              c.data.physical_address,
                              bytes,
                              in ? 1 : 0,
                              true,
                              td_address(c, last),
                              td_address(c, last));
        }
        ehci::prepare_qtd(td[last], 0, 0, bytes && in ? 0 : 1, true);
    }
    else
        ehci::prepare_qtd(td[0], c.data.physical_address, bytes, in ? 1 : 0, toggle);
    ehci::prepare_head(
        *qh, c.schedule.physical_address, td_address(c, 0), d.address, endpoint, packet);
    dma_sync_for_device(c.data);
    dma_sync_for_device(c.schedule);
    c.op[6] = uint32_t(c.schedule.physical_address);
    c.exposed = true;
    if(!schedule(c, true))
        return {Status::Failed, 0};
    bool completed = false;
    uint32_t fault = 0;
    for(unsigned attempt = 0; attempt < 100000; ++attempt)
    {
        dma_sync_for_cpu(c.schedule);
        for(unsigned i = 0; i <= last; ++i)
            fault |= td[i].token & ehci::errors;
        if(fault || !(td[last].token & ehci::active))
        {
            completed = true;
            break;
        }
        if((c.op[1] & (1u << 4)) || !root_connected(d))
            break;
        delay_us(10);
    }
    if(c.probing)
        d.last_status = td[last].token | fault;
    if(!completed)
        debug("ehci: transfer deadline address=")(d.address)(" endpoint=")(endpoint)();
    if(!stop_schedule(c))
        return {Status::Failed, 0};
    dma_sync_for_cpu(c.schedule);
    if(!completed)
    {
        retire(c);
        return {Status::Failed, 0};
    }
    if(fault)
    {
        if(fault == ehci::halted)
            return {Status::Stall, 0};
        d.failed = true;
        d.storage.invalidate();
        return {Status::Failed, 0};
    }
    uint32_t residual = bytes ? ehci::remaining(td[payload].token) : 0;
    if(residual > bytes)
    {
        retire(c);
        return {Status::Failed, 0};
    }
    uint32_t actual = bytes - residual;
    if(!setup)
        toggle = (qh->token >> 31) != 0;
    if(in && actual)
    {
        dma_sync_for_cpu(c.data);
        memcpy(data, c.data.virtual_address, actual);
    }
    return {Status::Ok, actual};
}
bool control(Device& d,
             uint8_t type,
             uint8_t request,
             uint16_t value,
             uint16_t index,
             void* data,
             uint16_t bytes,
             uint32_t* actual = nullptr)
{
    uint8_t setup[8]{type,
                     request,
                     uint8_t(value),
                     uint8_t(value >> 8),
                     uint8_t(index),
                     uint8_t(index >> 8),
                     uint8_t(bytes),
                     uint8_t(bytes >> 8)};
    if(d.controller->probing)
        d.last_detail = uint32_t(type) << 24 | uint32_t(request) << 16 | value;
    bool toggle = false;
    auto result = transfer(d, 0, 64, type & 0x80, data, bytes, toggle, setup);
    if(actual)
        *actual = result.bytes;
    return result.status == Status::Ok && (actual || result.bytes == bytes);
}
bool hub_control(void* p,
                 uint8_t type,
                 uint8_t request,
                 uint16_t value,
                 uint16_t index,
                 void* data,
                 uint16_t bytes)
{
    return control(*static_cast<Device*>(p), type, request, value, index, data, bytes);
}
bool enumerate(Controller&, uint8_t root, Device* parent, uint8_t port);
bool hub_child(void* p, uint8_t port)
{
    auto& hub = *static_cast<Device*>(p);
    if(enumerate(*hub.controller, hub.root, &hub, port))
        return true;
    // Do not leave a failed address-zero child enabled while enumerating siblings.
    uint8_t status[4]{};
    return !hub.controller->failed && control(hub, 0x23, 1, 1, port, nullptr, 0) &&
           control(hub, 0xa3, 0, 0, port, status, sizeof(status)) && !(status[0] & 0x12);
}
usb::hub::Transport hub_transport(Device& d)
{
    return {&d, hub_control, delay_ms, hub_child};
}
bool path_connected(Device& d)
{
    if(!root_connected(d))
        return false;
    for(Device* child = &d; child->parent; child = child->parent)
    {
        uint32_t status = 0;
        if(!usb::hub::port_status(hub_transport(*child->parent), child->port, status) ||
           !usb::hub::stable_high_speed(status))
            return false;
    }
    return true;
}
Transfer bulk(void* context, bool in, void* data, uint32_t bytes)
{
    auto& d = *static_cast<Device*>(context);
    if(bytes > 4096)
        return {Status::Failed, 0};
    return transfer(d,
                    (in ? d.interface.in : d.interface.out) & 15,
                    512,
                    in,
                    data,
                    uint16_t(bytes),
                    in ? d.in_toggle : d.out_toggle);
}
bool clear_halt(void* context, bool in)
{
    auto& d = *static_cast<Device*>(context);
    if(!control(d, 2, 1, 0, in ? d.interface.in : d.interface.out, nullptr, 0))
        return false;
    (in ? d.in_toggle : d.out_toggle) = false;
    return true;
}
bool reset_storage(void* context)
{
    auto& d = *static_cast<Device*>(context);
    return control(d, 0x21, 0xff, 0, d.interface.number, nullptr, 0);
}
bool submit(BlockDevice& block, BlockRequest& request)
{
    auto& d = *static_cast<Device*>(block.driver_state);
    auto& c = *d.controller;
    const bool flush = request.operation == BlockOperation::Flush;
    if((!flush && request.operation != BlockOperation::Read &&
        request.operation != BlockOperation::Write) ||
       (!flush && (!request.buffer || !request.sector_count || request.sector_count > 8 ||
                   request.sector >= block.sector_count ||
                   request.sector_count > block.sector_count - request.sector)))
    {
        block_request_complete(request, BlockRequestStatus::Invalid);
        return false;
    }
    if(__atomic_exchange_n(&c.busy, 1, __ATOMIC_ACQUIRE))
    {
        request.status = BlockRequestStatus::Busy;
        return false;
    }
    bool ok = !c.failed && !d.failed && path_connected(d);
    if(ok)
        ok = flush ? d.storage.flush()
                   : d.storage.read_write(request.operation == BlockOperation::Write,
                                          request.sector,
                                          request.buffer,
                                          request.sector_count);
    if(!ok)
    {
        d.failed = true;
        d.storage.invalidate();
    }
    __atomic_store_n(&c.busy, 0, __ATOMIC_RELEASE);
    block_request_complete(request,
                           ok ? BlockRequestStatus::Success : BlockRequestStatus::DeviceError,
                           ok && !flush ? request.sector_count * 512 : 0);
    return ok;
}
bool enumerate(Controller& c, uint8_t root, Device* parent, uint8_t port)
{
    if(c.failed || c.count == max_devices || (parent && parent->depth >= max_depth))
        return false;
    auto& d = c.devices[c.count++];
    d.controller = &c;
    d.root = root;
    d.port = port;
    d.parent = parent;
    d.depth = parent ? parent->depth + 1 : 0;
    d.stage = "device-descriptor";
    uint8_t descriptor[18]{};
    if(!control(d, 0x80, 6, 0x100, 0, descriptor, sizeof(descriptor)) || descriptor[0] != 18 ||
       descriptor[1] != 1 || descriptor[7] != 64 || !descriptor[17])
        return false;
    d.stage = "set-address";
    if(!control(d, 0, 5, c.count, 0, nullptr, 0))
        return false;
    d.address = c.count;
    delay_ms(nullptr, 2);
    d.stage = "config-header";
    auto* config = c.configuration;
    if(!control(d, 0x80, 6, 0x200, 0, config, 9))
        return false;
    uint16_t length = uint16_t(config[2]) | uint16_t(config[3]) << 8;
    d.stage = "configuration";
    if(length < 9 || length > sizeof(c.configuration) ||
       !control(d, 0x80, 6, 0x200, 0, config, length))
        return false;
    uint8_t hub_configuration = 0;
    if(descriptor[4] == 9 && usb::hub::configuration(config, length, hub_configuration))
    {
        if(d.depth >= max_depth || !control(d, 0, 9, hub_configuration, 0, nullptr, 0))
            return false;
        d.stage = "hub-descriptor";
        uint8_t hub_descriptor[16]{};
        uint32_t actual = 0;
        usb::hub::Descriptor hub{};
        if(!control(d, 0xa0, 6, 0x2900, 0, hub_descriptor, sizeof(hub_descriptor), &actual) ||
           !usb::hub::parse_descriptor(hub_descriptor, actual, hub))
            return false;
        d.hub = true;
        debug("ehci: hub address=")(d.address)(" root=")(root)(" ports=")(hub.ports)(" depth=")(
            d.depth)();
        d.stage = "hub-ports";
        if(!usb::hub::enumerate(hub_transport(d), hub))
        {
            retire(c);  // An unresolved reset cannot leave address zero reachable.
            return false;
        }
        d.stage = "hub-ready";
        return true;
    }
    d.stage = "storage-config";
    if(!usb::storage::parse_configuration(config, length, d.interface) ||
       d.interface.in_packet != 512 || d.interface.out_packet != 512 ||
       !control(d, 0, 9, d.interface.configuration, 0, nullptr, 0))
        return false;
    d.stage = "storage-probe";
    debug("ehci: storage probe address=")(d.address)();
    if(!d.storage.initialize({&d, bulk, clear_halt, reset_storage}))
    {
        d.stage = d.storage.probe_stage;
        d.failed = true;
        debug("ehci: storage rejected address=")(d.address)();
        return false;
    }
    d.block = {"ehci-bot", d.storage.sectors, 512, 8, 1, &d, submit, submit};
    d.published = true;
    d.stage = d.storage.probe_stage;
    debug("ehci: storage ready address=")(d.address)(" root=")(root)(" hub=")(
        parent ? parent->address : 0)(" port=")(port)(" sectors=")(d.storage.sectors)();
    return true;
}
void root_write(Controller& c, uint8_t port, uint32_t set, uint32_t clear = 0)
{
    auto& reg = c.op[17 + port - 1];
    reg = (reg & ~(port_changes | clear)) | set;
}
bool legacy_handoff(const PciDevice& pci, uint32_t caps)
{
    uint8_t pointer = (caps >> 8) & 0xff;
    uint64_t visited = 0;
    while(pointer)
    {
        if(pointer < 0x40 || pointer > 0xf8 || (pointer & 3) || (visited & (1ull << (pointer / 4))))
            return false;
        visited |= 1ull << (pointer / 4);
        uint32_t cap = pci_config_read32(pci, pointer);
        if((cap & 0xff) == 1)
        {
            pci_config_write32(pci, pointer, cap | (1u << 24));
            bool owned = false;
            for(unsigned i = 0; i < 10000; ++i)
            {
                if(!(pci_config_read32(pci, pointer) & (1u << 16)))
                {
                    owned = true;
                    break;
                }
                delay_us(100);
            }
            if(!owned)
                return false;
            // Disable legacy SMIs and acknowledge their status after ownership.
            pci_config_write32(pci, pointer + 4, 0xe0000000u);
        }
        pointer = uint8_t(cap >> 8);
    }
    return true;
}
void capture(Controller& c, uint32_t pci_index)
{
    c.probing = false;
    Os1ObserveUsbRecord record{};
    record.kind = OS1_OBSERVE_USB_CONTROLLER;
    record.pci_index = pci_index;
    record.status = c.op ? c.op[1] : 0;
    record.detail = c.count;
    copy_fixed_string(record.stage, sizeof(record.stage), c.failed ? "retired" : "started");
    usb::diagnostics::append(record);
    for(unsigned port = 1; port <= c.ports; ++port)
    {
        record = {};
        record.kind = OS1_OBSERVE_USB_ROOT_PORT;
        record.pci_index = pci_index;
        record.root_port = port;
        record.status = c.root_after[port - 1];
        record.detail = c.root_before[port - 1];
        copy_fixed_string(record.stage,
                          sizeof(record.stage),
                          c.root_stage[port - 1] ? c.root_stage[port - 1] : "not-visited");
        usb::diagnostics::append(record);
    }
    for(unsigned i = 0; i < c.count; ++i)
    {
        const auto& d = c.devices[i];
        record = {};
        record.kind = OS1_OBSERVE_USB_DEVICE;
        record.pci_index = pci_index;
        record.root_port = d.root;
        record.port = d.port;
        record.address = d.address;
        record.parent_address = d.parent ? d.parent->address : 0;
        record.status = d.last_status;
        record.detail = d.last_detail;
        if(d.storage.probe_stage)
            record.detail = uint32_t(d.storage.probe_opcode) << 24 |
                            uint32_t(d.storage.probe_result) << 16 | d.storage.probe_mode;
        if(d.storage.probe_sense != 0xffffffff)
            record.detail = 0x35000000 | d.storage.probe_sense;
        record.sectors = d.storage.sectors;
        copy_fixed_string(record.stage, sizeof(record.stage), d.stage);
        usb::diagnostics::append(record);
    }
}
bool probe(VirtualMemory& vm,
           PageFrameContainer& frames,
           const PciDevice& pci,
           size_t pci_index,
           DeviceId id)
{
    unsigned slot = 0;
    while(slot < max_controllers && controllers[slot])
        ++slot;
    if(slot == max_controllers || !tsc_clock_calibrate())
        return false;
    auto& c = *new(pool[slot]) Controller{};
    c.owner = id;
    c.pci = &pci;
    debug("ehci: probe pci=")(pci.bus, 16)(":")(pci.slot, 16)(".")(pci.function)();
    if(!claim_pci_bar(id, uint16_t(pci_index), 0, c.bar))
        return false;
    auto fail = [&]() {
        if(c.exposed)
        {
            controllers[slot] = &c;
            retire(c);
        }
        else
        {
            if(c.op)
            {
                c.op[0] = c.op[0] & ~(run_bit | async_bit);
                pci_set_command_bits(pci, 0, 4);
            }
            if(c.schedule.active)
                dma_release_buffer(frames, c.schedule);
            if(c.data.active)
                dma_release_buffer(frames, c.data);
            release_pci_bars_for_owner(id);
        }
        return false;
    };
    if((c.bar.type != PciBarType::Mmio32 && c.bar.type != PciBarType::Mmio64) || !c.bar.base ||
       c.bar.size < 256 || c.bar.size > 65536 || !map_mmio_range(vm, c.bar.base, c.bar.size))
        return fail();
    pci_set_command_bits(pci, 2);
    auto* caps = kernel_physical_pointer<volatile uint32_t>(c.bar.base);
    uint32_t header = caps[0], structural = caps[1], capability = caps[2];
    unsigned offset = header & 0xff;
    c.ports = structural & 15;
    if(header == 0xffffffff || offset < 16 || (offset & 3) || !c.ports ||
       offset + 68 + 4u * c.ports > c.bar.size || !legacy_handoff(pci, capability))
        return fail();
    c.op = kernel_physical_pointer<volatile uint32_t>(c.bar.base + offset);
    c.op[2] = 0;
    c.op[0] = c.op[0] & ~run_bit;
    if(!wait_register(c.op[1], 1u << 12, 1u << 12))
        return fail();
    c.op[0] = 2;
    if(!wait_register(c.op[0], 2, 0))
        return fail();
    if(!dma_allocate_buffer(frames, id, 4096, DmaDirection::Bidirectional, c.schedule) ||
       !dma_allocate_buffer(frames, id, 4096, DmaDirection::Bidirectional, c.data) ||
       c.schedule.physical_address > 0xfffff000u || c.data.physical_address > 0xfffff000u)
        return fail();
    c.op[2] = 0;  // Polling profile; no interrupt endpoint schedule.
    if(capability & 1)
        c.op[4] = 0;
    c.op[1] = 0x3f;
    pci_enable_mmio_bus_mastering(pci);
    c.op[0] = (8u << 16) | run_bit;
    if(!wait_register(c.op[1], 1u << 12, 0))
        return fail();
    controllers[slot] = &c;
    debug("ehci: controller running")();
    c.op[16] = 1;
    for(uint8_t port = 1; port <= c.ports; ++port)
        root_write(c, port, 1u << 12);
    delay_ms(nullptr, 100);
    for(uint8_t port = 1; port <= c.ports && !c.failed; ++port)
    {
        c.root_before[port - 1] = c.op[17 + port - 1];
        c.root_after[port - 1] = c.root_before[port - 1];
        c.root_stage[port - 1] = "disconnected";
        if(!(c.op[17 + port - 1] & 1))
            continue;
        c.root_stage[port - 1] = "reset";
        root_write(c, port, 1u << 8, 4u | (1u << 13));
        delay_ms(nullptr, 50);
        root_write(c, port, 0, 1u << 8);
        if(!wait_register(c.op[17 + port - 1], 1u << 8, 0))
        {
            retire(c);
            break;
        }
        delay_ms(nullptr, 10);
        root_write(c, port, port_changes);
        c.root_after[port - 1] = c.op[17 + port - 1];
        if(!ehci::root_usable(c.root_after[port - 1]))
        {
            c.root_stage[port - 1] = "not-usable";
            debug("ehci: unsupported root speed port=")(port)();
            continue;
        }
        c.root_stage[port - 1] = "enumerated";
        if(!enumerate(c, port, nullptr, port))
        {
            c.root_stage[port - 1] = "enumeration-failed";
            debug("ehci: enumeration failed root=")(port)();
            if(!c.failed)
            {
                root_write(c, port, 0, 4);
                if(!wait_register(c.op[17 + port - 1], 4u | (1u << 8), 0))
                    retire(c);
            }
        }
    }
    capture(c, uint32_t(pci_index));
    if(c.failed)
        return false;
    device_binding_publish(id, uint16_t(pci_index), "ehci", &c);
    device_binding_set_state(id, DeviceState::Started);
    debug("ehci: ready ports=")(c.ports)(" addresses=")(c.count)();
    return true;
}
}  // namespace
const PciDriver& ehci_pci_driver()
{
    static const PciMatch matches[]{
        {.class_code = 0x0c,
         .subclass = 3,
         .prog_if = 0x20,
         .match_flags = kPciMatchClassCode | kPciMatchSubclass | kPciMatchProgIf}};
    static const PciDriver driver{
        .name = "ehci",
        .matches = matches,
        .match_count = 1,
        .probe = probe,
        .remove = [](DeviceId id) {
            for(auto* c : controllers)
                if(c && c->owner.bus == id.bus && c->owner.index == id.index)
                {
                    retire(*c);
                    device_binding_remove(id);
                }
        }};
    return driver;
}
BlockDevice* ehci_storage_block_device(size_t index)
{
    for(auto* c : controllers)
        if(c)
            for(auto& d : c->devices)
                if(d.published)
                {
                    if(!index)
                        return &d.block;
                    --index;
                }
    return nullptr;
}
