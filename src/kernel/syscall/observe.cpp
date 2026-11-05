#include "syscall/observe.hpp"

#include <os1/observe.h>

#include "arch/x86_64/cpu/cpu.hpp"
#include "core/kernel_state.hpp"
#include "debug/event_ring.hpp"
#include "drivers/bus/device.hpp"
#include "drivers/bus/resource.hpp"
#include "fs/initrd.hpp"
#include "mm/kmem.hpp"
#include "mm/dma.hpp"
#include "mm/user_copy.hpp"
#include "platform/platform.hpp"
#include "platform/topology.hpp"
#include "proc/thread.hpp"
#include "sync/smp.hpp"
#include "util/fixed_string.hpp"

namespace
{
[[nodiscard]] size_t count_active_processes()
{
    IrqSpinGuard guard(g_process_table_lock);
    size_t count = 0;
    for(Process* process = first_process(); nullptr != process; process = next_process(process))
    {
        if(ProcessState::Free != process->state)
        {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] uint32_t count_active_device_bindings()
{
    uint32_t count = 0;
    const DeviceBinding* bindings = device_bindings();
    const size_t binding_count = device_binding_count();
    for(size_t i = 0; i < binding_count; ++i)
    {
        if(bindings[i].active)
        {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] uint32_t count_active_acpi_devices()
{
    uint32_t count = 0;
    const AcpiDeviceInfo* devices = platform_acpi_devices();
    const size_t device_count = platform_acpi_device_count();
    for(size_t i = 0; i < device_count; ++i)
    {
        if(devices[i].active && (0 != (devices[i].status & 0x1u)))
        {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] uint32_t count_active_irq_routes()
{
    uint32_t count = 0;
    const IrqRoute* routes = platform_irq_routes();
    const size_t route_count = platform_irq_route_count();
    for(size_t i = 0; i < route_count; ++i)
    {
        if(routes[i].active)
        {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] uint32_t count_active_pci_bar_claims()
{
    uint32_t count = 0;
    const PciBarClaim* claims = pci_bar_claims();
    const size_t claim_count = pci_bar_claim_count();
    for(size_t i = 0; i < claim_count; ++i)
    {
        if(claims[i].active)
        {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] uint32_t count_active_dma_allocations()
{
    uint32_t count = 0;
    const DmaAllocationRecord* allocations = dma_allocation_records();
    const size_t allocation_count = dma_allocation_count();
    for(size_t i = 0; i < allocation_count; ++i)
    {
        if(allocations[i].active)
        {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] uint32_t observe_console_kind(const TextDisplayBackend* text_display)
{
    if(nullptr == text_display)
    {
        return OS1_OBSERVE_CONSOLE_SERIAL;
    }

    switch(text_display->kind)
    {
        case TextDisplayBackendKind::VgaText:
            return OS1_OBSERVE_CONSOLE_VGA;
        case TextDisplayBackendKind::FramebufferText:
            return OS1_OBSERVE_CONSOLE_FRAMEBUFFER;
        default:
            return OS1_OBSERVE_CONSOLE_NONE;
    }
}

bool begin_observe_transfer(const ObserveContext& context,
                            Thread* thread,
                            uint64_t user_buffer,
                            size_t user_length,
                            uint32_t kind,
                            uint32_t record_size,
                            uint32_t record_count,
                            size_t& offset,
                            long& result)
{
    offset = 0;
    result = -1;
    if((nullptr == context.frames) || (nullptr == thread) || (0 == user_buffer) ||
       (0 == user_length))
    {
        return false;
    }

    const size_t payload_bytes =
        static_cast<size_t>(record_size) * static_cast<size_t>(record_count);
    if((0 != record_count) && ((payload_bytes / record_count) != record_size))
    {
        return false;
    }
    const size_t total_bytes = sizeof(Os1ObserveHeader) + payload_bytes;
    if(total_bytes < sizeof(Os1ObserveHeader) || (user_length < total_bytes))
    {
        return false;
    }

    const Os1ObserveHeader header{
        .abi_version = OS1_OBSERVE_ABI_VERSION,
        .kind = kind,
        .record_size = record_size,
        .record_count = record_count,
    };
    if(!copy_to_user(*context.frames, thread, user_buffer, &header, sizeof(header)))
    {
        return false;
    }

    offset = sizeof(header);
    result = static_cast<long>(total_bytes);
    return true;
}

bool write_observe_record(const ObserveContext& context,
                          Thread* thread,
                          uint64_t user_buffer,
                          size_t& offset,
                          const void* record,
                          size_t record_size)
{
    if((nullptr == context.frames) || (nullptr == thread) || (nullptr == record))
    {
        return false;
    }
    if(!copy_to_user(*context.frames, thread, user_buffer + offset, record, record_size))
    {
        return false;
    }
    offset += record_size;
    return true;
}

long sys_observe_system(const ObserveContext& context,
                        Thread* thread,
                        uint64_t user_buffer,
                        size_t length)
{
    size_t offset = 0;
    long result = -1;
    if(!begin_observe_transfer(context,
                               thread,
                               user_buffer,
                               length,
                               OS1_OBSERVE_SYSTEM,
                               sizeof(Os1ObserveSystemRecord),
                               1,
                               offset,
                               result))
    {
        return -1;
    }

    Os1ObserveSystemRecord record{};
    record.boot_source = context.boot_info ? static_cast<uint32_t>(context.boot_info->source) : 0;
    record.console_kind = observe_console_kind(context.text_display);
    record.tick_count = context.timer_ticks;
    {
        IrqSpinGuard guard(g_page_frames_lock);
        record.total_pages = context.frames ? context.frames->page_count() : 0;
        record.free_pages = context.frames ? context.frames->free_page_count() : 0;
    }
    record.process_count = static_cast<uint32_t>(count_active_processes());
    record.runnable_thread_count = static_cast<uint32_t>(runnable_thread_count());
    record.cpu_count = static_cast<uint32_t>(ncpu);
    record.pci_device_count = static_cast<uint32_t>(platform_pci_device_count());
    const VirtioBlkDevice* virtio_blk = platform_virtio_blk();
    record.virtio_blk_present = (nullptr != virtio_blk) ? 1u : 0u;
    record.virtio_blk_capacity_sectors = (nullptr != virtio_blk) ? virtio_blk->capacity_sectors : 0;
    copy_fixed_string(record.bootloader_name,
                      sizeof(record.bootloader_name),
                      (context.boot_info && context.boot_info->bootloader_name)
                          ? context.boot_info->bootloader_name
                          : "");
    return write_observe_record(context, thread, user_buffer, offset, &record, sizeof(record))
               ? result
               : -1;
}

long sys_observe_processes(const ObserveContext& context,
                           Thread* thread,
                           uint64_t user_buffer,
                           size_t length)
{
    uint32_t record_count = 0;
    {
        IrqSpinGuard process_guard(g_process_table_lock);
        IrqSpinGuard thread_guard(g_thread_registry_lock);
        for(Thread* entry = first_thread(); nullptr != entry; entry = next_thread(entry))
        {
            if((ThreadState::Free != entry->state) && (nullptr != entry->process))
            {
                ++record_count;
            }
        }
    }

    size_t offset = 0;
    long result = -1;
    if(!begin_observe_transfer(context,
                               thread,
                               user_buffer,
                               length,
                               OS1_OBSERVE_PROCESSES,
                               sizeof(Os1ObserveProcessRecord),
                               record_count,
                               offset,
                               result))
    {
        return -1;
    }

    IrqSpinGuard process_guard(g_process_table_lock);
    IrqSpinGuard thread_guard(g_thread_registry_lock);
    for(Thread* entry = first_thread(); nullptr != entry; entry = next_thread(entry))
    {
        if((ThreadState::Free == entry->state) || (nullptr == entry->process))
        {
            continue;
        }

        Os1ObserveProcessRecord record{};
        record.pid = entry->process->pid;
        record.tid = entry->tid;
        record.cr3 = entry->address_space_cr3;
        record.process_state = static_cast<uint32_t>(entry->process->state);
        record.thread_state = static_cast<uint32_t>(entry->state);
        record.flags =
            entry->user_mode ? static_cast<uint32_t>(OS1_OBSERVE_PROCESS_FLAG_USER_MODE) : 0u;
        copy_fixed_string(record.name, sizeof(record.name), entry->process->name);
        if(!write_observe_record(context, thread, user_buffer, offset, &record, sizeof(record)))
        {
            return -1;
        }
    }

    return result;
}

long sys_observe_cpus(const ObserveContext& context,
                      Thread* thread,
                      uint64_t user_buffer,
                      size_t length)
{
    uint32_t record_count = 0;
    for(cpu* entry = g_cpu_boot; nullptr != entry; entry = entry->next)
    {
        ++record_count;
    }

    size_t offset = 0;
    long result = -1;
    if(!begin_observe_transfer(context,
                               thread,
                               user_buffer,
                               length,
                               OS1_OBSERVE_CPUS,
                               sizeof(Os1ObserveCpuRecord),
                               record_count,
                               offset,
                               result))
    {
        return -1;
    }

    uint32_t logical_index = 0;
    for(cpu* entry = g_cpu_boot; nullptr != entry; entry = entry->next, ++logical_index)
    {
        Os1ObserveCpuRecord record{};
        record.logical_index = logical_index;
        record.apic_id = entry->id;
        record.flags = (entry == g_cpu_boot) ? static_cast<uint32_t>(OS1_OBSERVE_CPU_FLAG_BSP) : 0u;
        if((entry == g_cpu_boot) || (0 != entry->booted))
        {
            record.flags |= static_cast<uint32_t>(OS1_OBSERVE_CPU_FLAG_BOOTED);
        }
        if((nullptr != entry->current_thread) && (nullptr != entry->current_thread->process))
        {
            record.current_pid = entry->current_thread->process->pid;
            record.current_tid = entry->current_thread->tid;
        }
        {
            IrqSpinGuard guard(entry->runq.lock);
            record.runq_depth = static_cast<uint32_t>(entry->runq.length);
        }
        record.timer_ticks = entry->timer_ticks;
        record.idle_ticks = entry->idle_ticks;
        record.migrate_in = entry->migrate_in;
        record.migrate_out = entry->migrate_out;
        if(!write_observe_record(context, thread, user_buffer, offset, &record, sizeof(record)))
        {
            return -1;
        }
    }

    return result;
}

long sys_observe_pci(const ObserveContext& context,
                     Thread* thread,
                     uint64_t user_buffer,
                     size_t length)
{
    const size_t device_count = platform_pci_device_count();
    const PciDevice* devices = platform_pci_devices();

    size_t offset = 0;
    long result = -1;
    if(!begin_observe_transfer(context,
                               thread,
                               user_buffer,
                               length,
                               OS1_OBSERVE_PCI,
                               sizeof(Os1ObservePciRecord),
                               static_cast<uint32_t>(device_count),
                               offset,
                               result))
    {
        return -1;
    }

    for(size_t i = 0; i < device_count; ++i)
    {
        Os1ObservePciRecord record{};
        record.segment_group = devices[i].segment_group;
        record.bus = devices[i].bus;
        record.slot = devices[i].slot;
        record.function = devices[i].function;
        record.vendor_id = devices[i].vendor_id;
        record.device_id = devices[i].device_id;
        record.class_code = devices[i].class_code;
        record.subclass = devices[i].subclass;
        record.prog_if = devices[i].prog_if;
        record.revision = devices[i].revision;
        record.interrupt_line = devices[i].interrupt_line;
        record.interrupt_pin = devices[i].interrupt_pin;
        record.capability_pointer = devices[i].capability_pointer;
        record.bar_count = devices[i].bar_count;
        for(size_t bar_index = 0; bar_index < 6; ++bar_index)
        {
            record.bars[bar_index].base = devices[i].bars[bar_index].base;
            record.bars[bar_index].size = devices[i].bars[bar_index].size;
            record.bars[bar_index].type = static_cast<uint8_t>(devices[i].bars[bar_index].type);
        }
        if(!write_observe_record(context, thread, user_buffer, offset, &record, sizeof(record)))
        {
            return -1;
        }
    }

    return result;
}

long sys_observe_devices(const ObserveContext& context,
                         Thread* thread,
                         uint64_t user_buffer,
                         size_t length)
{
    const DeviceBinding* bindings = device_bindings();
    const size_t binding_count = device_binding_count();

    size_t offset = 0;
    long result = -1;
    if(!begin_observe_transfer(context,
                               thread,
                               user_buffer,
                               length,
                               OS1_OBSERVE_DEVICES,
                               sizeof(Os1ObserveDeviceRecord),
                               count_active_device_bindings() + count_active_acpi_devices(),
                               offset,
                               result))
    {
        return -1;
    }

    for(size_t i = 0; i < binding_count; ++i)
    {
        if(!bindings[i].active)
        {
            continue;
        }

        Os1ObserveDeviceRecord record{};
        record.bus = static_cast<uint8_t>(bindings[i].id.bus);
        record.state = static_cast<uint8_t>(bindings[i].state);
        record.id = bindings[i].id.index;
        record.pci_index = (DeviceBus::Pci == bindings[i].id.bus) ? bindings[i].pci_index
                                                                  : static_cast<uint16_t>(OS1_OBSERVE_INDEX_NONE);
        copy_fixed_string(record.driver_name,
                          sizeof(record.driver_name),
                          (nullptr != bindings[i].driver_name) ? bindings[i].driver_name : "");
        if(!write_observe_record(context, thread, user_buffer, offset, &record, sizeof(record)))
        {
            return -1;
        }
    }

    const AcpiDeviceInfo* acpi_devices = platform_acpi_devices();
    const size_t acpi_device_count = platform_acpi_device_count();
    for(size_t i = 0; i < acpi_device_count; ++i)
    {
        if(!acpi_devices[i].active || (0 == (acpi_devices[i].status & 0x1u)))
        {
            continue;
        }

        Os1ObserveDeviceRecord record{};
        record.bus = static_cast<uint8_t>(DeviceBus::Acpi);
        record.state = static_cast<uint8_t>(DeviceState::Discovered);
        record.id = static_cast<uint16_t>(i);
        record.pci_index = static_cast<uint16_t>(OS1_OBSERVE_INDEX_NONE);
        copy_fixed_string(record.driver_name,
                          sizeof(record.driver_name),
                          (0 != acpi_devices[i].hardware_id[0]) ? acpi_devices[i].hardware_id
                                                                : acpi_devices[i].name);
        if(!write_observe_record(context, thread, user_buffer, offset, &record, sizeof(record)))
        {
            return -1;
        }
    }

    return result;
}

long sys_observe_resources(const ObserveContext& context,
                           Thread* thread,
                           uint64_t user_buffer,
                           size_t length)
{
    const PciBarClaim* claims = pci_bar_claims();
    const size_t claim_count = pci_bar_claim_count();
    const DmaAllocationRecord* allocations = dma_allocation_records();
    const size_t allocation_count = dma_allocation_count();

    size_t offset = 0;
    long result = -1;
    if(!begin_observe_transfer(context,
                               thread,
                               user_buffer,
                               length,
                               OS1_OBSERVE_RESOURCES,
                               sizeof(Os1ObserveResourceRecord),
                               count_active_pci_bar_claims() + count_active_dma_allocations(),
                               offset,
                               result))
    {
        return -1;
    }

    for(size_t i = 0; i < claim_count; ++i)
    {
        if(!claims[i].active)
        {
            continue;
        }

        Os1ObserveResourceRecord record{};
        record.base = claims[i].base;
        record.size = claims[i].size;
        record.owner_id = claims[i].owner.index;
        record.reference_id = claims[i].pci_index;
        record.kind = OS1_OBSERVE_RESOURCE_PCI_BAR;
        record.owner_bus = static_cast<uint8_t>(claims[i].owner.bus);
        record.entry_index = claims[i].bar_index;
        record.detail = static_cast<uint8_t>(claims[i].type);
        if(!write_observe_record(context, thread, user_buffer, offset, &record, sizeof(record)))
        {
            return -1;
        }
    }

    for(size_t i = 0; i < allocation_count; ++i)
    {
        if(!allocations[i].active)
        {
            continue;
        }

        Os1ObserveResourceRecord record{};
        record.base = allocations[i].physical_base;
        record.size = allocations[i].size_bytes;
        record.flags = allocations[i].coherent
                   ? static_cast<uint32_t>(OS1_OBSERVE_RESOURCE_FLAG_COHERENT)
                   : 0u;
        record.page_count = allocations[i].page_count;
        record.owner_id = allocations[i].owner.index;
        record.reference_id = OS1_OBSERVE_INDEX_NONE;
        record.kind = OS1_OBSERVE_RESOURCE_DMA;
        record.owner_bus = static_cast<uint8_t>(allocations[i].owner.bus);
        record.detail = static_cast<uint8_t>(allocations[i].direction);
        if(!write_observe_record(context, thread, user_buffer, offset, &record, sizeof(record)))
        {
            return -1;
        }
    }

    return result;
}

long sys_observe_irqs(const ObserveContext& context,
                      Thread* thread,
                      uint64_t user_buffer,
                      size_t length)
{
    const IrqRoute* routes = platform_irq_routes();
    const size_t route_count = platform_irq_route_count();

    size_t offset = 0;
    long result = -1;
    if(!begin_observe_transfer(context,
                               thread,
                               user_buffer,
                               length,
                               OS1_OBSERVE_IRQS,
                               sizeof(Os1ObserveIrqRecord),
                               count_active_irq_routes(),
                               offset,
                               result))
    {
        return -1;
    }

    for(size_t i = 0; i < route_count; ++i)
    {
        if(!routes[i].active)
        {
            continue;
        }

        Os1ObserveIrqRecord record{};
        record.vector = routes[i].vector;
        record.kind = static_cast<uint8_t>(routes[i].kind);
        record.owner_bus = static_cast<uint8_t>(routes[i].owner.bus);
        record.source_irq = routes[i].source_irq;
        record.owner_id = routes[i].owner.index;
        record.source_id = routes[i].source_id;
        record.flags = routes[i].flags;
        record.gsi = routes[i].gsi;
        if(!write_observe_record(context, thread, user_buffer, offset, &record, sizeof(record)))
        {
            return -1;
        }
    }

    return result;
}

struct InitrdCountContext
{
    uint32_t count = 0;
};

bool count_initrd_record(const char*, const uint8_t*, uint64_t, void* context)
{
    auto* count = static_cast<InitrdCountContext*>(context);
    if(nullptr == count)
    {
        return false;
    }
    ++count->count;
    return true;
}

struct InitrdWriteContext
{
    const ObserveContext* observe_context = nullptr;
    Thread* thread = nullptr;
    uint64_t user_buffer = 0;
    size_t offset = 0;
};

bool write_initrd_record_callback(const char* archive_name,
                                  const uint8_t*,
                                  uint64_t file_size,
                                  void* context)
{
    auto* write = static_cast<InitrdWriteContext*>(context);
    if((nullptr == write) || (nullptr == write->observe_context))
    {
        return false;
    }

    Os1ObserveInitrdRecord record{};
    copy_initrd_path(record.path, sizeof(record.path), archive_name);
    record.size = file_size;
    return write_observe_record(*write->observe_context,
                                write->thread,
                                write->user_buffer,
                                write->offset,
                                &record,
                                sizeof(record));
}

long sys_observe_initrd(const ObserveContext& context,
                        Thread* thread,
                        uint64_t user_buffer,
                        size_t length)
{
    InitrdCountContext count{};
    if((nullptr != context.boot_info) && (context.boot_info->module_count > 0))
    {
        if(!for_each_initrd_file(count_initrd_record, &count))
        {
            return -1;
        }
    }

    size_t offset = 0;
    long result = -1;
    if(!begin_observe_transfer(context,
                               thread,
                               user_buffer,
                               length,
                               OS1_OBSERVE_INITRD,
                               sizeof(Os1ObserveInitrdRecord),
                               count.count,
                               offset,
                               result))
    {
        return -1;
    }

    if(0 == count.count)
    {
        return result;
    }

    InitrdWriteContext write{
        .observe_context = &context,
        .thread = thread,
        .user_buffer = user_buffer,
        .offset = offset,
    };
    if(!for_each_initrd_file(write_initrd_record_callback, &write))
    {
        return -1;
    }
    return result;
}

OS1_BSP_ONLY Os1ObserveEventRecord g_event_observe_snapshot[OS1_OBSERVE_EVENT_RING_CAPACITY];

long sys_observe_events(const ObserveContext& context,
                        Thread* thread,
                        uint64_t user_buffer,
                        size_t length)
{
    const uint32_t record_count =
        kernel_event::snapshot(g_event_observe_snapshot, OS1_OBSERVE_EVENT_RING_CAPACITY);

    size_t offset = 0;
    long result = -1;
    if(!begin_observe_transfer(context,
                               thread,
                               user_buffer,
                               length,
                               OS1_OBSERVE_EVENTS,
                               sizeof(Os1ObserveEventRecord),
                               record_count,
                               offset,
                               result))
    {
        return -1;
    }

    for(uint32_t i = 0; i < record_count; ++i)
    {
        if(!write_observe_record(context,
                                 thread,
                                 user_buffer,
                                 offset,
                                 &g_event_observe_snapshot[i],
                                 sizeof(g_event_observe_snapshot[i])))
        {
            return -1;
        }
    }

    return result;
}

long sys_observe_kmem(const ObserveContext& context,
                      Thread* thread,
                      uint64_t user_buffer,
                      size_t length)
{
    const uint32_t record_count = static_cast<uint32_t>(kmem_cache_stats_count());

    size_t offset = 0;
    long result = -1;
    if(!begin_observe_transfer(context,
                               thread,
                               user_buffer,
                               length,
                               OS1_OBSERVE_KMEM,
                               sizeof(Os1ObserveKmemRecord),
                               record_count,
                               offset,
                               result))
    {
        return -1;
    }

    for(uint32_t index = 0; index < record_count; ++index)
    {
        KmemCacheStats snapshot{};
        if(!kmem_get_cache_stats(index, snapshot))
        {
            return -1;
        }

        Os1ObserveKmemRecord record{};
        record.cache_index = index;
        record.object_size = static_cast<uint32_t>(snapshot.object_size);
        record.alignment = static_cast<uint32_t>(snapshot.alignment);
        record.slab_pages = snapshot.slab_count;
        record.slab_count = snapshot.slab_count;
        record.free_objects = snapshot.free_object_count;
        record.live_objects = snapshot.live_object_count;
        record.peak_live_objects = snapshot.peak_live_object_count;
        record.alloc_count = snapshot.alloc_count;
        record.free_count = snapshot.free_count;
        record.failed_alloc_count = snapshot.failed_alloc_count;
        copy_fixed_string(
            record.name, sizeof(record.name), (nullptr != snapshot.name) ? snapshot.name : "");
        if(!write_observe_record(context, thread, user_buffer, offset, &record, sizeof(record)))
        {
            return -1;
        }
    }

    return result;
}

long sys_observe_acpi(const ObserveContext& context,
                      Thread* thread,
                      uint64_t user_buffer,
                      size_t length)
{
    size_t offset = 0;
    long result = -1;
    if(!begin_observe_transfer(context,
                               thread,
                               user_buffer,
                               length,
                               OS1_OBSERVE_ACPI,
                               sizeof(Os1ObserveAcpiRecord),
                               1,
                               offset,
                               result))
    {
        return -1;
    }

    Os1ObserveAcpiRecord record{};

    const PciDevice* pci_devices = platform_pci_devices();
    const size_t pci_device_count = platform_pci_device_count();
    bool stored_route = false;
    for(size_t index = 0; index < pci_device_count; ++index)
    {
        const PciDevice& device = pci_devices[index];
        if(0 == device.interrupt_pin)
        {
            continue;
        }

        ++record.route_probe_count;
        const AcpiPciRoute* route =
            platform_cached_acpi_pci_route(device.bus,
                                           device.slot,
                                           device.function,
                                           static_cast<uint8_t>(device.interrupt_pin - 1u));
        if(nullptr == route)
        {
            continue;
        }

        ++record.route_success_count;
        if(!stored_route)
        {
            record.route_irq = route->irq;
            record.route_flags = route->flags;
            record.route_bus = device.bus;
            record.route_slot = device.slot;
            record.route_function = device.function;
            record.route_pin = static_cast<uint8_t>(device.interrupt_pin - 1u);
            record.route_source_is_gsi = route->source_is_gsi ? 1u : 0u;
            stored_route = true;
        }
    }

    // Legacy power counters stay zero. Observation never executes firmware methods.
    return write_observe_record(context, thread, user_buffer, offset, &record, sizeof(record))
               ? result
               : -1;
}
}  // namespace

long sys_observe(const ObserveContext& context, uint64_t kind, uint64_t user_buffer, size_t length)
{
    Thread* thread = current_thread();
    if(nullptr == thread)
    {
        return -1;
    }

    switch(kind)
    {
        case OS1_OBSERVE_SYSTEM:
            return sys_observe_system(context, thread, user_buffer, length);
        case OS1_OBSERVE_PROCESSES:
            return sys_observe_processes(context, thread, user_buffer, length);
        case OS1_OBSERVE_CPUS:
            return sys_observe_cpus(context, thread, user_buffer, length);
        case OS1_OBSERVE_PCI:
            return sys_observe_pci(context, thread, user_buffer, length);
        case OS1_OBSERVE_INITRD:
            return sys_observe_initrd(context, thread, user_buffer, length);
        case OS1_OBSERVE_EVENTS:
            return sys_observe_events(context, thread, user_buffer, length);
        case OS1_OBSERVE_DEVICES:
            return sys_observe_devices(context, thread, user_buffer, length);
        case OS1_OBSERVE_RESOURCES:
            return sys_observe_resources(context, thread, user_buffer, length);
        case OS1_OBSERVE_IRQS:
            return sys_observe_irqs(context, thread, user_buffer, length);
        case OS1_OBSERVE_KMEM:
            return sys_observe_kmem(context, thread, user_buffer, length);
        case OS1_OBSERVE_ACPI:
            return sys_observe_acpi(context, thread, user_buffer, length);
        default:
            return -1;
    }
}
