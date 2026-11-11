#include <os1/hardware_tools.hpp>
#include <os1/input.hpp>
#include <os1/log.hpp>
#include <os1/system.hpp>

#include "freestanding/string.hpp"
#include "hardware/archive_stream.hpp"
#include "hardware/capture.hpp"
#include "hardware/test_run.hpp"
#include "log/frame.hpp"

namespace u = os1::user;
namespace fs = u::fs;
namespace f = u::files;
namespace tr = os1::hardware::test_run;
namespace
{
char directory[256]{};
alignas(8) unsigned char scratch[65536]{};
os1::log::Frame identity{};
u::hardware_tools::Printer printer;
int journal = -1, archive = -1;
bool io_failed{};
unsigned passed{}, failed{}, skipped{};

void text(int fd, const char* value)
{
    if(!f::put(fd, value, f::length(value)))
        io_failed = true;
}
void number(int fd, uint64_t value)
{
    if(!f::number(fd, value))
        io_failed = true;
}
void field(int fd, const char* name, uint64_t value)
{
    text(fd, name);
    text(fd, "=");
    number(fd, value);
    text(fd, "\n");
}
bool sync()
{
    return u::system_control(OS1_SYSTEM_SYNC) == 0;
}
void result(const char* name, long status, bool skip = false)
{
    const char* label = skip ? "SKIP " : status < 0 ? "FAIL " : "PASS ";
    if(skip)
        ++skipped;
    else if(status < 0)
        ++failed;
    else
        ++passed;
    const int outputs[] = {1, journal};
    for(int fd : outputs)
    {
        text(fd, label);
        text(fd, name);
        text(fd, " status=");
        if(status < 0)
            text(fd, "-");
        number(fd, status < 0 ? uint64_t(0) - uint64_t(status) : status);
        text(fd, " time_ns=");
        long now = u::ipc_clock();
        number(fd, now < 0 ? 0 : now);
        text(fd, "\n");
    }
    if(!sync())
        io_failed = true;
}
void stage(const char* name)
{
    text(1, "run-tests: ");
    text(1, name);
    text(1, "\n");
    text(journal, "START ");
    text(journal, name);
    text(journal, "\n");
    if(!sync())
        io_failed = true;
}
long mkdir(const char* path, bool exclusive = false)
{
    char leaf[64]{};
    f::Handle parent(f::parent(path, leaf, fs::directory_rights, fs::descendant_rights));
    if(parent.fd < 0)
        return parent.fd;
    long fd = fs::open(
        parent.fd, leaf, fs::directory_rights, fs::descendant_rights, OS1_OBJECT_DIRECTORY);
    if(fd == OS1_ERR_EXISTS && !exclusive)
        fd = fs::open(parent.fd, leaf, fs::directory_rights, fs::descendant_rights);
    if(fd < 0)
        return fd;
    u::close(fd);
    return 0;
}
long file(const char* name)
{
    char path[320]{};
    freestanding::copy_string(path, sizeof(path), directory);
    unsigned at = f::length(path);
    path[at++] = '/';
    freestanding::copy_string(path + at, sizeof(path) - at, name);
    return f::open_file(path, f::Mode::Exclusive);
}
// Stage each bounded snapshot in RAM, then append one standard tar member.
// The progress journal and summary remain independently readable on disk.
struct Artifact : f::Handle
{
    const char* name;
    explicit Artifact(const char* n) : f::Handle(f::temporary()), name(n) {}
    ~Artifact()
    {
        long size = fd < 0 ? fd : u::seek(fd, 0, OS1_SEEK_END);
        char header[512]{};
        if(size < 0 || size > 262144 || !tr::tar_header(name, size, header) || u::seek(fd, 0) < 0 ||
           !f::put(archive, header, sizeof(header)))
        {
            io_failed = true;
            return;
        }
        unsigned left = size;
        while(left)
        {
            long n = u::read(fd, scratch, left < sizeof(scratch) ? left : sizeof(scratch));
            if(n <= 0 || !f::put(archive, reinterpret_cast<char*>(scratch), n))
            {
                io_failed = true;
                return;
            }
            left -= n;
        }
        char padding[512]{};
        unsigned pad = (512 - unsigned(size) % 512) % 512;
        if(pad && !f::put(archive, padding, pad))
            io_failed = true;
    }
};
long export_tmp(int log)
{
    os1_log_export_request request{1, sizeof(request), OS1_LOG_EXPORT_TMP, 0};
    return u::logs::call(log, OS1_LOG_EXPORT, &request, sizeof(request), nullptr, 0);
}
bool identify(int log, tr::Hash& machine)
{
    if(export_tmp(log) < 0)
        return false;
    f::Handle in(u::open("/tmp/log/kernel.bin", OS1_RIGHT_READ));
    if(in.fd < 0 || u::read(in.fd, scratch, sizeof(identity)) != sizeof(identity))
        return false;
    auto& header = *reinterpret_cast<os1::log::Frame*>(scratch);
    if(!os1::log::header_valid(header))
        return false;
    unsigned at = sizeof(identity);
    while(at < header.bytes)
    {
        long n = u::read(in.fd, scratch + at, header.bytes - at);
        if(n <= 0)
            return false;
        at += n;
    }
    if(!os1::log::valid(header, at))
        return false;
    identity = header;
    for(char c : identity.image)
        if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    // Use stable PCI identity fields, never BAR addresses or USB attachments.
    long size = u::observe(OS1_OBSERVE_PCI, scratch, sizeof(scratch));
    if(size < long(sizeof(Os1ObserveHeader)))
        return false;
    auto* h = reinterpret_cast<Os1ObserveHeader*>(scratch);
    if(h->record_size != sizeof(Os1ObservePciRecord) ||
       h->record_count > (size - sizeof(*h)) / sizeof(Os1ObservePciRecord))
        return false;
    auto* records = reinterpret_cast<Os1ObservePciRecord*>(h + 1);
    machine.number(h->record_count);
    for(unsigned i = 0; i < h->record_count; ++i)
    {
        const auto& p = records[i];
        machine.number(p.segment_group);
        machine.number(p.bus);
        machine.number(p.slot);
        machine.number(p.function);
        machine.number(p.vendor_id);
        machine.number(p.device_id);
    }
    const unsigned identity_fields[] = {2, 8, 9, 10, 30};
    for(unsigned field : identity_fields)
    {
        Os1HardwareQuery q{};
        q.version = 2;
        q.operation = OS1_HARDWARE_DESCRIBE;
        q.id = os1::hardware::object_id(os1::hardware::CpuIdentity, 0) + field;
        if(u::observe(OS1_OBSERVE_HARDWARE, &q, sizeof(q)) < 0 || q.count != 1)
            return false;
        machine.number(q.nodes[0].integer);
        machine.text(q.nodes[0].text);
    }
    return true;
}
bool output_directory(uint64_t fingerprint, const char* label)
{
    char machine[64] = "m-", build[24] = "k-", run[40]{};
    unsigned at = 2;
    if(label)
    {
        freestanding::copy_string(machine + at, sizeof(machine) - at, label);
        at += f::length(label);
        machine[at++] = '-';
    }
    tr::hex(fingerprint, machine + at);
    for(unsigned i = 0; i < 16; ++i)
        build[2 + i] = identity.image[i];
    freestanding::copy_string(directory, sizeof(directory), "/disk/tests");
    if(mkdir(directory) < 0)
        return false;
    const char* components[] = {machine, build};
    for(const char* component : components)
    {
        at = f::length(directory);
        directory[at++] = '/';
        freestanding::copy_string(directory + at, sizeof(directory) - at, component);
        if(mkdir(directory) < 0)
            return false;
    }
    at = f::length(directory);
    directory[at++] = '/';
    for(unsigned n = 1; n <= tr::run_limit; ++n)
    {
        tr::run_name(identity.boot, n, run);
        freestanding::copy_string(directory + at, sizeof(directory) - at, run);
        long status = mkdir(directory, true);
        if(!status)
            return true;
        if(status != OS1_ERR_EXISTS)
            return false;
    }
    return false;
}
long snapshot(const char* name, unsigned kind, unsigned ticket = 0)
{
    Artifact out(name);
    if(out.fd < 0)
        return out.fd;
    long n = u::observe(kind, scratch, sizeof(scratch));
    if(n < 0)
        return n;
    if(ticket)
    {
        constexpr auto offset =
            sizeof(Os1ObserveHeader) + offsetof(Os1ObserveEhciStallTrace, ticket);
        unsigned observed = 0;
        if(n != sizeof(Os1ObserveHeader) + sizeof(Os1ObserveEhciStallTrace))
            return OS1_ERR_INVALID;
        __builtin_memcpy(&observed, scratch + offset, sizeof(observed));
        if(observed != ticket)
            return OS1_ERR_CANCELLED;
    }
    return f::put(out.fd, reinterpret_cast<char*>(scratch), n) ? 0 : OS1_ERR_IO;
}
unsigned report_number(const char* text, const char* key)
{
    for(; *text; ++text)
    {
        if(*text != ' ')
            continue;
        const char* value = text + 1;
        const char* k = key;
        while(*k && *value == *k)
        {
            ++value;
            ++k;
        }
        if(*k)
            continue;
        unsigned n = 0;
        while(*value >= '0' && *value <= '9')
        {
            if(n > (UINT32_MAX - unsigned(*value - '0')) / 10)
                return 0;
            n = n * 10 + unsigned(*value++ - '0');
        }
        return n;
    }
    return 0;
}
long ehci_stall_capture()
{
    Artifact demo("gfxdemo-diagnostics.txt");
    if(demo.fd < 0)
        return demo.fd;
    text(1, "Move the USB mouse continuously for 20 to 30 seconds. Press q when done.\n");
    const char* args[] = {"gfxdemo", "--stall-report"};
    const long pid = u::run_program(
        "/disk/bin/gfxdemo", 2, args, false, 0, nullptr, nullptr, nullptr, -1, nullptr, demo.fd);
    if(pid < 0)
        return pid;
    int child_status = 0;
    const long waited = u::waitpid(pid, &child_status);
    if(waited != pid || u::seek(demo.fd, 0) < 0)
        return waited < 0 ? waited : OS1_ERR_IO;
    const long count = u::read(demo.fd, scratch, 1024);
    if(count <= 0 || count == 1024)
        return count < 0 ? count : OS1_ERR_IO;
    scratch[count] = 0;
    const auto* report = reinterpret_cast<const char*>(scratch);
    const unsigned ticket = report_number(report, "trace_ticket=");
    const unsigned events = report_number(report, "events=");
    if(!ticket)
        return OS1_ERR_NOT_FOUND;
    const long saved = snapshot("ehci-stalls.bin", OS1_OBSERVE_EHCI_STALL_TRACE, ticket);
    return saved < 0 ? saved : child_status || !events ? OS1_ERR_IO : 0;
}
long inventory(const char* name, unsigned mode)
{
    struct ArchiveIO
    {
        long position()
        {
            return u::seek(archive, 0, OS1_SEEK_CUR);
        }
        bool seek(long offset)
        {
            return u::seek(archive, offset) == offset;
        }
        bool write(const char* data, unsigned count)
        {
            return f::put(archive, data, count);
        }
    } io;
    tr::ArchiveStream<ArchiveIO> stream(io, name);
    printer = {};
    printer.output_context = &stream;
    printer.output = [](void* context, const char* data, unsigned count) {
        return static_cast<tr::ArchiveStream<ArchiveIO>*>(context)->write(data, count);
    };
    printer.verbose = true;
    printer.tree = true;
    printer.text("platform boot snapshot; USB attachments updated at runtime\n");
    if(mode == 0)
    {
        printer.collection(OS1_HARDWARE_PLATFORM);
        printer.collection(OS1_HARDWARE_TOPOLOGY);
        printer.collection(OS1_HARDWARE_PROCESSORS);
        printer.collection(OS1_HARDWARE_ACPI);
    }
    else if(mode == 1)
        printer.collection(OS1_HARDWARE_PCI);
    else if(mode == 2)
    {
        printer.collection(OS1_HARDWARE_PCI, 0, true);
        printer.collection(OS1_HARDWARE_USB);
    }
    else
        printer.collection(OS1_HARDWARE_BLOCKS);
    bool saved = stream.finish();
    printer.output = nullptr;
    printer.output_context = nullptr;
    if(!saved)
        io_failed = true;
    return saved && !printer.failed ? 0 : OS1_ERR_IO;
}
long copy_log(const char* source, const char* name)
{
    f::Handle in(u::open(source, OS1_RIGHT_READ));
    Artifact out(name);
    if(in.fd < 0 || out.fd < 0)
        return OS1_ERR_IO;
    unsigned total = 0;
    for(;;)
    {
        long n = u::read(in.fd, scratch, sizeof(scratch));
        if(n < 0)
            return n;
        if(!n)
            return 0;
        total += n;
        if(total > os1::log::segment_limit)
            return OS1_ERR_LIMIT;
        if(!f::put(out.fd, reinterpret_cast<char*>(scratch), n))
            return OS1_ERR_IO;
    }
}
long catalog_capture()
{
    Artifact out("catalog.bin");
    os1::hardware::capture::Header header{};
    if(out.fd < 0 || !f::put(out.fd, reinterpret_cast<const char*>(&header), sizeof(header)))
        return OS1_ERR_IO;
    printer = {};
    const unsigned groups[] = {OS1_HARDWARE_PLATFORM,
                               OS1_HARDWARE_PCI,
                               OS1_HARDWARE_USB,
                               OS1_HARDWARE_USB_INTERFACES,
                               OS1_HARDWARE_BLOCKS,
                               OS1_HARDWARE_PARTITIONS,
                               OS1_HARDWARE_FILESYSTEMS,
                               OS1_HARDWARE_MOUNTS};
    for(unsigned group : groups)
    {
        unsigned start = 0;
        for(;;)
        {
            Os1HardwareQuery page{};
            long status = printer.query(page, group, OS1_HARDWARE_CHILDREN, start);
            if(status < 0)
                return status;
            for(unsigned i = 0; i < page.count; ++i)
            {
                const auto& node = page.nodes[i];
                if(node.id < os1::hardware::record_base || (node.id & 63) ||
                   node.type != OS1_HARDWARE_OBJECT)
                    continue;
                if(header.count == os1::hardware::capture::record_limit)
                    return OS1_ERR_LIMIT;
                u::hardware_tools::Printer::Fields fields{};
                if(!printer.fields(node.id, fields))
                    return OS1_ERR_IO;
                os1::hardware::capture::Record record{};
                record.id = node.id;
                record.parent = fields.values[5];
                record.backing = fields.values[37];
                record.source = (node.id - os1::hardware::record_base) >> 16;
                record.known = fields.known;
                for(unsigned v = 0; v < 22; ++v)
                    record.values[v] = fields.values[v + 8];
                freestanding::copy_string(record.name, sizeof(record.name), fields.name);
                freestanding::copy_string(record.state, sizeof(record.state), fields.state);
                freestanding::copy_string(record.driver, sizeof(record.driver), fields.driver);
                if(!f::put(out.fd, reinterpret_cast<const char*>(&record), sizeof(record)))
                    return OS1_ERR_IO;
                ++header.count;
            }
            if(page.end)
                break;
            start = page.next;
        }
    }
    header.generation = printer.generation;
    header.complete = 1;
    return u::seek(out.fd, 0) >= 0 &&
                   f::put(out.fd, reinterpret_cast<const char*>(&header), sizeof(header))
               ? 0
               : OS1_ERR_IO;
}
long roundtrip(int fd)
{
    if(fd < 0)
        return fd;
    for(unsigned i = 0; i < 4096; ++i)
        scratch[i] = (i * 37 + i / 251) & 255;
    if(!f::put(fd, reinterpret_cast<char*>(scratch), 4096) || !sync() || u::seek(fd, 0) < 0)
        return OS1_ERR_IO;
    unsigned at = 0;
    while(at < 4096)
    {
        long n = u::read(fd, scratch + at, 4096 - at);
        if(n <= 0)
            return OS1_ERR_IO;
        at += n;
    }
    for(unsigned i = 0; i < 4096; ++i)
        if(scratch[i] != ((i * 37 + i / 251) & 255))
            return OS1_ERR_IO;
    return u::read(fd, scratch, 1) == 0 ? 0 : OS1_ERR_IO;
}
struct Input
{
    uint64_t id{}, interface{}, events{}, movement{}, button_changes{}, shift_changes{};
    unsigned buttons{}, shift{}, kind{}, catalog_id{};
    tr::InputFailures failures{};
    int subscription{-1};
};
Input inputs[OS1_INPUT_DEVICE_LIMIT]{};
unsigned input_count{}, usb_pointers{};
long input_checks()
{
    Artifact out("input.txt");
    f::Handle root(u::input::bind(u::root(), OS1_INPUT_INTERFACE_ID));
    if(out.fd < 0 || root.fd < 0)
        return OS1_ERR_IO;
    uint64_t after = 0;
    for(unsigned i = 0; i < OS1_INPUT_DEVICE_LIMIT; ++i)
    {
        os1_ipc_result r{};
        os1_input_selector selector{after};
        long status = u::input::call(
            root.fd, OS1_INPUT_INTERFACE_ID, OS1_INPUT_LIST, &selector, sizeof(selector), r);
        if(status < 0)
            return status;
        if(!r.payload_bytes)
            break;
        if(r.payload_bytes != sizeof(os1_input_device))
            return OS1_ERR_INVALID;
        os1_input_device info{};
        __builtin_memcpy(&info, r.payload, sizeof(info));
        if(info.device <= after || !info.connected)
            return OS1_ERR_INVALID;
        after = info.device;
        field(out.fd, "device", info.device);
        text(out.fd, "name=");
        text(out.fd, info.name);
        text(out.fd, "\n");
        field(out.fd, "kind", info.kind);
        field(out.fd, "catalog_id", info.catalog_id);
        usb_pointers += info.kind == OS1_INPUT_POINTER && info.catalog_id != 0;
        selector.device = info.device;
        f::Handle device(u::input::call(
            root.fd, OS1_INPUT_INTERFACE_ID, OS1_INPUT_OPEN, &selector, sizeof(selector), r, true));
        if(device.fd < 0)
            return device.fd;
        uint64_t iface =
            info.kind == OS1_INPUT_POINTER ? OS1_POINTER_INTERFACE_ID : OS1_KEYBOARD_INTERFACE_ID;
        f::Handle client(u::input::bind(device.fd, iface));
        if(client.fd < 0)
            return client.fd;
        status = u::input::call(client.fd, iface, OS1_INPUT_GET_STATE, nullptr, 0, r);
        if(status < 0 || r.payload_bytes != sizeof(os1_input_state))
            return status < 0 ? status : OS1_ERR_INVALID;
        os1_input_state state{};
        __builtin_memcpy(&state, r.payload, sizeof(state));
        field(out.fd, "connected", state.connected);
        if(!state.connected || state.device != info.device)
            return OS1_ERR_CLOSED;
        f::Handle first(u::input::call(client.fd, iface, OS1_INPUT_SUBSCRIBE, nullptr, 0, r, true));
        f::Handle second(
            u::input::call(client.fd, iface, OS1_INPUT_SUBSCRIBE, nullptr, 0, r, true));
        if(first.fd < 0 || second.fd < 0)
            return OS1_ERR_IO;
        f::Handle a(u::input::bind(first.fd, OS1_INPUT_SUBSCRIPTION_INTERFACE_ID));
        f::Handle b(u::input::bind(second.fd, OS1_INPUT_SUBSCRIPTION_INTERFACE_ID));
        if(a.fd < 0 || b.fd < 0)
            return OS1_ERR_IO;
        status = u::input::call(
            a.fd, OS1_INPUT_SUBSCRIPTION_INTERFACE_ID, OS1_INPUT_UNSUBSCRIBE, nullptr, 0, r);
        if(status ||
           u::input::call(
               a.fd, OS1_INPUT_SUBSCRIPTION_INTERFACE_ID, OS1_INPUT_TRY_READ, nullptr, 0, r) !=
               OS1_ERR_CLOSED)
            return OS1_ERR_INVALID;
        status = u::input::call(
            b.fd, OS1_INPUT_SUBSCRIPTION_INTERFACE_ID, OS1_INPUT_TRY_READ, nullptr, 0, r);
        if(status != 0 && status != OS1_ERR_WOULD_BLOCK)
            return status;
        if(!status)
        {
            os1_input_event event{};
            __builtin_memcpy(&event, r.payload, sizeof(event));
            if(!tr::usable_input_event(event, r.payload_bytes, info.device))
                return OS1_ERR_INVALID;
        }
        auto& retained = inputs[input_count++];
        retained.id = info.device;
        retained.interface = iface;
        retained.kind = info.kind;
        retained.catalog_id = info.catalog_id;
        retained.buttons = state.buttons;
        retained.shift = state.keys[28] & 2;
        retained.subscription = b.take();
    }
    return io_failed ? OS1_ERR_IO : 0;
}
void failure_sample(const char* label, const tr::InputFailureSample& sample)
{
    text(journal, label);
    text(journal, "\n");
    field(journal, "observed_ns", sample.observed_ns);
    field(journal, "sequence", sample.sequence);
    field(journal, "event_ns", sample.event_ns);
    field(journal, "event_device", sample.device);
    field(journal, "lost", sample.lost);
    text(journal, "status=");
    if(sample.status < 0)
        text(journal, "-");
    number(journal, sample.status < 0 ? uint64_t(0) - uint64_t(sample.status) : sample.status);
    text(journal, "\n");
    field(journal, "payload_bytes", sample.bytes);
    field(journal, "flags", sample.flags);
    field(journal, "kind", sample.kind);
}
void observe_input(bool stimulus)
{
    if(stimulus)
        text(1, "For 20 seconds: move pointers and press/release buttons and Shift.\n");
    long start = u::ipc_clock();
    if(start < 0)
    {
        result("input clock", start);
        return;
    }
    os1_ipc_pair timer{};
    if(u::ipc(OS1_IPC_CREATE, 0, &timer) < 0)
    {
        result("input timer", OS1_ERR_NO_MEMORY);
        return;
    }
    f::Handle timer_client(timer.client), timer_provider(timer.provider);
    uint64_t duration = stimulus ? UINT64_C(20000000000) : UINT64_C(1000000000);
    long timer_error = 0;
    long now = start;
    const uint64_t end = uint64_t(start) + duration;
    while(uint64_t(now) < end)
    {
        for(unsigned i = 0; i < input_count; ++i)
        {
            auto& d = inputs[i];
            if(d.subscription < 0)
                continue;
            for(unsigned burst = 0; burst < 32; ++burst)
            {
                os1_ipc_result r{};
                long s = u::input::call(d.subscription,
                                        OS1_INPUT_SUBSCRIPTION_INTERFACE_ID,
                                        OS1_INPUT_TRY_READ,
                                        nullptr,
                                        0,
                                        r);
                if(s == OS1_ERR_WOULD_BLOCK)
                    break;
                os1_input_event e{};
                if(!s && r.payload_bytes == sizeof(e))
                    __builtin_memcpy(&e, r.payload, sizeof(e));
                bool usable = d.failures.accept(s, r.payload_bytes, e, d.id, d.kind, now);
                if(s != 0 || r.payload_bytes != sizeof(e))
                {
                    u::close(d.subscription);
                    d.subscription = -1;
                    break;
                }
                if(usable)
                {
                    ++d.events;
                    d.movement += e.dx != 0 || e.dy != 0;
                    d.button_changes += d.buttons != e.buttons;
                    d.shift_changes += d.shift != unsigned(e.keys[28] & 2);
                    d.buttons = e.buttons;
                    d.shift = e.keys[28] & 2;
                }
            }
        }
        now = u::ipc_clock();
        if(now < 0)
        {
            timer_error = now;
            break;
        }
        if(uint64_t(now) >= end)
            break;
        uint64_t deadline = uint64_t(now) + 20000000;
        if(deadline > end)
            deadline = end;
        long waited = u::ipc_wait_until(timer.client, deadline);
        if(waited != OS1_ERR_TIMED_OUT)
        {
            timer_error = waited < 0 ? waited : OS1_ERR_INVALID;
            break;
        }
        now = u::ipc_clock();
        if(now < 0)
        {
            timer_error = now;
            break;
        }
    }
    // Close every subscription before report writes or filesystem synchronization.
    for(unsigned i = 0; i < input_count; ++i)
    {
        if(inputs[i].subscription >= 0)
            u::close(inputs[i].subscription);
        inputs[i].subscription = -1;
    }
    if(timer_error)
        result("input timer wait", timer_error);
    for(unsigned i = 0; i < input_count; ++i)
    {
        field(journal, "observed_device", inputs[i].id);
        field(journal, "catalog_id", inputs[i].catalog_id);
        auto& failures = inputs[i].failures;
        field(journal, "failure_occurrences", failures.total);
        field(journal, "lost_events", failures.lost_events);
        field(journal, "lost_global_records", failures.lost_records);
        field(journal, "removed_events", failures.removed);
        field(journal, "invalid_replies", failures.invalid);
        field(journal, "closed_replies", failures.closed);
        field(journal, "error_replies", failures.errors);
        if(failures.total)
        {
            failure_sample("first_failure", failures.first);
            failure_sample("last_failure", failures.last);
        }
        if(failures.lost_events)
            result("input event loss", OS1_ERR_IO);
        if(failures.removed)
            result("input device removed", OS1_ERR_CLOSED);
        if(failures.invalid)
            result("input invalid reply", OS1_ERR_INVALID);
        if(failures.closed)
            result("input stream closed", OS1_ERR_CLOSED);
        if(failures.errors)
            result("input call error", OS1_ERR_IO);
        field(journal, "observed_events", inputs[i].events);
        field(journal, "movement_events", inputs[i].movement);
        field(journal, "button_changes", inputs[i].button_changes);
        field(journal, "left_shift_changes", inputs[i].shift_changes);
        result("physical stimulus observed", 0, !stimulus || !inputs[i].events);
    }
}
}  // namespace

