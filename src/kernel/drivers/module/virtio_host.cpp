// Bounded generic virtio queue adapter for the first kernel module profile.
#include "drivers/module/virtio_host.hpp"

#include "core/kernel_state.hpp"
#include "debug/debug.hpp"
#include "debug/event_ring.hpp"
#include "drivers/bus/resource.hpp"
#include "drivers/timer/tsc_clock.hpp"
#include "drivers/virtio/pci_transport.hpp"
#include "drivers/virtio/virtqueue.hpp"
#include "handoff/memory_layout.h"
#include "mm/virtual_memory.hpp"
#include "platform/pci_config.hpp"
#include "platform/platform.hpp"
#include "util/memory.h"

extern PageFrameContainer page_frames;
namespace
{
struct Queue
{
    Virtqueue ring{};
    DmaBuffer buffer{};
    uint32_t busy = 0;
    bool device_writes = false;
    bool configured = false;
};
struct Transport
{
    bool allocated = false;
    uint32_t active = 0;
    uint32_t stopping = 0;
    uint32_t failed = 0;
    uint32_t pci_index = 0;
    uint32_t config_bytes = 0;
    VirtioPciTransport pci{};
    Queue queues[2]{};
    void (*callback)(void*) = nullptr;
    void* context = nullptr;
};
// Stable storage: pending IRQ callbacks and uncertain DMA can never address freed memory.
// One admission per boot bounds both normal retained resources and failure quarantine.
Transport state;
uint64_t now_ns()
{
    return tsc_clock_now_100ns() * 100;
}
int stop(void* handle)
{
    if(handle != &state)
        return 0;
    __atomic_store_n(&state.stopping, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&state.active, 0, __ATOMIC_RELEASE);
    bool retired = true;
    if(state.pci.common_cfg)
    {
        virtio_pci_write_device_status(state.pci, 0);
        uint64_t start = now_ns();
        while(state.pci.common_cfg->device_status)
        {
            if(now_ns() - start >= 100000000)
            {
                retired = false;
                break;
            }
            asm volatile("pause");
        }
    }
    if(state.pci.device)
    {
        pci_release_interrupt(*state.pci.device, state.pci.interrupt);
        auto command = pci_config_read16(*state.pci.device, 4);
        pci_config_write16(*state.pci.device, 4, uint16_t(command & ~4u));
    }
    // Retain queues, BAR ownership and callback storage even after a reset acknowledgement.
    // Generic cross-CPU IRQ reclamation is not yet an implemented contract.
    debug(retired ? "driver: virtio transport stopped; resources retained"
                  : "driver: virtio DMA quarantine; reboot required")();
    return retired;
}
void irq(void*)
{
    if(state.pci.isr_status && state.pci.interrupt.mode == PciInterruptMode::LegacyIntx)
        (void)*state.pci.isr_status;
    if(__atomic_load_n(&state.active, __ATOMIC_ACQUIRE) && state.callback)
        state.callback(state.context);
}
void* open(uint32_t index, uint32_t config_bytes, uint64_t features)
{
    if(state.allocated || index >= platform_pci_device_count() || config_bytes == 0 ||
       config_bytes > 256)
        return nullptr;
    state.allocated = true;
    state.pci_index = index;
    state.config_bytes = config_bytes;
    auto& device = platform_pci_devices()[index];
    VirtualMemory vm(page_frames, g_kernel_root_cr3);
    if(!virtio_pci_bind_transport(
           vm, DeviceId{DeviceBus::Pci, uint16_t(index)}, device, config_bytes, state.pci))
    {
        stop(&state);
        return nullptr;
    }
    virtio_pci_write_device_status(state.pci, 0);
    // Reset is asynchronous. Do not reuse queue state until the device acknowledges it.
    auto begin = now_ns();
    while(state.pci.common_cfg->device_status)
    {
        if(now_ns() - begin >= 100000000)
        {
            stop(&state);
            return nullptr;
        }
        asm volatile("pause");
    }
    virtio_pci_write_device_status(state.pci, kVirtioStatusAcknowledge | kVirtioStatusDriver);
    if(!virtio_pci_negotiate_features(state.pci, features, features))
    {
        stop(&state);
        return nullptr;
    }
    return &state;
}
int read_config(void* handle, uint32_t offset, void* out, uint32_t bytes)
{
    if(handle != &state || !out || offset > state.config_bytes ||
       bytes > state.config_bytes - offset || !state.pci.device_cfg)
        return 0;
    for(unsigned attempt = 0; attempt < 8; ++attempt)
    {
        uint8_t generation = state.pci.common_cfg->config_generation;
        for(unsigned i = 0; i < bytes; ++i)
            static_cast<uint8_t*>(out)[i] = state.pci.device_cfg[offset + i];
        if(generation == state.pci.common_cfg->config_generation)
            return 1;
    }
    return 0;
}
uint8_t* queue(void* handle, uint32_t index, uint32_t writes, uint32_t* capacity)
{
    if(handle != &state || index >= 2 || writes > 1 || !capacity || state.queues[index].configured)
        return nullptr;
    auto& q = state.queues[index];
    auto* common = state.pci.common_cfg;
    common->queue_select = uint16_t(index);
    auto size = common->queue_size;
    if(!size)
        return nullptr;
    if(size > kVirtqueueMaxSize)
        size = kVirtqueueMaxSize;
    if(!virtqueue_allocate(page_frames, state.pci.owner, size, q.ring) ||
       !dma_allocate_buffer(page_frames,
                            state.pci.owner,
                            2048,
                            writes ? DmaDirection::FromDevice : DmaDirection::ToDevice,
                            q.buffer))
        return nullptr;
    memset(q.buffer.virtual_address, 0, q.buffer.size_bytes);
    if(!virtio_pci_setup_queue(state.pci,
                               uint16_t(index),
                               size,
                               q.ring.ring_memory.physical_address,
                               q.ring.ring_memory.physical_address + kPageSize,
                               q.ring.ring_memory.physical_address + 2 * kPageSize))
        return nullptr;
    q.device_writes = writes != 0;
    q.configured = true;
    *capacity = uint32_t(q.buffer.size_bytes);
    return static_cast<uint8_t*>(q.buffer.virtual_address);
}
int interrupt(void* handle, void (*callback)(void*), void* context)
{
    if(handle != &state || !callback || state.callback || !state.queues[0].configured ||
       !state.queues[1].configured)
        return 0;
    state.callback = callback;
    state.context = context;
    VirtualMemory vm(page_frames, g_kernel_root_cr3);
    if(!virtio_pci_bind_queue_interrupt(vm, state.pci, 0, 0, irq, nullptr))
        return 0;
    for(unsigned i = 0; i < 2; ++i)
    {
        state.pci.common_cfg->queue_select = uint16_t(i);
        auto vector = state.pci.interrupt.mode == PciInterruptMode::Msix ? 0 : kVirtioNoVector;
        state.pci.common_cfg->queue_msix_vector = uint16_t(vector);
        if(state.pci.common_cfg->queue_msix_vector != vector)
            return 0;
    }
    return 1;
}
int start(void* handle)
{
    if(handle != &state || !state.callback || !state.queues[0].configured ||
       !state.queues[1].configured || __atomic_load_n(&state.failed, __ATOMIC_ACQUIRE) ||
       __atomic_load_n(&state.stopping, __ATOMIC_ACQUIRE))
        return 0;
    __atomic_store_n(&state.active, 1, __ATOMIC_RELEASE);
    virtio_pci_write_device_status(
        state.pci, uint8_t(state.pci.common_cfg->device_status | kVirtioStatusDriverOk));
    auto& device = *state.pci.device;
    kernel_event::record(OS1_KERNEL_EVENT_PCI_BIND,
                         OS1_KERNEL_EVENT_FLAG_SUCCESS,
                         state.pci_index,
                         pci_bdf(device),
                         (uint64_t(device.vendor_id) << 16) | device.device_id,
                         state.queues[0].ring.size);
    return 1;
}
int submit(void* handle, uint32_t index, uint32_t bytes)
{
    if(handle != &state || index >= 2 || __atomic_load_n(&state.failed, __ATOMIC_ACQUIRE) ||
       __atomic_load_n(&state.stopping, __ATOMIC_ACQUIRE))
        return 0;
    auto& q = state.queues[index];
    if(!q.configured || !bytes || bytes > q.buffer.size_bytes)
        return 0;
    uint32_t expected = 0;
    if(!__atomic_compare_exchange_n(
           &q.busy, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 0;
    q.ring.desc[0] = {
        q.buffer.physical_address, bytes, uint16_t(q.device_writes ? kVirtqDescFlagWrite : 0), 0};
    dma_sync_for_device(q.buffer);
    dma_sync_for_device(q.ring.ring_memory);
    if(!virtqueue_submit(q.ring, 0))
    {
        __atomic_store_n(&q.busy, 0, __ATOMIC_RELEASE);
        return 0;
    }
    virtio_pci_notify_queue(state.pci, uint16_t(index));
    return 1;
}
int complete(void* handle, uint32_t index, uint32_t* bytes)
{
    if(handle != &state || index >= 2 || !bytes ||
       !__atomic_load_n(&state.active, __ATOMIC_ACQUIRE))
        return -1;
    auto& q = state.queues[index];
    dma_sync_for_cpu(q.ring.ring_memory);
    VirtqUsedElem used{};
    if(!virtqueue_consume_used(q.ring, used))
        return 0;
    if(used.id != 0 || used.len > q.buffer.size_bytes ||
       !__atomic_load_n(&q.busy, __ATOMIC_ACQUIRE))
    {
        __atomic_store_n(&state.failed, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&state.active, 0, __ATOMIC_RELEASE);
        return -1;
    }
    dma_sync_for_cpu(q.buffer);
    *bytes = used.len;
    __atomic_store_n(&q.busy, 0, __ATOMIC_RELEASE);
    return 1;
}
void log(const char* text)
{
    debug(text)();
}
void received(uint32_t bytes, uint32_t type)
{
    kernel_event::record(OS1_KERNEL_EVENT_NET_RX,
                         OS1_KERNEL_EVENT_FLAG_SUCCESS,
                         bytes,
                         type,
                         state.pci_index,
                         state.pci.interrupt.vector);
}
void checked(uint32_t bytes, uint32_t type, uint32_t count)
{
    debug("virtio-net smoke ok bytes=")(bytes)(" ethertype=0x")(
        type, 16, 4)(" arp-cache=")(count)();
}

}  // namespace
const os1_driver_host& driver_host_api()
{
    static const os1_driver_host api = {OS1_DRIVER_ABI,
                                        sizeof(os1_driver_host),
                                        open,
                                        read_config,
                                        queue,
                                        interrupt,
                                        start,
                                        submit,
                                        complete,
                                        stop,
                                        now_ns,
                                        log,
                                        received,
                                        checked};
    return api;
}
