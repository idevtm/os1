#include <os1/observe.h>
#include <stddef.h>
#include <stdint.h>

#include <os1/command_engine.hpp>
#include <os1/line_editor.hpp>
#include <os1/log.hpp>
#include <os1/parameter_client.hpp>
#include <os1/prompt.hpp>
#include <os1/shell_runtime.hpp>
#include <os1/syscall.hpp>
#include <os1/system.hpp>
#include <os1/vfs.hpp>

#include "interface/parameter_demo.hpp"
#include "shell/program_path.hpp"

namespace
{
constexpr size_t kShellLineBytes = os1::shell::line_capacity;
constexpr size_t kShellMaxTokens = 16;
int output_fd = 1;
bool output_failed = false;
bool command_failed = false;
char* log_output_buffer = nullptr;
size_t log_output_size = 0;
constexpr size_t kLogOutputBytes = 4096;
struct DiagnosticOutput
{
    int saved = output_fd;
    DiagnosticOutput()
    {
        output_fd = 2;
    }
    ~DiagnosticOutput()
    {
        output_fd = saved;
    }
};
constexpr size_t kShellObserveBufferBytes = 64 * 1024;

alignas(16) uint8_t g_observe_buffer[kShellObserveBufferBytes];

size_t string_length(const char* text)
{
    size_t length = 0;
    while((nullptr != text) && text[length])
    {
        ++length;
    }
    return length;
}

void write_bytes(const char* data, size_t length)
{
    if((nullptr == data) || (0 == length))
    {
        return;
    }
    if(log_output_buffer)
    {
        for(size_t i = 0; i < length; ++i)
        {
            log_output_buffer[log_output_size++] = data[i];
            if(log_output_size == kLogOutputBytes)
            {
                if(!os1::user::files::put(output_fd, log_output_buffer, log_output_size))
                    output_failed = true;
                log_output_size = 0;
            }
        }
        return;
    }
    if(output_fd == 1 && output_failed)
        return;
    if(!os1::user::files::put(output_fd, data, length) && output_fd == 1)
    {
        output_failed = true;
        os1::user::files::message("shell: output failed; file may be partial\n");
    }
}

void write_string(const char* text)
{
    write_bytes(text, string_length(text));
}

void write_char(char c)
{
    write_bytes(&c, 1);
}

void write_unsigned_base(uint64_t value, uint32_t base, size_t minimum_digits)
{
    char digits[32];
    size_t index = 0;
    do
    {
        const uint32_t digit = static_cast<uint32_t>(value % base);
        digits[index++] =
            (digit < 10u) ? static_cast<char>('0' + digit) : static_cast<char>('a' + (digit - 10u));
        value /= base;
    } while(value > 0);

    while(index < minimum_digits)
    {
        digits[index++] = '0';
    }

    while(index > 0)
    {
        write_char(digits[--index]);
    }
}

void write_unsigned(uint64_t value)
{
    write_unsigned_base(value, 10, 1);
}

void write_signed(long value)
{
    if(value < 0)
    {
        write_char('-');
        write_unsigned(static_cast<uint64_t>(-value));
        return;
    }
    write_unsigned(static_cast<uint64_t>(value));
}

void write_hex(uint64_t value, size_t digits = 1)
{
    write_unsigned_base(value, 16, digits);
}

bool is_space(char c)
{
    return (' ' == c) || ('\t' == c) || ('\n' == c) || ('\r' == c);
}

bool strings_equal(const char* lhs, const char* rhs)
{
    if((nullptr == lhs) || (nullptr == rhs))
    {
        return false;
    }
    for(size_t index = 0;; ++index)
    {
        if(lhs[index] != rhs[index])
        {
            return false;
        }
        if(0 == lhs[index])
        {
            return true;
        }
    }
}

size_t normalize_line(char* buffer, long count)
{
    size_t length = (count > 0) ? static_cast<size_t>(count) : 0;
    if(length >= kShellLineBytes)
    {
        length = kShellLineBytes - 1;
    }
    while((length > 0) && (('\n' == buffer[length - 1]) || ('\r' == buffer[length - 1])))
    {
        --length;
    }
    buffer[length] = 0;
    return length;
}

void write_observe_failure(const char* command)
{
    command_failed = true;
    DiagnosticOutput diagnostic_output;
    write_string("observe failed: ");
    write_string(command);
    write_char('\n');
}

const char* boot_source_name(uint32_t source)
{
    switch(source)
    {
        case 1:
            return "bios";
        case 2:
            return "limine";
        case 3:
            return "test";
        default:
            return "unknown";
    }
}

const char* console_kind_name(uint32_t kind)
{
    switch(kind)
    {
        case OS1_OBSERVE_CONSOLE_VGA:
            return "vga";
        case OS1_OBSERVE_CONSOLE_FRAMEBUFFER:
            return "framebuffer";
        case OS1_OBSERVE_CONSOLE_SERIAL:
            return "serial";
        default:
            return "none";
    }
}

const char* device_bus_name(uint32_t bus)
{
    switch(bus)
    {
        case 0:
            return "platform";
        case 1:
            return "pci";
        case 2:
            return "acpi";
        default:
            return "unknown";
    }
}

const char* process_state_name(uint32_t state)
{
    switch(state)
    {
        case 1:
            return "ready";
        case 2:
            return "running";
        case 3:
            return "dying";
        case 4:
            return "zombie";
        default:
            return "free";
    }
}

const char* thread_state_name(uint32_t state)
{
    switch(state)
    {
        case 1:
            return "ready";
        case 2:
            return "running";
        case 3:
            return "blocked";
        case 4:
            return "dying";
        default:
            return "free";
    }
}

const char* device_state_name(uint32_t state)
{
    switch(state)
    {
        case 0:
            return "discovered";
        case 1:
            return "probing";
        case 2:
            return "bound";
        case 3:
            return "started";
        case 4:
            return "stopping";
        case 5:
            return "removed";
        case 6:
            return "failed";
        default:
            return "unknown";
    }
}

const char* pci_bar_type_name(uint8_t type)
{
    switch(type)
    {
        case 1:
            return "mmio32";
        case 2:
            return "mmio64";
        case 3:
            return "io";
        default:
            return "unused";
    }
}

const char* irq_kind_name(uint8_t kind)
{
    switch(kind)
    {
        case 1:
            return "legacy-isa";
        case 2:
            return "local-apic";
        case 3:
            return "msi";
        case 4:
            return "msix";
        default:
            return "unknown";
    }
}

const char* resource_kind_name(uint8_t kind)
{
    switch(kind)
    {
        case OS1_OBSERVE_RESOURCE_PCI_BAR:
            return "bar";
        case OS1_OBSERVE_RESOURCE_DMA:
            return "dma";
        default:
            return "unknown";
    }
}

const char* dma_direction_name(uint8_t direction)
{
    switch(direction)
    {
        case 0:
            return "bidirectional";
        case 1:
            return "to-device";
        case 2:
            return "from-device";
        default:
            return "unknown";
    }
}

const char* event_type_name(uint32_t type)
{
    switch(type)
    {
        case OS1_KERNEL_EVENT_TRAP:
            return "trap";
        case OS1_KERNEL_EVENT_SCHED_TRANSITION:
            return "sched-transition";
        case OS1_KERNEL_EVENT_IRQ:
            return "irq";
        case OS1_KERNEL_EVENT_BLOCK_IO:
            return "block-io";
        case OS1_KERNEL_EVENT_PCI_BIND:
            return "pci-bind";
        case OS1_KERNEL_EVENT_USER_COPY_FAILURE:
            return "user-copy-failure";
        case OS1_KERNEL_EVENT_SMOKE_MARKER:
            return "smoke-marker";
        case OS1_KERNEL_EVENT_TIMER_SOURCE:
            return "timer-source";
        case OS1_KERNEL_EVENT_NET_RX:
            return "net-rx";
        case OS1_KERNEL_EVENT_KMEM_CORRUPTION:
            return "kmem-corruption";
        case OS1_KERNEL_EVENT_AP_ONLINE:
            return "ap-online";
        case OS1_KERNEL_EVENT_AP_TICK:
            return "ap-tick";
        case OS1_KERNEL_EVENT_IPI_RESCHED:
            return "ipi-resched";
        case OS1_KERNEL_EVENT_KERNEL_THREAD_PING:
            return "kernel-thread-ping";
        case OS1_KERNEL_EVENT_IPI_TLB_SHOOTDOWN:
            return "ipi-tlb-shootdown";
        case OS1_KERNEL_EVENT_THREAD_MIGRATE:
            return "thread-migrate";
        case OS1_KERNEL_EVENT_RUNQ_DEPTH:
            return "runq-depth";
        default:
            return "unknown";
    }
}

const char* timer_source_event_name(uint64_t mode)
{
    switch(mode)
    {
        case OS1_KERNEL_EVENT_TIMER_SOURCE_PIT:
            return "timer-source-pit";
        case OS1_KERNEL_EVENT_TIMER_SOURCE_LAPIC:
            return "timer-source-lapic";
        default:
            return "timer-source";
    }
}

const char* event_record_name(const Os1ObserveEventRecord& record)
{
    if(OS1_KERNEL_EVENT_TIMER_SOURCE == record.type)
    {
        return timer_source_event_name(record.arg0);
    }
    return event_type_name(record.type);
}

void write_bus_owner(uint32_t bus, uint64_t id)
{
    write_string(device_bus_name(bus));
    write_char(':');
    write_unsigned(id);
}

void write_yes_no(bool value)
{
    write_string(value ? "yes" : "no");
}

template<typename Record>
bool observe(uint32_t kind, const Record*& records, uint32_t& record_count)
{
    records = nullptr;
    record_count = 0;

    const long bytes = os1::user::observe(kind, g_observe_buffer, sizeof(g_observe_buffer));
    if(bytes < static_cast<long>(sizeof(Os1ObserveHeader)))
    {
        return false;
    }

    const auto* header = reinterpret_cast<const Os1ObserveHeader*>(g_observe_buffer);
    if((header->abi_version != OS1_OBSERVE_ABI_VERSION) || (header->kind != kind) ||
       (header->record_size != sizeof(Record)))
    {
        return false;
    }

    const size_t payload_bytes =
        static_cast<size_t>(header->record_count) * static_cast<size_t>(header->record_size);
    const size_t total_bytes = sizeof(Os1ObserveHeader) + payload_bytes;
    if((total_bytes > sizeof(g_observe_buffer)) || (static_cast<size_t>(bytes) != total_bytes))
    {
        return false;
    }

    record_count = header->record_count;
    records = reinterpret_cast<const Record*>(g_observe_buffer + sizeof(Os1ObserveHeader));
    return true;
}

void run_help()
{
    write_string("context [object.path]; Tab explores, Up/Down selects, Enter accepts\n");
    write_string("files: touch path; write [--append|--replace] path; edit path\n");
    write_string(
        "text: head source lines:10; tail source lines:10; grep pattern source; wc source; file "
        "path\n");
    write_string("pager: less path; space/enter/f next page, b previous page, q quit\n");
    write_string("streams: < file, > file, >> file, << delimiter; Ctrl-D ends input\n");
    write_string(
        "help echo pid sys ps cpu pci initrd devices usb irqs resources kmem events exec exit sync "
        "shutdown reboot\n");
    write_string(
        "call <image> <method> [field:value | field.case | field:{...}]\nintf <image>: offline "
        "constructor/method parameter help\n");
}

void run_pid()
{
    write_string("pid: ");
    const long pid = os1::user::getpid();
    if(pid < 0)
    {
        write_string("error\n");
        return;
    }
    write_unsigned(static_cast<uint64_t>(pid));
    write_char('\n');
}

void run_sys()
{
    const Os1ObserveSystemRecord* records = nullptr;
    uint32_t record_count = 0;
    if(!observe(OS1_OBSERVE_SYSTEM, records, record_count) || (1u != record_count))
    {
        write_observe_failure("sys");
        return;
    }

    const Os1ObserveSystemRecord& record = records[0];
    write_string("sys boot=");
    write_string(boot_source_name(record.boot_source));
    write_string(" console=");
    write_string(console_kind_name(record.console_kind));
    write_string(" ticks=");
    write_unsigned(record.tick_count);
    write_string(" pages=");
    write_unsigned(record.free_pages);
    write_char('/');
    write_unsigned(record.total_pages);
    write_string(" procs=");
    write_unsigned(record.process_count);
    write_string(" runnable=");
    write_unsigned(record.runnable_thread_count);
    write_string(" cpus=");
    write_unsigned(record.cpu_count);
    write_string(" pci=");
    write_unsigned(record.pci_device_count);
    write_string(" virtio_blk=");
    write_yes_no(0 != record.virtio_blk_present);
    if(0 != record.virtio_blk_present)
    {
        write_string(" sectors=");
        write_unsigned(record.virtio_blk_capacity_sectors);
    }
    if(0 != record.bootloader_name[0])
    {
        write_string(" bootloader=");
        write_string(record.bootloader_name);
    }
    write_char('\n');
}

void run_ps()
{
    const Os1ObserveProcessRecord* records = nullptr;
    uint32_t record_count = 0;
    if(!observe(OS1_OBSERVE_PROCESSES, records, record_count))
    {
        write_observe_failure("ps");
        return;
    }

    write_string("pid tid pstate tstate mode cr3 name\n");
    for(uint32_t i = 0; i < record_count; ++i)
    {
        write_unsigned(records[i].pid);
        write_char(' ');
        write_unsigned(records[i].tid);
        write_char(' ');
        write_string(process_state_name(records[i].process_state));
        write_char(' ');
        write_string(thread_state_name(records[i].thread_state));
        write_char(' ');
        write_string((0 != (records[i].flags & OS1_OBSERVE_PROCESS_FLAG_USER_MODE)) ? "user"
                                                                                    : "kernel");
        write_string(" 0x");
        write_hex(records[i].cr3, 16);
        write_char(' ');
        write_string(records[i].name);
        write_char('\n');
    }
}

void run_cpu()
{
    const Os1ObserveCpuRecord* records = nullptr;
    uint32_t record_count = 0;
    if(!observe(OS1_OBSERVE_CPUS, records, record_count))
    {
        write_observe_failure("cpu");
        return;
    }

    write_string("cpu slot apic bsp booted pid tid runq ticks idle mig_in mig_out\n");
    for(uint32_t i = 0; i < record_count; ++i)
    {
        write_string("cpu ");
        write_unsigned(records[i].logical_index);
        write_char(' ');
        write_hex(records[i].apic_id, 2);
        write_char(' ');
        write_yes_no(0 != (records[i].flags & OS1_OBSERVE_CPU_FLAG_BSP));
        write_char(' ');
        write_yes_no(0 != (records[i].flags & OS1_OBSERVE_CPU_FLAG_BOOTED));
        write_char(' ');
        write_unsigned(records[i].current_pid);
        write_char(' ');
        write_unsigned(records[i].current_tid);
        write_char(' ');
        write_unsigned(records[i].runq_depth);
        write_char(' ');
        write_unsigned(records[i].timer_ticks);
        write_char(' ');
        write_unsigned(records[i].idle_ticks);
        write_char(' ');
        write_unsigned(records[i].migrate_in);
        write_char(' ');
        write_unsigned(records[i].migrate_out);
        write_char('\n');
    }
}

void run_pci()
{
    const Os1ObservePciRecord* records = nullptr;
    uint32_t record_count = 0;
    if(!observe(OS1_OBSERVE_PCI, records, record_count))
    {
        write_observe_failure("pci");
        return;
    }

    write_string("pci bdf vendor:device class irq bars\n");
    for(uint32_t i = 0; i < record_count; ++i)
    {
        write_string("pci ");
        write_hex(records[i].bus, 2);
        write_char(':');
        write_hex(records[i].slot, 2);
        write_char('.');
        write_unsigned(records[i].function);
        write_char(' ');
        write_hex(records[i].vendor_id, 4);
        write_char(':');
        write_hex(records[i].device_id, 4);
        write_string(" class=");
        write_hex(records[i].class_code, 2);
        write_char(':');
        write_hex(records[i].subclass, 2);
        write_string(" irq=");
        write_unsigned(records[i].interrupt_line);
        write_char('/');
        write_unsigned(records[i].interrupt_pin);

        bool wrote_bar = false;
        for(uint8_t bar_index = 0; bar_index < records[i].bar_count && bar_index < 6; ++bar_index)
        {
            if((0 == records[i].bars[bar_index].base) || (0 == records[i].bars[bar_index].size))
            {
                continue;
            }
            write_string(wrote_bar ? "," : " bars=");
            write_string("bar");
            write_unsigned(bar_index);
            write_char(':');
            write_string(pci_bar_type_name(records[i].bars[bar_index].type));
            write_char('@');
            write_string("0x");
            write_hex(records[i].bars[bar_index].base, 1);
            write_char('+');
            write_string("0x");
            write_hex(records[i].bars[bar_index].size, 1);
            wrote_bar = true;
        }
        if(!wrote_bar)
        {
            write_string(" bars=none");
        }
        write_char('\n');
    }
}

void run_initrd()
{
    const Os1ObserveInitrdRecord* records = nullptr;
    uint32_t record_count = 0;
    if(!observe(OS1_OBSERVE_INITRD, records, record_count))
    {
        write_observe_failure("initrd");
        return;
    }

    write_string("initrd path size\n");
    for(uint32_t i = 0; i < record_count; ++i)
    {
        write_string("initrd ");
        write_string(records[i].path);
        write_string(" size=");
        write_unsigned(records[i].size);
        write_char('\n');
    }
    write_string("initrd complete\n");
}

void run_usb()
{
    const Os1ObserveUsbRecord* records = nullptr;
    uint32_t count = 0;
    if(!observe(OS1_OBSERVE_USB, records, count))
    {
        write_observe_failure("usb");
        return;
    }
    write_string("USB boot snapshot: controller topology and USB mounts\n");
    for(uint32_t i = 0; i < count; ++i)
    {
        const auto& r = records[i];
        // Empty ports add little to the first screen; the binary snapshot retains them.
        if(r.kind == OS1_OBSERVE_USB_ROOT_PORT && strings_equal(r.stage, "disconnected"))
            continue;
        write_string(r.kind == OS1_OBSERVE_USB_CONTROLLER  ? "controller"
                     : r.kind == OS1_OBSERVE_USB_ROOT_PORT ? "root"
                     : r.kind == OS1_OBSERVE_USB_DEVICE    ? "device"
                                                           : "mount");
        if(r.pci_index != OS1_OBSERVE_INDEX_NONE)
        {
            write_string(" pci=");
            write_unsigned(r.pci_index);
        }
        if(r.root_port)
        {
            write_string(" root=");
            write_unsigned(r.root_port);
        }
        if(r.port)
        {
            write_string(" port=");
            write_unsigned(r.port);
        }
        if(r.kind == OS1_OBSERVE_USB_DEVICE)
        {
            write_string(" addr=");
            write_unsigned(r.address);
            write_string(" parent=");
            write_unsigned(r.parent_address);
        }
        write_char(' ');
        write_string(r.stage);
        write_string(" status=0x");
        write_hex(r.status, 1);
        write_string(" detail=0x");
        write_hex(r.detail, 1);
        if(r.sectors)
        {
            write_string(" sectors=");
            write_unsigned(r.sectors);
        }
        write_char('\n');
    }
    if(!count)
        write_string("No captured USB topology or mount attempts\n");
}

void run_devices()
{
    const Os1ObserveDeviceRecord* records = nullptr;
    uint32_t record_count = 0;
    if(!observe(OS1_OBSERVE_DEVICES, records, record_count))
    {
        write_observe_failure("devices");
        return;
    }

    write_string("device bus id state pci driver\n");
    for(uint32_t i = 0; i < record_count; ++i)
    {
        write_string("device ");
        write_string(device_bus_name(records[i].bus));
        write_char(' ');
        write_unsigned(records[i].id);
        write_char(' ');
        write_string(device_state_name(records[i].state));
        write_char(' ');
        if(OS1_OBSERVE_INDEX_NONE == records[i].pci_index)
        {
            write_char('-');
        }
        else
        {
            write_unsigned(records[i].pci_index);
        }
        write_char(' ');
        write_string((0 != records[i].driver_name[0]) ? records[i].driver_name : "-");
        write_char('\n');
    }

    write_string("devices complete\n");
}

void run_irqs()
{
    const Os1ObserveIrqRecord* records = nullptr;
    uint32_t record_count = 0;
    if(!observe(OS1_OBSERVE_IRQS, records, record_count))
    {
        write_observe_failure("irqs");
        return;
    }

    write_string("irq vector kind owner source gsi flags\n");
    for(uint32_t i = 0; i < record_count; ++i)
    {
        write_string("irq ");
        write_unsigned(records[i].vector);
        write_char(' ');
        write_string(irq_kind_name(records[i].kind));
        write_char(' ');
        write_bus_owner(records[i].owner_bus, records[i].owner_id);
        write_string(" irq=");
        write_unsigned(records[i].source_irq);
        write_string(" id=");
        write_unsigned(records[i].source_id);
        write_string(" gsi=");
        write_unsigned(records[i].gsi);
        write_string(" flags=0x");
        write_hex(records[i].flags, 1);
        write_char('\n');
    }

    write_string("irqs complete\n");
}

void run_resources()
{
    const Os1ObserveResourceRecord* records = nullptr;
    uint32_t record_count = 0;
    if(!observe(OS1_OBSERVE_RESOURCES, records, record_count))
    {
        write_observe_failure("resources");
        return;
    }

    write_string("resource kind owner ref detail base size extra\n");
    for(uint32_t i = 0; i < record_count; ++i)
    {
        write_string("resource ");
        write_string(resource_kind_name(records[i].kind));
        write_char(' ');
        write_bus_owner(records[i].owner_bus, records[i].owner_id);
        write_char(' ');
        if(OS1_OBSERVE_RESOURCE_PCI_BAR == records[i].kind)
        {
            write_string("pci=");
            write_unsigned(records[i].reference_id);
            write_string(" bar=");
            write_unsigned(records[i].entry_index);
            write_string(" type=");
            write_string(pci_bar_type_name(records[i].detail));
        }
        else if(OS1_OBSERVE_RESOURCE_DMA == records[i].kind)
        {
            write_string("dir=");
            write_string(dma_direction_name(records[i].detail));
            write_string(" pages=");
            write_unsigned(records[i].page_count);
            write_string(" coherent=");
            write_yes_no(0 != (records[i].flags & OS1_OBSERVE_RESOURCE_FLAG_COHERENT));
        }
        else
        {
            write_string("detail=unknown");
        }
        write_string(" base=0x");
        write_hex(records[i].base, 1);
        write_string(" size=0x");
        write_hex(records[i].size, 1);
        write_char('\n');
    }

    write_string("resources complete\n");
}

void run_kmem()
{
    const Os1ObserveKmemRecord* records = nullptr;
    uint32_t record_count = 0;
    if(!observe(OS1_OBSERVE_KMEM, records, record_count))
    {
        write_observe_failure("kmem");
        return;
    }

    uint64_t slab_pages = 0;
    uint64_t free_objects = 0;
    uint64_t live_objects = 0;
    uint64_t failed_allocations = 0;
    for(uint32_t index = 0; index < record_count; ++index)
    {
        slab_pages += records[index].slab_pages;
        free_objects += records[index].free_objects;
        live_objects += records[index].live_objects;
        failed_allocations += records[index].failed_alloc_count;
    }

    write_string("kmem global caches=");
    write_unsigned(record_count);
    write_string(" slab_pages=");
    write_unsigned(slab_pages);
    write_string(" free=");
    write_unsigned(free_objects);
    write_string(" live=");
    write_unsigned(live_objects);
    write_string(" fail=");
    write_unsigned(failed_allocations);
    write_char('\n');

    for(uint32_t index = 0; index < record_count; ++index)
    {
        write_string("kmem cache ");
        write_string((0 != records[index].name[0]) ? records[index].name : "-");
        write_string(" obj=");
        write_unsigned(records[index].object_size);
        write_string(" align=");
        write_unsigned(records[index].alignment);
        write_string(" slabs=");
        write_unsigned(records[index].slab_count);
        write_string(" free=");
        write_unsigned(records[index].free_objects);
        write_string(" live=");
        write_unsigned(records[index].live_objects);
        write_string(" peak=");
        write_unsigned(records[index].peak_live_objects);
        write_string(" alloc=");
        write_unsigned(records[index].alloc_count);
        write_string(" frees=");
        write_unsigned(records[index].free_count);
        write_string(" fail=");
        write_unsigned(records[index].failed_alloc_count);
        write_char('\n');
    }

    write_string("kmem complete\n");
}

void run_events()
{
    const Os1ObserveEventRecord* records = nullptr;
    uint32_t record_count = 0;
    if(!observe(OS1_OBSERVE_EVENTS, records, record_count))
    {
        write_observe_failure("events");
        return;
    }

    write_string("event type seq tick cpu pid tid flags arg0 arg1 arg2 arg3\n");
    for(uint32_t i = 0; i < record_count; ++i)
    {
        write_string("event ");
        write_string(event_record_name(records[i]));
        write_string(" seq=");
        write_unsigned(records[i].sequence);
        write_string(" tick=");
        write_unsigned(records[i].tick_count);
        write_string(" cpu=");
        write_unsigned(records[i].cpu);
        write_string(" pid=");
        write_unsigned(records[i].pid);
        write_string(" tid=");
        write_unsigned(records[i].tid);
        write_string(" flags=0x");
        write_hex(records[i].flags, 1);
        write_string(" arg0=0x");
        write_hex(records[i].arg0, 1);
        write_string(" arg1=0x");
        write_hex(records[i].arg1, 1);
        write_string(" arg2=0x");
        write_hex(records[i].arg2, 1);
        write_string(" arg3=0x");
        write_hex(records[i].arg3, 1);
        write_char('\n');
    }
    write_string("events complete\n");
}

void run_unknown(const char* command)
{
    command_failed = true;
    DiagnosticOutput diagnostic_output;
    write_string("unknown command: ");
    write_string(command);
    write_char('\n');
}

long resolve_command_path(const char* command, char* path, size_t path_size)
{
    return os1::user::context::search(command, path, path_size);
}

void write_child_failure(const char* command, int status)
{
    command_failed = true;
    write_string("command failed: ");
    write_string(command);
    write_string(" status=");
    write_signed(status);
    write_char('\n');
}

const char* raw_tail(const char* raw, unsigned words);
void parameter_diagnostic(const os1::parameters::Diagnostic& d);
void run_exec(size_t argc, char* argv[kShellMaxTokens], const char* raw)
{
    command_failed = true;
    DiagnosticOutput diagnostic_output;
    if(argc < 2)
    {
        write_string("usage: exec <path> [args...]\n");
        return;
    }

    char path[OS1_PATH_MAX];
    os1::user::files::Handle executable(resolve_command_path(argv[1], path, sizeof(path)));
    if(executable.fd < 0)
    {
        write_string("exec failed: ");
        write_string(argv[1]);
        write_char('\n');
        return;
    }

    write_string("shell exec start ");
    write_string(path);
    write_char('\n');
    os1::parameters::Diagnostic diagnostic{};
    const long result = os1::user::run_program(path,
                                               argc - 1,
                                               argv + 1,
                                               true,
                                               0,
                                               nullptr,
                                               raw_tail(raw, 2),
                                               &diagnostic,
                                               executable.take());
    if(diagnostic.code != os1::parameters::Error::None)
        parameter_diagnostic(diagnostic);
    write_string("shell exec returned ");
    write_signed(result);
    write_char('\n');
}

void parameter_diagnostic(const os1::parameters::Diagnostic& d)
{
    command_failed = true;
    DiagnosticOutput diagnostic_output;
    write_string("parameter error code=");
    write_unsigned(static_cast<unsigned>(d.code));
    write_string(" path=");
    write_string(d.path);
    write_string(" bytes=");
    write_unsigned(d.begin);
    write_char(':');
    write_unsigned(d.end);
    write_string(" expected-type=");
    write_unsigned(d.expected_type);
    write_string(" ");
    write_string(d.explanation);
    write_char('\n');
}
// Only command words are separated here. The remaining bytes stay intact.
const char* raw_tail(const char* raw, unsigned words)
{
    while(words--)
    {
        while(is_space(*raw))
            ++raw;
        while(*raw && !is_space(*raw))
            ++raw;
    }
    while(is_space(*raw))
        ++raw;
    return raw;
}
void run_call(size_t argc, char** argv, const char* raw)
{
    command_failed = true;
    DiagnosticOutput diagnostic_output;
    if(argc < 3)
    {
        write_string("usage: call <image> <method> [raw named parameters]\n");
        return;
    }
    char path[OS1_PATH_MAX];
    long image = resolve_command_path(argv[1], path, sizeof(path));
    if(image < 0)
    {
        write_string("call: cannot open image\n");
        return;
    }
    static os1::parameters::Metadata schema, scratch;
    os1::parameters::Diagnostic d{};
    os1_constructor_args args{};
    if(os1::user::parameter_metadata(image, schema, scratch) != 1)
    {
        os1::user::close(image);
        write_string("call: invalid or absent rich metadata\n");
        return;
    }
    if(!os1::user::parameter_arguments(schema, raw_tail(raw, 3), args, d))
    {
        os1::user::close(image);
        parameter_diagnostic(d);
        return;
    }
    os1::user::ParameterClient client;
    long status = client.start(image, schema, d);
    os1::user::close(image);
    unsigned member = 0;
    if(status >= 0)
        for(unsigned i = 0; i < client.interface.member_count; ++i)
        {
            const auto& m = client.interface.members[i];
            if(m.kind == os1::interface::Method && m.flags && strings_equal(m.name, argv[2]))
                member = m.id;
        }
    os1_ipc_result result{};
    if(status >= 0)
        status =
            member ? client.call(member, args.payload, args.size, result) : OS1_ERR_UNSUPPORTED;
    client.stop();
    if(status < 0 || result.transport_status || result.application_status)
    {
        write_string("call failed status=");
        write_signed(status < 0                ? status
                     : result.transport_status ? result.transport_status
                                               : result.application_status);
        write_char('\n');
        return;
    }
    command_failed = false;
    output_fd = diagnostic_output.saved;
    if(client.interface.interface_id == os1::parameter_demo::interface_id &&
       member == os1::parameter_demo::Render && result.payload_bytes >= 8)
    {
        write_string("parameter method count=");
        write_unsigned(os1::constructor::decode(result.payload, 8));
        write_string(" text=");
        write_bytes(reinterpret_cast<const char*>(result.payload + 8), result.payload_bytes - 8);
        write_char('\n');
    }
    else
    {
        write_string("call result bytes=");
        write_unsigned(result.payload_bytes);
        write_string(" hex=");
        for(unsigned i = 0; i < result.payload_bytes; ++i)
            write_hex(result.payload[i], 2);
        write_char('\n');
    }
}

void run_external(size_t argc,
                  char** argv,
                  const char* raw,
                  const char* path,
                  int executable_fd,
                  const os1_constructor_args& constructor)
{
    command_failed = true;
    DiagnosticOutput diagnostic_output;
    const char* command = argv[0];
    os1::user::files::Handle executable(executable_fd);

    os1::parameters::Diagnostic diagnostic{};
    const long pid = os1::user::run_program(path,
                                            argc,
                                            argv,
                                            false,
                                            0,
                                            nullptr,
                                            raw_tail(raw, 1),
                                            &diagnostic,
                                            executable.take(),
                                            &constructor);
    if(pid < 0)
    {
        if(diagnostic.code != os1::parameters::Error::None)
        {
            parameter_diagnostic(diagnostic);
            return;
        }
        if(pid == OS1_ERR_INVALID)
        {
            write_string("invalid arguments or metadata: ");
            write_string(command);
            write_char('\n');
        }
        else
            run_unknown(command);
        return;
    }

    int status = 0;
    const long waited = os1::user::waitpid(static_cast<uint64_t>(pid), &status);
    if(waited != pid)
    {
        write_string("wait failed: ");
        write_string(command);
        write_char('\n');
        return;
    }

    command_failed = status != 0;
    if(status != 0)
        write_child_failure(command, status);
}
}  // namespace

