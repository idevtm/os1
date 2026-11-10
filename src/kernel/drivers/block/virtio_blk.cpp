// Modern virtio-blk PCI driver using the shared virtio transport, one DMA
// backed queue, and interrupt-driven completions.
#include "drivers/block/virtio_blk.hpp"

#include "arch/x86_64/cpu/x86.hpp"
#include "core/kernel_state.hpp"
#include "debug/debug.hpp"
#include "debug/event_ring.hpp"
#include "drivers/block/completion.hpp"
#include "drivers/bus/device.hpp"
#include "drivers/bus/resource.hpp"
#include "drivers/virtio/pci_transport.hpp"
#include "drivers/virtio/virtqueue.hpp"
#include "handoff/memory_layout.h"
#include "mm/dma.hpp"
#include "platform/hardware_catalog.hpp"
#include "platform/irq_registry.hpp"
#include "platform/pci_config.hpp"
#include "platform/state.hpp"
#include "proc/thread.hpp"
#include "storage/block_device.hpp"
#include "util/string.h"

namespace
{
constexpr uint16_t kPciVendorVirtio = 0x1AF4;
constexpr uint16_t kPciDeviceVirtioBlkModern = 0x1042;
constexpr PciMatch kVirtioBlkMatches[]{{
    .vendor_id = kPciVendorVirtio,
    .device_id = kPciDeviceVirtioBlkModern,
    .match_flags = static_cast<uint8_t>(kPciMatchVendorId | kPciMatchDeviceId),
}};
constexpr uint32_t kVirtioBlkRequestIn = 0;
constexpr uint32_t kVirtioBlkRequestOut = 1;
constexpr uint16_t kVirtioBlkQueueTargetSize = 8;
constexpr uint16_t kVirtioBlkDescriptorsPerRequest = 3;
constexpr uint16_t kVirtioBlkMaxRequestSlots = kVirtqueueMaxSize / kVirtioBlkDescriptorsPerRequest;
constexpr uint64_t kVirtioSectorSize = 512;
// Bound on the data-descriptor span so the per-slot DMA buffer stays a single
// page-aligned contiguous range. 8 sectors == 4 KiB, which matches the typical
// filesystem block size and keeps the slot DMA buffer at one page plus the
// header + status byte.
constexpr uint32_t kVirtioBlkMaxSectorsPerRequest = 8;
constexpr const char* kVirtioSector0Prefix = "OS1 VIRTIO TEST DISK SECTOR 0 SIGNATURE";
constexpr const char* kVirtioSector1Prefix = "OS1 VIRTIO TEST DISK SECTOR 1 PAYLOAD";
constexpr const char* kVirtioWriteProbe = "OS1 VIRTIO WRITE PATH OK";
constexpr uint8_t kVirtioStatusOk = 0;

struct [[gnu::packed]] VirtioBlkConfig
{
    uint64_t capacity;
    uint32_t size_max;
    uint32_t seg_max;
    uint16_t cylinders;
    uint8_t heads;
    uint8_t sectors;
    uint32_t blk_size;
};

struct [[gnu::packed]] VirtioBlkRequestHeader
{
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

struct VirtioBlkRequestSlot
{
    bool active = false;
    uint16_t head_descriptor = 0;
    uint16_t descriptor_mask = 0;
    DmaBuffer buffer{};
    VirtioBlkRequestHeader* header = nullptr;
    uint8_t* data = nullptr;
    volatile uint8_t* status = nullptr;
    BlockRequest* request = nullptr;
};

struct VirtioBlkState
{
    bool present = false;
    bool flush_supported = false;
    bool read_only = false;
    DeviceId owner{DeviceBus::Pci, 0};
    uint16_t queue_size = 0;
    uint16_t request_slot_count = 0;
    uint16_t pci_index = 0;
    uint16_t descriptor_in_use_mask = 0;
    uint64_t capacity_sectors = 0;
    VirtioPciTransport transport{};
    Virtqueue queue{};
    VirtioBlkRequestSlot request_slots[kVirtioBlkMaxRequestSlots]{};
};

constinit VirtioBlkState g_virtio_blk{};
BlockDevice g_virtio_blk_device{};
Spinlock queue_lock{"virtio-blk"};

[[nodiscard]] uint16_t virtio_blk_slot_count(uint16_t queue_size)
{
    return static_cast<uint16_t>(queue_size / kVirtioBlkDescriptorsPerRequest);
}

[[nodiscard]] uint16_t virtio_blk_descriptor_mask(uint16_t head_descriptor)
{
    return static_cast<uint16_t>((1u << head_descriptor) | (1u << (head_descriptor + 1u)) |
                                 (1u << (head_descriptor + 2u)));
}

void virtio_blk_fail_immediately(BlockRequest& request, BlockRequestStatus status)
{
    completion_reset(request.completion);
    request.status = status;
    request.bytes_transferred = 0;
    request.driver_context = nullptr;
    (void)completion_signal(request.completion);
}

bool virtio_blk_allocate_request_buffers(PageFrameContainer& frames, VirtioBlkState& state)
{
    // Reserve enough room for a header, the largest supported multi-sector data
    // span, and the trailing status byte. Status sits at a slot-fixed offset so
    // the per-request descriptor only varies the data length.
    const size_t request_buffer_size =
        sizeof(VirtioBlkRequestHeader) + kVirtioBlkMaxSectorsPerRequest * kVirtioSectorSize + 1u;
    for(uint16_t slot_index = 0; slot_index < state.request_slot_count; ++slot_index)
    {
        auto& slot = state.request_slots[slot_index];
        slot = {};
        slot.head_descriptor = static_cast<uint16_t>(slot_index * kVirtioBlkDescriptorsPerRequest);
        slot.descriptor_mask = virtio_blk_descriptor_mask(slot.head_descriptor);
        if(!dma_allocate_buffer(
               frames, state.owner, request_buffer_size, DmaDirection::Bidirectional, slot.buffer))
        {
            return false;
        }

        slot.header = static_cast<VirtioBlkRequestHeader*>(slot.buffer.virtual_address);
        slot.data =
            static_cast<uint8_t*>(slot.buffer.virtual_address) + sizeof(VirtioBlkRequestHeader);
        slot.status = reinterpret_cast<volatile uint8_t*>(
            static_cast<uint8_t*>(slot.buffer.virtual_address) + sizeof(VirtioBlkRequestHeader) +
            kVirtioBlkMaxSectorsPerRequest * kVirtioSectorSize);
    }
    return true;
}

void virtio_blk_release_request_slot(VirtioBlkState& state, VirtioBlkRequestSlot& slot)
{
    state.descriptor_in_use_mask =
        static_cast<uint16_t>(state.descriptor_in_use_mask & ~slot.descriptor_mask);
    slot.active = false;
    slot.request = nullptr;
}

void virtio_blk_release_request_buffers(PageFrameContainer& frames, VirtioBlkState& state)
{
    for(uint16_t slot_index = 0; slot_index < kVirtioBlkMaxRequestSlots; ++slot_index)
    {
        auto& slot = state.request_slots[slot_index];
        if(nullptr != slot.request)
        {
            block_request_complete(*slot.request, BlockRequestStatus::Timeout);
        }
        dma_release_buffer(frames, slot.buffer);
        slot = {};
    }
    state.request_slot_count = 0;
    state.descriptor_in_use_mask = 0;
}

void virtio_blk_release_runtime_resources(PageFrameContainer& frames, VirtioBlkState& state)
{
    if(nullptr != state.transport.device)
    {
        pci_release_interrupt(*state.transport.device, state.transport.interrupt);
    }
    virtio_blk_release_request_buffers(frames, state);
    virtqueue_release(frames, state.queue);
    release_pci_bars_for_owner(state.owner);
    platform_release_irq_routes_for_owner(state.owner);
}

VirtioBlkRequestSlot* virtio_blk_find_request_slot(VirtioBlkState& state, uint16_t head_descriptor)
{
    for(uint16_t slot_index = 0; slot_index < state.request_slot_count; ++slot_index)
    {
        auto& slot = state.request_slots[slot_index];
        if(slot.head_descriptor == head_descriptor)
        {
            return &slot;
        }
    }
    return nullptr;
}

VirtioBlkRequestSlot* virtio_blk_acquire_request_slot(VirtioBlkState& state)
{
    for(uint16_t slot_index = 0; slot_index < state.request_slot_count; ++slot_index)
    {
        auto& slot = state.request_slots[slot_index];
        if(slot.active || (0 != (state.descriptor_in_use_mask & slot.descriptor_mask)))
        {
            continue;
        }

        slot.active = true;
        state.descriptor_in_use_mask =
            static_cast<uint16_t>(state.descriptor_in_use_mask | slot.descriptor_mask);
        return &slot;
    }
    return nullptr;
}

void virtio_blk_drain_used(VirtioBlkState& state)
{
    dma_sync_for_cpu(state.queue.ring_memory);

    VirtqUsedElem used{};
    while(virtqueue_consume_used(state.queue, used))
    {
        auto* slot = virtio_blk_find_request_slot(state, static_cast<uint16_t>(used.id));
        if((nullptr == slot) || !slot->active || (nullptr == slot->request))
        {
            continue;
        }

        BlockRequest* request = slot->request;
        dma_sync_for_cpu(slot->buffer);

        const uint8_t completion_status = *slot->status;
        const uint32_t completion_bytes = used.len;
        const uint32_t request_bytes =
            request->sector_count * static_cast<uint32_t>(kVirtioSectorSize);
        const bool success = kVirtioStatusOk == completion_status;
        if(success && (BlockOperation::Read == request->operation) && (nullptr != request->buffer))
        {
            memcpy(request->buffer, slot->data, request_bytes);
        }

        virtio_blk_release_request_slot(state, *slot);
        request->driver_context = nullptr;
        if(success)
        {
            kernel_event::record(OS1_KERNEL_EVENT_BLOCK_IO,
                                 OS1_KERNEL_EVENT_FLAG_SUCCESS,
                                 request->sector,
                                 request_bytes,
                                 static_cast<uint64_t>(request->operation),
                                 completion_bytes);
            block_request_complete(*request, BlockRequestStatus::Success, request_bytes);
            continue;
        }

        kernel_event::record(OS1_KERNEL_EVENT_BLOCK_IO,
                             OS1_KERNEL_EVENT_FLAG_FAILURE,
                             request->sector,
                             request_bytes,
                             completion_status,
                             completion_bytes);
        block_request_complete(*request, BlockRequestStatus::DeviceError);
    }
}

bool virtio_blk_wait_for_boot_completion(VirtioBlkState& state, BlockRequest& request)
{
    for(uint32_t spin = 0; spin < 10000000u; ++spin)
    {
        if(poll_block_completion(request, queue_lock, [&] { virtio_blk_drain_used(state); }))
        {
            return true;
        }
        ipi_poll_tlb_shootdown();
        pause();
    }
    return false;
}

void virtio_blk_irq(void* data)
{
    auto& state = *static_cast<VirtioBlkState*>(data);
    if(nullptr != state.transport.isr_status &&
       (PciInterruptMode::LegacyIntx == state.transport.interrupt.mode))
    {
        (void)*state.transport.isr_status;
    }

    IrqSpinGuard guard(queue_lock);
    virtio_blk_drain_used(state);
}

bool virtio_blk_submit_request(BlockDevice& device, BlockRequest& request)
{
    auto& state = *static_cast<VirtioBlkState*>(device.driver_state);
    const bool is_flush = request.operation == BlockOperation::Flush;
    VirtioBlkRequestSlot* slot = nullptr;
    {
        IrqSpinGuard guard(queue_lock);
        if(!state.present ||
           (is_flush &&
            (!state.flush_supported || request.buffer || request.sector_count || request.sector)) ||
           (!is_flush && ((nullptr == request.buffer) || (0u == request.sector_count) ||
                          (request.sector_count > kVirtioBlkMaxSectorsPerRequest))))
        {
            virtio_blk_fail_immediately(request, BlockRequestStatus::Invalid);
            return false;
        }
        // Reject sector ranges that overflow uint64_t or exceed device capacity.
        if((request.sector > state.capacity_sectors) ||
           ((state.capacity_sectors - request.sector) < request.sector_count))
        {
            virtio_blk_fail_immediately(request, BlockRequestStatus::Invalid);
            return false;
        }

        const bool is_read = BlockOperation::Read == request.operation;
        const bool is_write = BlockOperation::Write == request.operation;
        if((!is_read && !is_write && !is_flush) || (is_write && state.read_only))
        {
            virtio_blk_fail_immediately(request, BlockRequestStatus::Invalid);
            return false;
        }

        slot = virtio_blk_acquire_request_slot(state);
        if(nullptr == slot)
        {
            virtio_blk_fail_immediately(request, BlockRequestStatus::Busy);
            return false;
        }

        const uint32_t request_bytes =
            request.sector_count * static_cast<uint32_t>(kVirtioSectorSize);
        const uint64_t status_offset =
            sizeof(VirtioBlkRequestHeader) + kVirtioBlkMaxSectorsPerRequest * kVirtioSectorSize;

        kernel_event::record(OS1_KERNEL_EVENT_BLOCK_IO,
                             OS1_KERNEL_EVENT_FLAG_BEGIN,
                             request.sector,
                             request_bytes,
                             static_cast<uint64_t>(request.operation),
                             request.sector_count);

        completion_reset(request.completion);
        request.status = BlockRequestStatus::Pending;
        request.bytes_transferred = 0;
        request.driver_context = slot;
        slot->request = &request;

        memset(slot->header, 0, sizeof(*slot->header));
        memset(slot->data, 0, request_bytes);
        *slot->status = 0xFFu;
        if(is_write)
        {
            memcpy(slot->data, request.buffer, request_bytes);
        }

        slot->header->type = is_flush ? 4u : is_read ? kVirtioBlkRequestIn : kVirtioBlkRequestOut;
        slot->header->sector = request.sector;

        state.queue.desc[slot->head_descriptor].addr = slot->buffer.physical_address;
        state.queue.desc[slot->head_descriptor].len = sizeof(VirtioBlkRequestHeader);
        state.queue.desc[slot->head_descriptor].flags = kVirtqDescFlagNext;
        state.queue.desc[slot->head_descriptor].next =
            static_cast<uint16_t>(slot->head_descriptor + (is_flush ? 2u : 1u));

        state.queue.desc[slot->head_descriptor + 1u].addr =
            slot->buffer.physical_address + sizeof(VirtioBlkRequestHeader);
        state.queue.desc[slot->head_descriptor + 1u].len = request_bytes;
        state.queue.desc[slot->head_descriptor + 1u].flags =
            is_read ? static_cast<uint16_t>(kVirtqDescFlagWrite | kVirtqDescFlagNext)
                    : kVirtqDescFlagNext;
        state.queue.desc[slot->head_descriptor + 1u].next =
            static_cast<uint16_t>(slot->head_descriptor + 2u);

        state.queue.desc[slot->head_descriptor + 2u].addr =
            slot->buffer.physical_address + status_offset;
        state.queue.desc[slot->head_descriptor + 2u].len = 1;
        state.queue.desc[slot->head_descriptor + 2u].flags = kVirtqDescFlagWrite;
        state.queue.desc[slot->head_descriptor + 2u].next = 0;

        dma_sync_for_device(slot->buffer);
        dma_sync_for_device(state.queue.ring_memory);
        if(!virtqueue_submit(state.queue, slot->head_descriptor))
        {
            request.driver_context = nullptr;
            virtio_blk_release_request_slot(state, *slot);
            virtio_blk_fail_immediately(request, BlockRequestStatus::Invalid);
            return false;
        }
        virtio_pci_notify_queue(state.transport, 0);
    }
    // Poll with no queue lock held. This bounded profile works from IF=0 syscall
    // continuations as well as boot; interrupt completion remains enabled.
    if(!virtio_blk_wait_for_boot_completion(state, request))
    {
        IrqSpinGuard guard(queue_lock);
        // Quarantine all DMA buffers after timeout. Never recycle descriptors still
        // owned by an unresponsive device, or retain pointers to caller stacks.
        return finish_block_poll(
            request,
            [&] { virtio_blk_drain_used(state); },
            [&] {
                state.present = false;
                virtio_pci_write_device_status(state.transport, 0);
                for(auto& pending : state.request_slots)
                {
                    if(pending.request)
                    {
                        pending.request->driver_context = nullptr;
                        block_request_complete(*pending.request, BlockRequestStatus::Timeout);
                        pending.request = nullptr;
                    }
                }
            });
    }
    return true;
}

bool virtio_blk_flush(BlockDevice& device, BlockRequest& request)
{
    return virtio_blk_submit_request(device, request);
}

bool verify_virtio_blk_prefix(BlockDevice& device, uint64_t sector, const char* expected_prefix)
{
    uint8_t buffer[kVirtioSectorSize]{};
    if(!block_read_sync(device, sector, buffer, 1))
    {
        return false;
    }
    const size_t prefix_length = strlen(expected_prefix);
    if(0 != memcmp(buffer, expected_prefix, prefix_length))
    {
        debug("virtio-blk: sector verification failed sector=")(sector)();
        return false;
    }
    return true;
}

bool verify_virtio_blk_write(BlockDevice& device)
{
    uint8_t buffer[kVirtioSectorSize]{};
    memcpy(buffer, kVirtioWriteProbe, strlen(kVirtioWriteProbe));
    if(!block_write_sync(device, 2, buffer, 1))
    {
        debug("virtio-blk: write smoke failed")();
        return false;
    }

    uint8_t verify[kVirtioSectorSize]{};
    if(!block_read_sync(device, 2, verify, 1))
    {
        return false;
    }
    if(0 != memcmp(verify, buffer, sizeof(buffer)))
    {
        debug("virtio-blk: write verification failed")();
        return false;
    }
    return true;
}

// Issues a single multi-sector request (sector_count > 1) so the QEMU smoke
// matrix exercises the new contract end-to-end. Returns false on any failure
// and serial-logs the first failing step.
bool verify_virtio_blk_multi_sector_read(BlockDevice& device)
{
    constexpr uint32_t kMultiSectors = 2;
    uint8_t buffer[kMultiSectors * kVirtioSectorSize]{};
    BlockRequest request{};
    request.operation = BlockOperation::Read;
    request.sector = 0;
    request.buffer = buffer;
    request.sector_count = kMultiSectors;
    if(!device.submit(device, request))
    {
        debug("virtio-blk: multi-sector submit failed")();
        return false;
    }
    if(!block_request_wait(request) || !block_request_succeeded(request))
    {
        debug("virtio-blk: multi-sector wait failed")();
        return false;
    }
    if(request.bytes_transferred != kMultiSectors * kVirtioSectorSize)
    {
        debug("virtio-blk: multi-sector bytes_transferred mismatch=")(request.bytes_transferred)();
        return false;
    }
    if(0 != memcmp(buffer, kVirtioSector0Prefix, strlen(kVirtioSector0Prefix)) ||
       0 != memcmp(buffer + kVirtioSectorSize, kVirtioSector1Prefix, strlen(kVirtioSector1Prefix)))
    {
        debug("virtio-blk: multi-sector content mismatch")();
        return false;
    }
    return true;
}

bool verify_virtio_blk_multi_sector_write(BlockDevice& device)
{
    if(g_virtio_blk.capacity_sectors < 4u)
    {
        debug("virtio-blk: multi-sector write skipped, disk too small")();
        return true;
    }

    constexpr uint32_t kMultiSectors = 2;
    uint8_t pattern[kMultiSectors * kVirtioSectorSize]{};
    for(uint64_t i = 0; i < sizeof(pattern); ++i)
    {
        // Distinguishable per-byte pattern so a one-sector misalignment is visible.
        pattern[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);
    }
    BlockRequest write_request{};
    write_request.operation = BlockOperation::Write;
    write_request.sector = 2;
    write_request.buffer = pattern;
    write_request.sector_count = kMultiSectors;
    if(!device.submit(device, write_request))
    {
        debug("virtio-blk: multi-sector write submit failed")();
        return false;
    }
    if(!block_request_wait(write_request) || !block_request_succeeded(write_request))
    {
        debug("virtio-blk: multi-sector write completion failed")();
        return false;
    }
    if(write_request.bytes_transferred != sizeof(pattern))
    {
        debug("virtio-blk: multi-sector write bytes_transferred mismatch=")(
            write_request.bytes_transferred)();
        return false;
    }

    uint8_t readback[kMultiSectors * kVirtioSectorSize]{};
    BlockRequest read_request{};
    read_request.operation = BlockOperation::Read;
    read_request.sector = 2;
    read_request.buffer = readback;
    read_request.sector_count = kMultiSectors;
    if(!device.submit(device, read_request))
    {
        debug("virtio-blk: multi-sector readback submit failed")();
        return false;
    }
    if(!block_request_wait(read_request) || !block_request_succeeded(read_request))
    {
        debug("virtio-blk: multi-sector readback failed")();
        return false;
    }
    if(0 != memcmp(readback, pattern, sizeof(pattern)))
    {
        debug("virtio-blk: multi-sector readback content mismatch")();
        return false;
    }
    return true;
}

}  // namespace

bool probe_virtio_blk_device(VirtualMemory& kernel_vm,
                             PageFrameContainer& frames,
                             const PciDevice& device,
                             size_t device_index,
                             VirtioBlkDevice& public_device)
{
    if((device.vendor_id != kPciVendorVirtio) || (device.device_id != kPciDeviceVirtioBlkModern))
    {
        return true;
    }
    if(g_virtio_blk.present)
    {
        debug("virtio-blk: additional device ignored")();
        return true;
    }

    auto& state = g_virtio_blk;
    state = {};
    state.owner = DeviceId{DeviceBus::Pci, static_cast<uint16_t>(device_index)};
    state.pci_index = static_cast<uint16_t>(device_index);

    if(!virtio_pci_bind_transport(
           kernel_vm, state.owner, device, sizeof(VirtioBlkConfig), state.transport))
    {
        debug("virtio-blk: transport bind failed")();
        return false;
    }

    virtio_pci_write_device_status(state.transport, 0);
    virtio_pci_write_device_status(state.transport, kVirtioStatusAcknowledge);
    virtio_pci_write_device_status(
        state.transport,
        static_cast<uint8_t>(state.transport.common_cfg->device_status | kVirtioStatusDriver));
    state.transport.common_cfg->device_feature_select = 0;
    const uint32_t features = state.transport.common_cfg->device_feature;
    state.flush_supported = (features & (1u << 9)) != 0;
    state.read_only = (features & (1u << 5)) != 0;
    if(!virtio_pci_negotiate_features(state.transport,
                                      kVirtioFeatureVersion1,
                                      kVirtioFeatureVersion1 | (1ull << 9) | (1ull << 5)))
    {
        debug("virtio-blk: feature negotiation rejected")();
        return false;
    }

    state.transport.common_cfg->queue_select = 0;
    const uint16_t device_queue_size = state.transport.common_cfg->queue_size;
    if(device_queue_size < 3u)
    {
        debug("virtio-blk: queue too small")();
        return false;
    }
    state.queue_size = (device_queue_size < kVirtioBlkQueueTargetSize) ? device_queue_size
                                                                       : kVirtioBlkQueueTargetSize;
    state.request_slot_count = virtio_blk_slot_count(state.queue_size);
    if(0 == state.request_slot_count)
    {
        debug("virtio-blk: queue leaves no request slots")();
        return false;
    }

    if(!virtqueue_allocate(frames, state.owner, state.queue_size, state.queue))
    {
        debug("virtio-blk: queue DMA allocation failed")();
        return false;
    }
    if(!virtio_blk_allocate_request_buffers(frames, state))
    {
        debug("virtio-blk: request DMA allocation failed")();
        virtqueue_release(frames, state.queue);
        return false;
    }

    if(!virtio_pci_bind_queue_interrupt(kernel_vm, state.transport, 0, 0, virtio_blk_irq, &state))
    {
        debug("virtio-blk: interrupt bind failed")();
        virtio_blk_release_request_buffers(frames, state);
        virtqueue_release(frames, state.queue);
        release_pci_bars_for_owner(state.owner);
        return false;
    }
    if(!virtio_pci_setup_queue(state.transport,
                               0,
                               state.queue_size,
                               state.queue.ring_memory.physical_address,
                               state.queue.ring_memory.physical_address + kPageSize,
                               state.queue.ring_memory.physical_address + 2u * kPageSize))
    {
        debug("virtio-blk: queue setup failed")();
        virtio_blk_release_runtime_resources(frames, state);
        return false;
    }

    auto* config = reinterpret_cast<volatile VirtioBlkConfig*>(state.transport.device_cfg);
    state.capacity_sectors = config->capacity;
    virtio_pci_write_device_status(
        state.transport,
        static_cast<uint8_t>(state.transport.common_cfg->device_status | kVirtioStatusDriverOk));
    state.present = true;

    public_device.present = true;
    public_device.queue_size = state.queue_size;
    public_device.capacity_sectors = state.capacity_sectors;
    public_device.pci_index = static_cast<uint16_t>(device_index);

    g_virtio_blk_device.name = "virtio-blk";
    g_virtio_blk_device.sector_count = state.capacity_sectors;
    g_virtio_blk_device.sector_size = static_cast<uint32_t>(kVirtioSectorSize);
    g_virtio_blk_device.max_sectors_per_request = kVirtioBlkMaxSectorsPerRequest;
    g_virtio_blk_device.queue_depth = state.request_slot_count;
    g_virtio_blk_device.driver_state = &state;
    g_virtio_blk_device.submit = virtio_blk_submit_request;
    g_virtio_blk_device.flush = virtio_blk_flush;
    hardware_catalog::block(g_virtio_blk_device,
                            os1::hardware::object_id(os1::hardware::Pci, device_index));

    (void)device_binding_publish(
        state.owner, static_cast<uint16_t>(device_index), "virtio-blk", &state);
    (void)device_binding_set_state(state.owner, DeviceState::Started);

    debug("virtio-blk: ready pci=")(device.bus, 16, 2)(":")(device.slot, 16, 2)(".")(
        device.function, 16, 1)(" sectors=")(state.capacity_sectors)(" qsize=")(state.queue_size)(
        " depth=")(state.request_slot_count)(" irq=")(state.transport.interrupt.vector, 16, 2)();
    kernel_event::record(OS1_KERNEL_EVENT_PCI_BIND,
                         OS1_KERNEL_EVENT_FLAG_SUCCESS,
                         static_cast<uint64_t>(device_index),
                         pci_bdf(device),
                         (static_cast<uint64_t>(device.vendor_id) << 16) | device.device_id,
                         state.capacity_sectors);
    return true;
}

bool probe_virtio_blk_pci_driver(VirtualMemory& kernel_vm,
                                 PageFrameContainer& frames,
                                 const PciDevice& device,
                                 size_t device_index,
                                 DeviceId id)
{
    (void)id;
    return probe_virtio_blk_device(
        kernel_vm, frames, device, device_index, g_platform.virtio_blk_public);
}

void remove_virtio_blk_device(DeviceId id)
{
    if(!g_virtio_blk.present || (id.bus != g_virtio_blk.owner.bus) ||
       (id.index != g_virtio_blk.owner.index))
    {
        return;
    }

    virtio_blk_release_runtime_resources(page_frames, g_virtio_blk);
    device_binding_remove(id);
    g_platform.block_device = nullptr;
    memset(&g_platform.virtio_blk_public, 0, sizeof(g_platform.virtio_blk_public));
    g_virtio_blk = {};
    g_virtio_blk_device = {};
}

const PciDriver& virtio_blk_pci_driver()
{
    static const PciDriver driver{
        .name = "virtio-blk",
        .matches = kVirtioBlkMatches,
        .match_count = sizeof(kVirtioBlkMatches) / sizeof(kVirtioBlkMatches[0]),
        .probe = probe_virtio_blk_pci_driver,
        .remove = remove_virtio_blk_device,
    };
    return driver;
}

const BlockDevice* virtio_blk_block_device()
{
    return g_virtio_blk.present ? &g_virtio_blk_device : nullptr;
}

bool run_virtio_blk_smoke()
{
    if(!g_virtio_blk.present)
    {
        return true;
    }
    uint8_t signature[512]{};
    if(!block_read_sync(g_virtio_blk_device, 0, signature, 1))
        return false;
    if(memcmp(signature, kVirtioSector0Prefix, strlen(kVirtioSector0Prefix)) != 0)
    {
        debug("virtio-blk: non-fixture disk; scratch smoke skipped")();
        return true;
    }
    if(g_virtio_blk.capacity_sectors < 4u)
    {
        debug("virtio-blk: test disk too small")();
        return false;
    }
    if(!verify_virtio_blk_prefix(g_virtio_blk_device, 0, kVirtioSector0Prefix) ||
       !verify_virtio_blk_prefix(g_virtio_blk_device, 1, kVirtioSector1Prefix) ||
       !verify_virtio_blk_write(g_virtio_blk_device) ||
       !verify_virtio_blk_multi_sector_read(g_virtio_blk_device) ||
       !verify_virtio_blk_multi_sector_write(g_virtio_blk_device))
    {
        return false;
    }
    debug("virtio-blk smoke ok")();
    return true;
}

bool run_virtio_blk_threaded_smoke()
{
    if(!g_virtio_blk.present)
    {
        return true;
    }

    BlockDevice* device = const_cast<BlockDevice*>(virtio_blk_block_device());
    uint8_t signature[512]{};
    if(!device || !block_read_sync(*device, 0, signature, 1))
        return false;
    if(memcmp(signature, kVirtioSector0Prefix, strlen(kVirtioSector0Prefix)) != 0)
        return true;
    const bool ok =
        (nullptr != device) && verify_virtio_blk_prefix(*device, 0, kVirtioSector0Prefix) &&
        verify_virtio_blk_write(*device) && verify_virtio_blk_multi_sector_read(*device) &&
        verify_virtio_blk_multi_sector_write(*device);
    debug(ok ? "virtio-blk threaded smoke ok" : "virtio-blk threaded smoke failed")();
    return ok;
}
