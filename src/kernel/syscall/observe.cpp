#include "syscall/observe.hpp"

#include <os1/observe.h>
#include <os1/resource.h>

#include "arch/x86_64/cpu/cpu.hpp"
#include "arch/x86_64/interrupt/interrupt.hpp"
#include "core/kernel_state.hpp"
#include "core/wall_clock.hpp"
#include "debug/event_ring.hpp"
#include "drivers/bus/device.hpp"
#include "drivers/bus/resource.hpp"
#include "drivers/usb/diagnostics.hpp"
#include "drivers/usb/stall_trace.hpp"
#include "fs/initrd.hpp"
#include "handoff/memory_layout.h"
#include "mm/dma.hpp"
#include "mm/kmem.hpp"
#include "mm/user_address.hpp"
#include "mm/user_copy.hpp"
#include "platform/hardware.hpp"
#include "platform/platform.hpp"
#include "platform/topology.hpp"
#include "proc/thread.hpp"
#include "sync/smp.hpp"
#include "util/fixed_string.hpp"

#if defined(OS1_HOST_TEST)
void (*host_observe_snapshot_hook)(uint32_t kind, void* state) = nullptr;
void* host_observe_snapshot_state = nullptr;
#endif

namespace
{
void observe_snapshot_checkpoint(uint32_t kind)
{
#if defined(OS1_HOST_TEST)
    if(host_observe_snapshot_hook)
        host_observe_snapshot_hook(kind, host_observe_snapshot_state);
#else
    (void)kind;
#endif
}

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
    if(total_bytes < sizeof(Os1ObserveHeader) || total_bytes > INT64_MAX ||
       user_length < total_bytes || !user_address::is_user_address_range(user_buffer, total_bytes))
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

// Capacity comes from both the registry size and the caller's byte budget.
template<class Record>
class ObserveSnapshot
{
public:
    ObserveSnapshot(PageFrameContainer* frames, size_t capacity)
        : capacity_(capacity), frames_(frames)
    {
        if(capacity <= kInlineRecords)
        {
            records_ = inline_records_;
            return;
        }

        const size_t bytes = capacity * sizeof(Record);
        const size_t pages = bytes / kPageSize + (bytes % kPageSize != 0);
        if(frames && pages <= UINT32_MAX &&
           frames->allocate(physical_, static_cast<unsigned>(pages)))
        {
            pages_ = static_cast<unsigned>(pages);
            records_ = kernel_physical_pointer<Record>(physical_);
        }
    }

    ~ObserveSnapshot()
    {
        for(unsigned page = 0; page < pages_; ++page)
            frames_->free(physical_ + page * kPageSize);
    }
    ObserveSnapshot(const ObserveSnapshot&) = delete;
    ObserveSnapshot& operator=(const ObserveSnapshot&) = delete;

    bool ready() const
    {
        return !capacity_ || records_;
    }
    Record* data()
    {
        return records_;
    }

    bool append(const Record& record)
    {
        if(count_ == capacity_)
            return false;
        records_[count_++] = record;
        return true;
    }

    template<class Source, class Convert>
    bool collect(bool (*visit)(bool (*)(void*, const Source&), void*), Convert convert)
    {
        struct Capture
        {
            ObserveSnapshot& snapshot;
            Convert convert;
        } capture{*this, convert};
        return visit(
            [](void* state, const Source& source) {
                auto& capture = *static_cast<Capture*>(state);
                return !source.active || capture.snapshot.append(capture.convert(source));
            },
            &capture);
    }

    long write(const ObserveContext& context,
               Thread* thread,
               uint64_t user_buffer,
               size_t length,
               uint32_t kind) const
    {
        return write(context, thread, user_buffer, length, kind, count_);
    }