namespace
{
struct ShellLifetime
{
    ShellLifetime()
    {
        os1::user::write(2, "shell started\n", 14);
    }
    ~ShellLifetime()
    {
        os1::user::commands::clear_bindings();
        os1::user::write(2, "shell ended\n", 12);
    }
};

bool history_enabled = true;
struct CommandRecord
{
    os1_log_command record{};
    bool executed = false;
    CommandRecord(const char* line, bool interactive)
    {
        record.version = 1;
        record.size = sizeof(record);
        if(interactive)
            record.flags |= OS1_LOG_INTERACTIVE;
        if(!history_enabled || (interactive && line[0] == ' '))
            record.flags |= OS1_LOG_SUPPRESSED;
        if(!(record.flags & OS1_LOG_SUPPRESSED))
        {
            size_t n = string_length(line);
            if(n >= sizeof(record.text))
                n = sizeof(record.text) - 1;
            __builtin_memcpy(record.text, line, n);
            os1::user::context::describe(os1::user::cwd(), record.cwd, sizeof(record.cwd));
        }
        if(os1::user::logs::context(OS1_LOG_COMMAND_BEGIN, &record) < 0)
            record.command_id = 0;
    }
    ~CommandRecord()
    {
        if(!record.command_id)
            return;
        record.status = (!executed || command_failed || output_failed) ? 1 : 0;
        os1::user::logs::context(OS1_LOG_COMMAND_END, &record);
    }
};
bool log_number(const char* s, uint64_t& value)
{
    value = 0;
    if(!*s)
        return false;
    for(; *s; ++s)
    {
        if(*s < '0' || *s > '9' || value > (UINT64_MAX - (*s - '0')) / 10)
            return false;
        value = value * 10 + (*s - '0');
    }
    return true;
}
struct LogOutput
{
    char bytes[kLogOutputBytes];
    LogOutput()
    {
        log_output_size = 0;
        log_output_buffer = bytes;
    }
    ~LogOutput()
    {
        log_output_buffer = nullptr;
        if(log_output_size && !os1::user::files::put(output_fd, bytes, log_output_size))
            output_failed = true;
        log_output_size = 0;
    }
};
void write_log_text(const char* text, size_t limit)
{
    const char hex[] = "0123456789abcdef";
    for(size_t i = 0; i < limit && text[i]; ++i)
    {
        unsigned char c = text[i];
        if(c < 32 || c >= 127 || c == '\\' || c == '"')
        {
            const char bytes[] = {'\\', 'x', hex[c >> 4], hex[c & 15]};
            write_bytes(bytes, 4);
        }
        else
            write_char(c);
    }
}
void run_log(bool history, size_t argc, char** argv)
{
    if(history && argc == 2 && (strings_equal(argv[1], "off") || strings_equal(argv[1], "on")))
    {
        history_enabled = strings_equal(argv[1], "on");
        return;
    }
    os1_log_query query{};
    query.version = 1;
    query.size = sizeof(query);
    uint64_t limit = 20;
    bool explicit_after = false;
    const bool information = argc == 2 && strings_equal(argv[1], "info");
    for(size_t i = 1; !information && i < argc; ++i)
    {
        const char* value = argv[i];
        while(*value && *value != ':')
            ++value;
        if(!*value)
        {
            command_failed = true;
            break;
        }
        size_t key = value++ - argv[i];
        uint64_t number = 0;
        if(key == 5 && os1::context::same(argv[i], 5, "level", 5))
        {
            query.min_level = strings_equal(value, "error")     ? OS1_LOG_ERROR
                              : strings_equal(value, "warning") ? OS1_LOG_WARNING
                              : strings_equal(value, "info")    ? OS1_LOG_INFO_LEVEL
                                                                : 99;
            if(query.min_level == 99)
                command_failed = true;
            continue;
        }
        if(!log_number(value, number))
        {
            command_failed = true;
            break;
        }
        if(key == 5 && os1::context::same(argv[i], 5, "limit", 5))
            limit = number;
        else if(key == 5 && os1::context::same(argv[i], 5, "after", 5))
        {
            query.after = number;
            explicit_after = true;
        }
        else if(key == 10 && os1::context::same(argv[i], 10, "command_id", 10))
            query.command_id = number;
        else if(key == 3 && os1::context::same(argv[i], 3, "pid", 3))
            query.pid = number;
        else if(key == 4 && os1::context::same(argv[i], 4, "kind", 4) && number <= OS1_LOG_TEXT)
            query.kind = number;
        else
            command_failed = true;
    }
    if(command_failed || !limit || limit > 256)
    {
        command_failed = true;
        os1::user::files::message("log: invalid filter; limit is 1..256\n");
        return;
    }
    os1::user::files::Handle client(history ? -1 : os1::user::logs::bind());
    os1_log_info info{};
    long status = history         ? os1::user::logs::context(OS1_LOG_CONTEXT_INFO, &info)
                  : client.fd < 0 ? client.fd
                                  : os1::user::logs::call(
                                        client.fd, OS1_LOG_INFO, nullptr, 0, &info, sizeof(info));
    if(status < 0)
    {
        command_failed = true;
        os1::user::files::message("log: access unavailable\n");
        return;
    }
    query.through = info.latest;
    LogOutput output;
    if(information)
    {
        write_string("log capacity_bytes=");
        write_unsigned(info.capacity_bytes);
        write_string(" latest=");
        write_unsigned(info.latest);
        write_string(" overwritten=");
        write_unsigned(info.overwritten);
        write_string(" dropped=");
        write_unsigned(info.dropped);
        write_string(" clock_resolution_ns=");
        write_unsigned(info.clock_resolution_ns);
        write_string(" diagnostic=");
        write_unsigned(info.diagnostic);
        write_char('\n');
        return;
    }
    // The default view is the recent sequence window. Explicit cursors allow older reads.
    if(!explicit_after && !query.command_id && !query.pid && !query.kind && !query.min_level &&
       info.latest > limit)
        query.after = info.latest - limit;
    for(uint64_t i = 0; i < limit && query.after < query.through; ++i)
    {
        os1_log_record record{};
        os1_log_history_query row{};
        row.query = query;
        status = history
                     ? os1::user::logs::context(OS1_LOG_HISTORY_READ, &row)
                     : os1::user::logs::call(
                           client.fd, OS1_LOG_READ, &query, sizeof(query), &record, sizeof(record));
        if(status <= 0)
        {
            if(status < 0)
                command_failed = true;
            break;
        }
        if(history)
            record = row.result.record;
        query.after = record.sequence;
        write_string(history ? "history " : "log ");
        write_unsigned(record.sequence);
        write_string(" time_ns=");
        write_unsigned(record.time_ns);
        write_string(" command=");
        write_unsigned(record.command_id);
        write_string(" pid=");
        write_unsigned(record.pid);
        write_string(" request=");
        write_unsigned(record.request_id);
        write_string(" session=");
        write_unsigned(record.session_id);
        write_string(" interface=");
        write_hex(record.interface_id);
        write_string(" operation=");
        write_unsigned(record.arg0);
        write_string(" duration_ns=");
        write_unsigned(record.duration_ns);
        write_string(" kind=");
        write_unsigned(record.kind);
        write_string(" status=");
        write_signed(record.status);
        if(record.flags & OS1_LOG_TRUNCATED)
            write_string(" truncated");
        if(history && (record.flags & OS1_LOG_PENDING))
            write_string(" pending");
        if(record.flags & OS1_LOG_SUPPRESSED)
            write_string(" [suppressed]");
        else
        {
            write_string(" text=\"");
            write_log_text(history ? row.result.text : record.text,
                           history ? sizeof(row.result.text) : sizeof(record.text));
            write_char('"');
        }
        write_char('\n');
    }
}

struct CommandSync
{
    bool pending;
    explicit CommandSync(bool enabled) : pending(enabled) {}
    bool finish()
    {
        if(!pending)
            return true;
        pending = false;
        // Binding checks SYSTEM_CONTROL directly; INSPECT is not required.
        if(os1::user::root() < 0)
            return true;
        long status = os1::user::system_control(OS1_SYSTEM_SYNC);
        if(status == OS1_ERR_ACCESS)
            return true;  // An attenuated shell retains no global sync authority.
        if(status >= 0)
            return true;
        os1::user::shell_runtime::diagnostic("sync failed; persistent data may be unclean");
        return false;
    }
    ~CommandSync()
    {
        finish();
    }
};
bool may_write(os1::user::shell_runtime::Method method)
{
    using M = os1::user::shell_runtime::Method;
    switch(method)
    {
        case M::Unknown:  // External programs, editors, and explicit exec can write.
        case M::Ensure:
        case M::Touch:
        case M::UpdateTimes:
        case M::Create:
        case M::Copy:
        case M::Move:
        case M::Remove:
        case M::MakeDirectory:
        case M::RemoveDirectory:
            return true;
        default:
            return false;
    }
}
}  // namespace