int main(int argc, char** argv)
{
    const char* label = nullptr;
    bool stimulus = true, expect_mouse = true, ehci_stalls = false;
    for(int i = 1; i < argc; ++i)
    {
        if(u::hardware_tools::equal(argv[i], "--input"))
            stimulus = true;
        else if(u::hardware_tools::equal(argv[i], "--ehci-stalls"))
            ehci_stalls = true;
        else if(u::hardware_tools::equal(argv[i], "--no-input"))
            stimulus = false;
        else if(u::hardware_tools::equal(argv[i], "--no-expect-usb-mouse"))
            expect_mouse = false;
        else if(u::hardware_tools::equal(argv[i], "--expect-usb-mouse"))
            expect_mouse = true;
        else if(u::hardware_tools::equal(argv[i], "--machine") && i + 1 < argc &&
                tr::label_valid(argv[i + 1]))
            label = argv[++i];
        else
        {
            f::message(
                "usage: run-tests [--machine NAME] [--no-input] [--no-expect-usb-mouse] "
                "[--ehci-stalls]\n");
            return 2;
        }
    }
    f::Handle log(u::logs::bind(true));
    tr::Hash fingerprint;
    if(log.fd < 0 || !identify(log.fd, fingerprint))
    {
        f::message("run-tests: cannot establish kernel and machine identity; no results created\n");
        return 1;
    }
    if(!output_directory(fingerprint.value, label))
    {
        f::message(
            "run-tests: cannot create a new /disk/tests run; check storage and free space\n");
        return 1;
    }
    text(1, "run-tests: results ");
    text(1, directory);
    text(1, "\n");
    f::Handle manifest(file("manifest.txt")), progress(file("progress.txt"));
    f::Handle captures(file("captures.tar"));
    journal = progress.fd;
    archive = captures.fd;
    if(manifest.fd < 0 || journal < 0 || archive < 0)
    {
        f::message("run-tests: cannot create result files; capture incomplete\n");
        return 1;
    }
    field(manifest.fd, "format_version", 1);
    field(manifest.fd, "runner_version", 3);
    field(manifest.fd, "test_suite_version", 3);
    field(manifest.fd, "machine_fingerprint", fingerprint.value);
    text(manifest.fd, "machine_label=");
    text(manifest.fd, label ? label : "auto");
    text(manifest.fd, "\nkernel_revision=");
    size_t revision_length = 0;
    while(revision_length < sizeof(identity.revision) && identity.revision[revision_length])
    {
        ++revision_length;
    }
    if(!f::put(manifest.fd, identity.revision, revision_length))
        io_failed = true;
    text(manifest.fd, "\nkernel_build=");
    if(!f::put(manifest.fd, identity.image, sizeof(identity.image)))
        io_failed = true;
    text(manifest.fd, "\n");
    field(manifest.fd, "boot_id", identity.boot);
    field(manifest.fd, "boot_mode", identity.boot_mode);
    field(manifest.fd, "diagnostic", identity.diagnostic);
    field(manifest.fd, "wall_ns", identity.wall_ns);
    field(manifest.fd, "start_ns", u::ipc_clock());
    field(manifest.fd, "pid", u::getpid());
    field(manifest.fd, "input_observation", stimulus);
    field(manifest.fd, "expect_usb_mouse", expect_mouse);
    if(ehci_stalls)
        field(manifest.fd, "ehci_stalls", 1);
    {
        f::Handle binary(u::open("/disk/bin/run-tests", OS1_RIGHT_READ));
        tr::Hash deployed;
        long n = binary.fd < 0 ? binary.fd : 0;
        while(binary.fd >= 0 && (n = u::read(binary.fd, scratch, sizeof(scratch))) > 0)
            for(long i = 0; i < n; ++i)
                deployed.byte(scratch[i]);
        if(n < 0)
            io_failed = true;
        else
            field(manifest.fd, "deployed_runner_fnv64", deployed.value);
    }
    result("initial kernel log copy", copy_log("/tmp/log/kernel.bin", "kernel-before.bin"));
    stage("hardware snapshots");
    result("structured catalog", catalog_capture());
    result("system snapshot", snapshot("system.bin", OS1_OBSERVE_SYSTEM));
    result("PCI snapshot", snapshot("pci.bin", OS1_OBSERVE_PCI));
    result("USB boot snapshot", snapshot("usb-boot.bin", OS1_OBSERVE_USB));
    result("lshw", inventory("lshw.txt", 0));
    result("lspci", inventory("lspci.txt", 1));
    result("lsusb", inventory("lsusb-before.txt", 2));
    result("lsblk", inventory("lsblk.txt", 3));
    stage("file roundtrips");
    {
        f::Handle tmp(f::temporary()), disk(file("storage-probe.bin"));
        result("tmp write/read", roundtrip(tmp.fd));
        result("disk write/sync/read", roundtrip(disk.fd));
    }
    stage("native input");
    result("USB input failures before",
           snapshot("usb-input-before.bin", OS1_OBSERVE_USB_INPUT_FAILURES));
    long input_status = input_checks();
    result("input acquisition/state/subscriptions", input_status, !input_status && !input_count);
    field(journal, "input_devices", input_count);
    field(journal, "usb_pointers", usb_pointers);
    result("expected USB mouse", usb_pointers ? 0 : OS1_ERR_NOT_FOUND, !expect_mouse);
    observe_input(stimulus);
    result("USB input failures after",
           snapshot("usb-input-after.bin", OS1_OBSERVE_USB_INPUT_FAILURES));
    result("USB after input", inventory("lsusb-after.txt", 2));
    if(ehci_stalls)
    {
        stage("EHCI pointer stall capture");
        result("EHCI trace and graphics diagnostics", ehci_stall_capture());
    }
    stage("log capture");
    long exported = export_tmp(log.fd);
    result("log export", exported);
    if(!exported)
        result("kernel log copy", copy_log("/tmp/log/kernel.bin", "kernel.bin"));
    char terminator[1024]{};
    if(!f::put(archive, terminator, sizeof(terminator)))
        io_failed = true;
    f::Handle summary(file("summary.txt"));
    if(summary.fd < 0)
    {
        f::message("run-tests: cannot create summary; capture incomplete\n");
        return 1;
    }
    field(summary.fd, "passed", passed);
    field(summary.fd, "failed", failed);
    field(summary.fd, "skipped", skipped);
    field(summary.fd, "io_failed", io_failed);
    field(summary.fd, "end_ns", u::ipc_clock());
    text(summary.fd, "state=complete\n");
    if(!sync() || io_failed)
    {
        f::message("run-tests: result persistence failed; capture may be incomplete\n");
        return 1;
    }
    text(1, failed ? "run-tests: FAIL; saved results\n" : "run-tests: PASS; saved results\n");
    return failed ? 1 : 0;
}