    long write(const ObserveContext& context,
               Thread* thread,
               uint64_t user_buffer,
               size_t length,
               uint32_t kind,
               uint32_t count) const
    {
        size_t offset = 0;
        long result = -1;
        if(count > capacity_ ||
           !begin_observe_transfer(
               context, thread, user_buffer, length, kind, sizeof(Record), count, offset, result))
            return -1;
        if(count && !write_observe_record(
                        context, thread, user_buffer, offset, records_, sizeof(Record) * count))
            return -1;
        return result;
    }

private:
    static constexpr size_t kInlineRecords = 512 / sizeof(Record);
    static_assert(kInlineRecords > 0);
    size_t capacity_;
    PageFrameContainer* frames_;
    uint64_t physical_ = 0;
    unsigned pages_ = 0;
    Record* records_ = nullptr;
    Record inline_records_[kInlineRecords];
    uint32_t count_ = 0;
};

template<class Record>
size_t snapshot_capacity(size_t length, size_t hint)
{
    if(length < sizeof(Os1ObserveHeader))
        return 0;
    const size_t limit = (length - sizeof(Os1ObserveHeader)) / sizeof(Record);
    const size_t count = hint < limit ? hint : limit;
    return count < UINT32_MAX ? count : UINT32_MAX;
}

// Retry once with a fresh count if a registry grew before capture.
template<class Record, class Count, class Capture>
long observe_registry(const ObserveContext& context,
                      Thread* thread,
                      uint64_t user_buffer,
                      size_t length,
                      uint32_t kind,
                      Count count,
                      Capture capture)
{
    const size_t limit = snapshot_capacity<Record>(length, UINT32_MAX);
    for(unsigned attempt = 0; attempt < 2; ++attempt)
    {
        const size_t capacity = snapshot_capacity<Record>(length, count());
        ObserveSnapshot<Record> snapshot(context.frames, capacity);
        if(!snapshot.ready())
            return -1;
        observe_snapshot_checkpoint(kind);
        if(capture(snapshot))
            return snapshot.write(context, thread, user_buffer, length, kind);
        if(capacity == limit)
            break;
    }
    return -1;
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
    auto count = [] {
        size_t record_count = 0;
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

        return record_count;
    };
    auto capture = [](auto& snapshot) {
        {
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
                record.process_state = static_cast<uint32_t>(entry->process->state);
                record.thread_state = static_cast<uint32_t>(entry->state);
                record.flags = entry->user_mode
                                   ? static_cast<uint32_t>(OS1_OBSERVE_PROCESS_FLAG_USER_MODE)
                                   : 0u;
                copy_fixed_string(record.name, sizeof(record.name), entry->process->name);
                if(!snapshot.append(record))
                {
                    return false;
                }
            }
        }
        return true;
    };
    return observe_registry<Os1ObserveProcessRecord>(
        context, thread, user_buffer, length, OS1_OBSERVE_PROCESSES, count, capture);
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
        if((entry == g_cpu_boot) || (ApStartupState::Online == cpu_ap_startup_state(entry)))
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
    auto count = [] { return device_binding_count() + platform_acpi_device_count(); };
    auto capture = [](auto& snapshot) {
        if(!snapshot.collect(visit_device_bindings, [](const DeviceBinding& source) {
               Os1ObserveDeviceRecord record{};
               record.bus = static_cast<uint8_t>(source.id.bus);
               record.state = static_cast<uint8_t>(source.state);
               record.id = source.id.index;
               record.pci_index = (DeviceBus::Pci == source.id.bus)
                                      ? source.pci_index
                                      : static_cast<uint16_t>(OS1_OBSERVE_INDEX_NONE);
               copy_fixed_string(record.driver_name,
                                 sizeof(record.driver_name),
                                 (nullptr != source.driver_name) ? source.driver_name : "");
               return record;
           }))
            return false;

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
            if(!snapshot.append(record))
            {
                return false;
            }
        }

        return true;
    };
    return observe_registry<Os1ObserveDeviceRecord>(
        context, thread, user_buffer, length, OS1_OBSERVE_DEVICES, count, capture);
}