int main(int argc_main, char** argv_main)
{
    ShellLifetime lifetime;
    os1::user::commands::initialize();
    os1::user::discovery::extend = os1::user::commands::extend;
    os1::user::commands::extra_call = [](const char* name, const char* text) -> long {
        static os1::shell::Command command;
        if(!os1::shell::parse(text, command))
            return OS1_ERR_INVALID;
        run_log(
            os1::discovery::same(name, "process.shell.history.print"), command.argc, command.argv);
        return command_failed ? OS1_ERR_INVALID : 0;
    };
    if(os1::user::logs::context(OS1_LOG_CONTEXT_OPEN) < 0)
        os1::user::files::message("shell: history capture unavailable\n");
    if(argc_main == 2 && os1::discovery::same(argv_main[1], "--boot"))
    {
        argc_main = 1;
        os1::user::files::Handle log(os1::user::logs::bind());
        os1_log_info info{};
        if(log.fd >= 0 &&
           os1::user::logs::call(log.fd, OS1_LOG_INFO, nullptr, 0, &info, sizeof(info)) ==
               sizeof(info) &&
           info.diagnostic)
        {
            os1::user::files::message("shell: diagnostic tests starting\n");
            // Only automatic boot tests use this explicit deployment setting.
            os1::user::files::Handle mode(
                os1::user::open("/disk/etc/run-tests-mode", OS1_RIGHT_READ));
            char setting[12]{};
            const bool unattended = mode.fd >= 0 &&
                                    os1::user::read(mode.fd, setting, sizeof(setting)) == 11 &&
                                    os1::discovery::same(setting, "unattended\n");
            const char* command =
                unattended ? "run-tests --no-input --no-expect-usb-mouse" : "run-tests";
            if(unattended)
                os1::user::files::message("shell: diagnostic tests use unattended mode\n");
            CommandRecord record(command, false);
            record.executed = true;
            const char* arguments[] = {"run-tests", "--no-input", "--no-expect-usb-mouse"};
            const os1_constructor_args constructor{};
            long child = os1::user::run_program("/disk/bin/run-tests",
                                                unattended ? 3 : 1,
                                                arguments,
                                                false,
                                                0,
                                                nullptr,
                                                nullptr,
                                                nullptr,
                                                -1,
                                                &constructor);
            int status = 0;
            command_failed = child < 0 || os1::user::waitpid(child, &status) != child || status;
            os1::user::files::message(
                command_failed
                    ? "shell: diagnostic tests failed or unavailable; inspect /disk/tests\n"
                    : "shell: diagnostic tests completed; results in /disk/tests\n");
        }
    }
    char line[kShellLineBytes];
    os1::user::files::Reader reader;
    os1::user::files::Handle script;
    if(argc_main > 2)
    {
        os1::user::files::message("usage: sh [script]\n");
        return 1;
    }
    if(argc_main == 2)
    {
        script.reset(os1::user::files::open_file(argv_main[1], os1::user::files::Mode::Existing));
        if(script.fd < 0)
        {
            os1::user::files::message("shell: cannot open script\n");
            return 1;
        }
        reader.fd = script.fd;
    }
    os1::user::files::Line input;

    os1_resource_info input_info{};
    const bool console_input = argc_main != 2 && os1::user::query(0, &input_info) >= 0 &&
                               input_info.kind == OS1_OBJECT_CONSOLE;
    if(console_input)
        os1::user::files::message("shell prompt ready\n");
    for(;;)
    {
        output_failed = false;
        command_failed = false;
        long input_status =
            console_input ? os1::user::prompt::read(reader, input) : reader.line(input);
        if(input_status < 0)
        {
            os1::user::files::message("shell read failed\n");
            if(!console_input || input_status != OS1_ERR_RANGE)
                return 1;
            continue;
        }

        if(!input.size)
        {
            if(input.eof && !console_input)
                return 0;
            os1::user::files::message("\n");
            continue;
        }
        for(size_t i = 0; i < input.size; ++i)
            line[i] = input.bytes[i];
        normalize_line(line, input.size);
        CommandRecord command_record(line, console_input);
        if(os1::user::commands::handles(line))
        {
            command_record.executed = true;
            os1::user::commands::error = nullptr;
            os1::user::commands::last_status = 0;
            os1::user::commands::mutated = false;
            if(!os1::user::commands::run(line, console_input))
            {
                os1::user::shell_runtime::diagnostic(
                    os1::user::commands::error ? os1::user::commands::error : "command failed");
                command_failed = true;
                if(console_input && os1::user::prompt::last_guided)
                    os1::user::prompt::retain(line);
            }
            CommandSync command_sync(os1::user::commands::mutated);
            if(!command_sync.finish())
                command_failed = true;
            if(command_failed && !console_input)
                return 1;
            continue;
        }
        static os1::shell::Command parsed;
        const char* error = os1::shell::parse(line, parsed) ? nullptr : parsed.error;
        const char* recovery_marker = nullptr;
        if(error || parsed.input == os1::shell::Input::Here)
        {
            const char* legacy_here = os1::shell::find_top_level_command_operator(
                line, os1::shell::CommandOperator::LegacyHereDocument);
            const char* typed_here = os1::shell::find_top_level_command_operator(
                line, os1::shell::CommandOperator::HereDocument);
            if(legacy_here && legacy_here != typed_here)
            {
                const char* call = line;
                while(os1::shell::space(*call))
                    ++call;
                while(os1::shell::expression::Tree::identifier(*call) || *call == '.')
                    ++call;
                const char* outer = os1::shell::find_top_level_command_operator(line);
                // A legacy command may use bare parentheses as ordinary arguments.
                if(*call == '(' && call < legacy_here && outer && outer > legacy_here &&
                   *outer != '=')
                {
                    char& delimiter = line[outer - line];
                    const char saved = delimiter;
                    delimiter = 0;
                    const bool typed_prefix = os1::user::commands::handles(line);
                    delimiter = saved;
                    if(typed_prefix)
                    {
                        parsed.input = os1::shell::Input::None;
                        recovery_marker = typed_here;
                        if(!error)
                            error = "unsupported redirection on typed expression";
                        legacy_here = nullptr;
                    }
                }
            }
            if(error && parsed.input != os1::shell::Input::Here && !recovery_marker)
                recovery_marker = legacy_here;
        }
        if(error)
        {
            if(console_input)
            {
                if(recovery_marker)
                    (void)os1::shell::parse(recovery_marker, parsed);
                if(parsed.input == os1::shell::Input::Here)
                    (void)os1::user::files::here_document(parsed.input_name, reader, true);
            }
            os1::user::files::message(error);
            os1::user::files::message("\n");
            if(!console_input)
                return 1;
            continue;
        }
        os1::user::files::PendingHereDocument here{
            reader,
            parsed.input == os1::shell::Input::Here ? parsed.input_name : nullptr,
            console_input};
        const size_t argc = parsed.argc;
        char** tokens = parsed.argv;
        const char* raw = parsed.raw;
        if(0 == argc)
        {
            continue;
        }
        // These are existing system-control commands under their discovery paths.
        if(os1::user::shell_runtime::prefix(tokens[0], "os.system.") &&
           (strings_equal(tokens[0] + 10, "sync") || strings_equal(tokens[0] + 10, "shutdown") ||
            strings_equal(tokens[0] + 10, "reboot")))
            tokens[0] += 10;

        static os1::user::shell_runtime::Call call;
        if(!call.prepare(parsed, !console_input))
        {
            os1::user::shell_runtime::diagnostic(call.error ? call.error : "invalid arguments");
            if(!console_input)
                return 1;
            continue;
        }
        // Here-document delimiters are syntax, not value expressions.
        if((parsed.input == os1::shell::Input::File &&
            !os1::user::shell_runtime::redirect_value(
                parsed.input_raw, parsed.input_name, !console_input)) ||
           !os1::user::shell_runtime::redirect_value(
               parsed.output_raw, parsed.output_name, !console_input))
        {
            if(!console_input)
                return 1;
            continue;
        }
        const bool log_command = strings_equal(tokens[0], "dmesg") ||
                                 strings_equal(tokens[0], "context") ||
                                 strings_equal(tokens[0], "process.context.list") ||
                                 strings_equal(tokens[0], "os.system.log.print") ||
                                 strings_equal(tokens[0], "history") ||
                                 strings_equal(tokens[0], "process.shell.history.print");
        bool external = call.method == os1::user::shell_runtime::Method::Unknown;
        const char* builtins[] = {"context",
                                  "process.context.list",
                                  "write",
                                  "edit",
                                  "help",
                                  "pid",
                                  "sys",
                                  "ps",
                                  "cpu",
                                  "pci",
                                  "initrd",
                                  "usb",
                                  "devices",
                                  "irqs",
                                  "resources",
                                  "kmem",
                                  "events",
                                  "call",
                                  "sync",
                                  "shutdown",
                                  "reboot",
                                  "system.sync",
                                  "system.shutdown",
                                  "system.reboot",
                                  "exec",
                                  "exit",
                                  "dmesg",
                                  "history",
                                  "os.system.log.print",
                                  "process.shell.history.print"};
        for(const char* builtin : builtins)
            if(strings_equal(tokens[0], builtin))
                external = false;
        os1::user::files::Handle executable;
        char executable_path[OS1_PATH_MAX]{};
        os1_constructor_args constructor{};
        if(external)
        {
            executable.reset(
                resolve_command_path(tokens[0], executable_path, sizeof(executable_path)));
            if(executable.fd < 0)
            {
                run_unknown(tokens[0]);
                if(!console_input)
                    return 1;
                continue;
            }
            bool rich = false;
            os1::parameters::Diagnostic diagnostic{};
            long preflight = os1::user::prepare_constructor(executable.fd,
                                                            argc,
                                                            tokens,
                                                            raw_tail(raw, 1),
                                                            constructor,
                                                            &diagnostic,
                                                            &rich,
                                                            true);
            if(preflight < 0)
            {
                if(diagnostic.code != os1::parameters::Error::None)
                    parameter_diagnostic(diagnostic);
                else
                    os1::user::shell_runtime::diagnostic(
                        "invalid constructor metadata or arguments");
                if(!console_input)
                    return 1;
                continue;
            }
            // Legacy argv is a string adapter, not a second expression grammar.
            static char arguments[os1::shell::argument_limit][os1::shell::line_capacity];
            bool valid = true;
            for(size_t i = 1; !rich && i < argc; ++i)
            {
                static os1::context::Data value;
                os1::context::init(value);
                os1::shell::ValueParser parser{parsed.raw + parsed.begin[i],
                                               parsed.end[i] - parsed.begin[i],
                                               0,
                                               !console_input,
                                               os1::user::shell_runtime::resolve,
                                               os1::user::shell_runtime::warning};
                if(!parser.parse(value, 0, "arg", 3) ||
                   !os1::shell::argv_string(value, 1, arguments[i], sizeof(arguments[i])))
                {
                    os1::user::shell_runtime::diagnostic("invalid executable argument");
                    valid = false;
                    break;
                }
                tokens[i] = arguments[i];
            }

            if(valid && !rich)
            {
                valid =
                    os1::user::prepare_constructor(
                        executable.fd, argc, tokens, raw_tail(raw, 1), constructor, &diagnostic) >=
                    0;
                if(!valid)
                    os1::user::shell_runtime::diagnostic("invalid constructor arguments");
            }
            if(!valid)
            {
                if(!console_input)
                    return 1;
                continue;
            }
        }
        os1::user::files::Handle copy_input;
        int copy_source = -2;
        if(os1::user::shell_runtime::prepare_input(call, copy_input, copy_source) < 0)
        {
            os1::user::shell_runtime::diagnostic(call.method ==
                                                         os1::user::shell_runtime::Method::Copy
                                                     ? "cat/copy failed to open source"
                                                     : "cannot open command input");
            if(!console_input)
                return 1;
            continue;
        }
        CommandSync command_sync((may_write(call.method) && !log_command) || parsed.output_name[0]);
        os1::user::files::Streams streams;
        if(!streams.apply(parsed, here, copy_source, call.recursive))
        {
            os1::user::files::message("shell: redirection failed\n");
            if(!console_input)
                return 1;
            continue;
        }
        // Output redirection can replace a readable source in descriptor 1.
        if(copy_source == 1 && streams.active[1])
            call.streams[os1::user::shell_runtime::source_slot(call.method)] = streams.saved[1].fd;
        command_record.executed = true;
        if(strings_equal(tokens[0], "context") || strings_equal(tokens[0], "process.context.list"))
        {
            const char* path = argc == 2 ? tokens[1] : "";
            if(os1::user::shell_runtime::prefix(path, "path:"))
                path += 5;
            // Reflection paths contain only identifiers and decimal list indexes.
            bool valid = argc <= 2;
            for(const char* p = path; *p; ++p)
                valid = valid && ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                                  *p == '_' || *p == '.');
            if(!valid || os1::user::prompt::inspect(path, output_fd) < 0)
            {
                os1::user::files::message("context: unavailable or invalid path\n");
                command_failed = true;
            }
        }
        else if(strings_equal(tokens[0], "dmesg") ||
                strings_equal(tokens[0], "os.system.log.print") ||
                strings_equal(tokens[0], "history") ||
                strings_equal(tokens[0], "process.shell.history.print"))
            run_log(strings_equal(tokens[0], "history") ||
                        strings_equal(tokens[0], "process.shell.history.print"),
                    argc,
                    tokens);
        else if(call.method != os1::user::shell_runtime::Method::Unknown)
        {
            long result = os1::user::shell_runtime::execute(call);
            if(result < 0)
            {
                DiagnosticOutput error;
                write_string("shell: ");
                write_string(tokens[0]);
                write_string(" failed status=");
                write_signed(result);
                write_char('\n');
                command_failed = true;
            }
            else if(result > 0)
                command_failed = true;  // A search with no matches is not an I/O error.
        }
        else if(strings_equal(tokens[0], "write"))
        {
            if(!console_input)
            {
                os1::user::shell_runtime::diagnostic("write requires interactive input");
                command_failed = true;
            }
            else
                command_failed = !os1::user::files::write_command(argc, tokens);
        }
        else if(strings_equal(tokens[0], "edit"))
        {
            if(!console_input)
            {
                os1::user::shell_runtime::diagnostic("edit requires interactive input");
                command_failed = true;
            }
            else
                command_failed = !os1::user::files::edit(argc, tokens);
        }
        else if(strings_equal(tokens[0], "help"))
        {
            run_help();
        }
        else if(strings_equal(tokens[0], "pid"))
        {
            run_pid();
        }
        else if(strings_equal(tokens[0], "sys"))
        {
            run_sys();
        }
        else if(strings_equal(tokens[0], "ps"))
        {
            run_ps();
        }
        else if(strings_equal(tokens[0], "cpu"))
        {
            run_cpu();
        }
        else if(strings_equal(tokens[0], "pci"))
        {
            run_pci();
        }
        else if(strings_equal(tokens[0], "initrd"))
        {
            run_initrd();
        }
        else if(strings_equal(tokens[0], "usb"))
            run_usb();
        else if(strings_equal(tokens[0], "devices"))
        {
            run_devices();
        }
        else if(strings_equal(tokens[0], "irqs"))
        {
            run_irqs();
        }
        else if(strings_equal(tokens[0], "resources"))
        {
            run_resources();
        }
        else if(strings_equal(tokens[0], "kmem"))
        {
            run_kmem();
        }
        else if(strings_equal(tokens[0], "events"))
        {
            run_events();
        }
        else if(strings_equal(tokens[0], "call"))
        {
            run_call(argc, tokens, raw);
        }
        else if(strings_equal(tokens[0], "exec"))
        {
            if(command_sync.finish())
                run_exec(argc, tokens, raw);
            else
                command_failed = true;
        }
        else if(strings_equal(tokens[0], "sync") || strings_equal(tokens[0], "system.sync") ||
                strings_equal(tokens[0], "shutdown") ||
                strings_equal(tokens[0], "system.shutdown") || strings_equal(tokens[0], "reboot") ||
                strings_equal(tokens[0], "system.reboot"))
        {
            unsigned member =
                strings_equal(tokens[0], "shutdown") || strings_equal(tokens[0], "system.shutdown")
                    ? OS1_SYSTEM_SHUTDOWN
                : strings_equal(tokens[0], "reboot") || strings_equal(tokens[0], "system.reboot")
                    ? OS1_SYSTEM_REBOOT
                    : OS1_SYSTEM_SYNC;
            long status = argc == 1 ? os1::user::system_control(member) : OS1_ERR_INVALID;
            if(status < 0)
            {
                os1::user::files::message("shell: system command failed: ");
                os1::user::files::message(tokens[0]);
                os1::user::files::message(" status=-");
                os1::user::files::number(2, uint64_t(0) - static_cast<uint64_t>(status));
                os1::user::files::message("; machine remains running\n");
                command_failed = true;
            }
        }
        else if(strings_equal(tokens[0], "exit"))
        {
            return command_sync.finish() ? 0 : 1;
        }
        else
        {
            run_external(argc, tokens, raw, executable_path, executable.take(), constructor);
        }
        if(!command_sync.finish())
            command_failed = true;
        if(!console_input && (command_failed || output_failed))
            return 1;
    }
}