long sys_observe_resources(const ObserveContext& context,
                           Thread* thread,
                           uint64_t user_buffer,
                           size_t length)
{
    auto count = [] { return pci_bar_claim_count() + dma_allocation_count(); };
    auto capture = [](auto& snapshot) {
        if(!snapshot.collect(visit_pci_bar_claims, [](const PciBarClaim& source) {
               Os1ObserveResourceRecord record{};
               record.base = source.base;
               record.size = source.size;
               record.owner_id = source.owner.index;
               record.reference_id = source.pci_index;
               record.kind = OS1_OBSERVE_RESOURCE_PCI_BAR;
               record.owner_bus = static_cast<uint8_t>(source.owner.bus);
               record.entry_index = source.bar_index;
               record.detail = static_cast<uint8_t>(source.type);
               return record;
           }))
            return false;

        if(!snapshot.collect(visit_dma_allocations, [](const DmaAllocationRecord& source) {
               Os1ObserveResourceRecord record{};
               record.base = source.physical_base;
               record.size = source.size_bytes;
               record.flags =
                   source.coherent ? static_cast<uint32_t>(OS1_OBSERVE_RESOURCE_FLAG_COHERENT) : 0u;
               record.page_count = source.page_count;
               record.owner_id = source.owner.index;
               record.reference_id = OS1_OBSERVE_INDEX_NONE;
               record.kind = OS1_OBSERVE_RESOURCE_DMA;
               record.owner_bus = static_cast<uint8_t>(source.owner.bus);
               record.detail = static_cast<uint8_t>(source.direction);
               return record;
           }))
            return false;

        return true;
    };
    return observe_registry<Os1ObserveResourceRecord>(
        context, thread, user_buffer, length, OS1_OBSERVE_RESOURCES, count, capture);
}

long sys_observe_irqs(const ObserveContext& context,
                      Thread* thread,
                      uint64_t user_buffer,
                      size_t length)
{
    auto count = [] { return platform_irq_route_count(); };
    auto capture = [](auto& snapshot) {
        if(!snapshot.collect(visit_irq_routes, [](const IrqRoute& source) {
               Os1ObserveIrqRecord record{};
               record.vector = source.vector;
               record.kind = static_cast<uint8_t>(source.kind);
               record.owner_bus = static_cast<uint8_t>(source.owner.bus);
               record.source_irq = source.source_irq;
               record.owner_id = source.owner.index;
               record.source_id = source.source_id;
               record.flags = source.flags;
               record.gsi = source.gsi;
               return record;
           }))
            return false;

        return true;
    };
    return observe_registry<Os1ObserveIrqRecord>(
        context, thread, user_buffer, length, OS1_OBSERVE_IRQS, count, capture);
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

void redact_event_addresses(Os1ObserveEventRecord& event, uint64_t caller_pid)
{
    const bool own_event = caller_pid != 0 && event.pid == caller_pid;
    if(event.type == OS1_KERNEL_EVENT_TRAP)
    {
        const bool own_user_trap = own_event && (event.flags & OS1_KERNEL_EVENT_FLAG_USER);
        if(!own_user_trap || !user_address::is_user_address_range(event.arg2, 1))
            event.arg2 = 0;
        if(!own_user_trap || event.arg0 != T_PGFLT ||
           !user_address::is_user_address_range(event.arg3, 1))
            event.arg3 = 0;
    }
    else if(event.type == OS1_KERNEL_EVENT_USER_COPY_FAILURE &&
            (!own_event || !user_address::is_user_address_range(event.arg0, 1)))
    {
        event.arg0 = 0;
        event.arg3 = 0;
    }
}

long sys_observe_events(const ObserveContext& context,
                        Thread* thread,
                        uint64_t user_buffer,
                        size_t length)
{
    if(length < sizeof(Os1ObserveHeader))
        return -1;
    const auto capacity = static_cast<uint32_t>(
        snapshot_capacity<Os1ObserveEventRecord>(length, OS1_OBSERVE_EVENT_RING_CAPACITY));
    ObserveSnapshot<Os1ObserveEventRecord> snapshot(context.frames, capacity);
    if(!snapshot.ready())
        return -1;
    uint32_t count = 0;
    if(!kernel_event::snapshot_all(snapshot.data(), capacity, count))
        return -1;
    observe_snapshot_checkpoint(OS1_OBSERVE_EVENTS);
    const uint64_t caller_pid = thread->user_mode && thread->process ? thread->process->pid : 0;
    for(uint32_t i = 0; i < count; ++i)
        redact_event_addresses(snapshot.data()[i], caller_pid);
    return snapshot.write(context, thread, user_buffer, length, OS1_OBSERVE_EVENTS, count);
}

long sys_observe_usb(const ObserveContext& context,
                     Thread* thread,
                     uint64_t user_buffer,
                     size_t length)
{
    const uint32_t count = static_cast<uint32_t>(usb::diagnostics::count());
    size_t offset = 0;
    long result = -1;
    if(!begin_observe_transfer(context,
                               thread,
                               user_buffer,
                               length,
                               OS1_OBSERVE_USB,
                               sizeof(Os1ObserveUsbRecord),
                               count,
                               offset,
                               result))
        return -1;
    for(uint32_t i = 0; i < count; ++i)
        if(!write_observe_record(context,
                                 thread,
                                 user_buffer,
                                 offset,
                                 usb::diagnostics::at(i),
                                 sizeof(Os1ObserveUsbRecord)))
            return -1;
    return result;
}

long sys_observe_usb_input_failures(const ObserveContext& context,
                                    Thread* thread,
                                    uint64_t user_buffer,
                                    size_t length)
{
    const uint32_t count = usb::diagnostics::input_failure_count();
    size_t offset = 0;
    long result = -1;
    if(!begin_observe_transfer(context,
                               thread,
                               user_buffer,
                               length,
                               OS1_OBSERVE_USB_INPUT_FAILURES,
                               sizeof(Os1ObserveUsbInputFailure),
                               count,
                               offset,
                               result))
        return -1;
    // The captured prefix is immutable. Never hold the diagnostic lock during user copies.
    for(uint32_t i = 0; i < count; ++i)
    {
        Os1ObserveUsbInputFailure record{};
        if(!usb::diagnostics::input_failure_at(i, record) ||
           !write_observe_record(context, thread, user_buffer, offset, &record, sizeof(record)))
            return -1;
    }
    return result;
}

long sys_observe_ehci_trace(const ObserveContext& context,
                            Thread* thread,
                            uint64_t user_buffer,
                            size_t length)
{
    Os1ObserveEhciStallTrace trace{};
    if(!usb::stall_trace::snapshot(trace))
        return OS1_ERR_NOT_FOUND;
    size_t offset = 0;
    long result = -1;
    if(!begin_observe_transfer(context,
                               thread,
                               user_buffer,
                               length,
                               OS1_OBSERVE_EHCI_STALL_TRACE,
                               sizeof(trace),
                               1,
                               offset,
                               result) ||
       !write_observe_record(context, thread, user_buffer, offset, &trace, sizeof(trace)))
        return -1;
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
        case OS1_OBSERVE_HARDWARE: {
            Os1HardwareQuery request{};
            if(length != sizeof(request) || !context.frames ||
               !copy_from_user(*context.frames, thread, user_buffer, &request, sizeof(request)))
                return OS1_ERR_INVALID;
            long result = hardware_query(request);
            if(result < 0)
                return result;
            return copy_to_user(*context.frames, thread, user_buffer, &request, sizeof(request))
                       ? 0
                       : OS1_ERR_FAULT;
        }
        case OS1_OBSERVE_WALL_CLOCK: {
            Os1ObserveWallClock result{};
            if(length != sizeof(result) || !context.frames)
                return OS1_ERR_INVALID;
            if(!wall_clock_now(result.unix_ns))
                return OS1_ERR_UNSUPPORTED;
            return copy_to_user(*context.frames, thread, user_buffer, &result, sizeof(result))
                       ? static_cast<long>(sizeof(result))
                       : OS1_ERR_FAULT;
        }
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
        case OS1_OBSERVE_USB:
            return sys_observe_usb(context, thread, user_buffer, length);
        case OS1_OBSERVE_EHCI_STALL_TRACE:
            return sys_observe_ehci_trace(context, thread, user_buffer, length);
        case OS1_OBSERVE_USB_INPUT_FAILURES:
            return sys_observe_usb_input_failures(context, thread, user_buffer, length);
        case OS1_OBSERVE_ACPI:
            return sys_observe_acpi(context, thread, user_buffer, length);
        default:
            return -1;
    }
}
