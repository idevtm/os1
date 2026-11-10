#include <os1/observe.h>

#include <os1/basic_files.hpp>
#include <os1/file_tools.hpp>
#include <os1/log.hpp>
#include <os1/system.hpp>

namespace
{
using namespace os1::user;
void text(const char* value)
{
    files::put(1, value, files::length(value));
}
bool equal(const char* a, const char* b)
{
    while(*a && *a == *b)
    {
        ++a;
        ++b;
    }
    return *a == *b;
}
void hex(uint64_t value)
{
    char bytes[16];
    unsigned n = 0;
    do
    {
        bytes[n++] = "0123456789abcdef"[value & 15];
        value >>= 4;
    } while(value);
    while(n)
        files::put(1, &bytes[--n], 1);
}
long log_status()
{
    files::Handle client(logs::bind());
    if(client.fd < 0)
        return client.fd;
    os1_log_export_info info{};
    long status = logs::call(client.fd, OS1_LOG_EXPORT_INFO, nullptr, 0, &info, sizeof(info));
    if(status != sizeof(info))
        return status < 0 ? status : OS1_ERR_INVALID;
    text("suspended:");
    files::number(1, info.suspended);
    text(" failures:");
    files::number(1, info.failures);
    text("\n");
    return 0;
}
long log_export()
{
    files::Handle client(logs::bind(true));
    if(client.fd < 0)
        return client.fd;
    os1_log_export_request request{1, sizeof(request), OS1_LOG_EXPORT_TMP, 0};
    return logs::call(client.fd, OS1_LOG_EXPORT, &request, sizeof(request), nullptr, 0);
}
void usb()
{
    alignas(16) static uint8_t buffer[64 * 1024];
    long size = observe(OS1_OBSERVE_USB, buffer, sizeof(buffer));
    if(size < long(sizeof(Os1ObserveHeader)))
    {
        text("rescue: USB snapshot unavailable\n");
        return;
    }
    const auto& header = *reinterpret_cast<const Os1ObserveHeader*>(buffer);
    if(header.abi_version != OS1_OBSERVE_ABI_VERSION || header.kind != OS1_OBSERVE_USB ||
       header.record_size != sizeof(Os1ObserveUsbRecord) ||
       header.record_count > (sizeof(buffer) - sizeof(header)) / sizeof(Os1ObserveUsbRecord) ||
       size != long(sizeof(header) + header.record_count * sizeof(Os1ObserveUsbRecord)))
    {
        text("rescue: invalid USB snapshot\n");
        return;
    }
    text("USB boot snapshot: controller topology and USB mounts\n");
    const auto* records = reinterpret_cast<const Os1ObserveUsbRecord*>(buffer + sizeof(header));
    for(unsigned i = 0; i < header.record_count; ++i)
    {
        const auto& r = records[i];
        if(r.kind == OS1_OBSERVE_USB_ROOT_PORT && equal(r.stage, "disconnected"))
            continue;
        text(r.kind == OS1_OBSERVE_USB_CONTROLLER  ? "controller"
             : r.kind == OS1_OBSERVE_USB_ROOT_PORT ? "root"
             : r.kind == OS1_OBSERVE_USB_DEVICE    ? "device"
                                                   : "mount");
        if(r.pci_index != OS1_OBSERVE_INDEX_NONE)
        {
            text(" pci=");
            files::number(1, r.pci_index);
        }
        if(r.root_port)
        {
            text(" root=");
            files::number(1, r.root_port);
        }
        if(r.port)
        {
            text(" port=");
            files::number(1, r.port);
        }
        text(" ");
        text(r.stage);
        text(" status=0x");
        hex(r.status);
        text(" detail=0x");
        hex(r.detail);
        text("\n");
    }
}
}  // namespace

int main(int, char**)
{
    text(
        "rescue: storage-independent console\n"
        "help; ls [path]; cat path; usb; retry; sync; shutdown; reboot; log_status; log_export\n");
    os1::user::files::Reader reader;
    for(;;)
    {
        text("rescue> ");
        os1::user::files::Line line;
        long status = reader.line(line);
        if(status < 0)
        {
            text("rescue: input failed or line too long\n");
            continue;
        }
        if(line.eof)
            return 0;
        if(line.size && line.bytes[line.size - 1] == '\n')
            line.bytes[--line.size] = 0;
        char* command = line.bytes;
        while(*command == ' ')
            ++command;
        char* argument = command;
        while(*argument && *argument != ' ')
            ++argument;
        if(*argument)
            *argument++ = 0;
        while(*argument == ' ')
            ++argument;
        if(!*command)
            continue;
        char* argv[] = {command, argument};
        if(equal(command, "ls"))
            status = os1::user::basic_files::list(*argument ? 2 : 1, argv);
        else if(equal(command, "cat") && *argument)
            status = os1::user::basic_files::cat(2, argv);
        else if(*argument)
        {
            text("rescue: unexpected argument\n");
            continue;
        }
        else if(equal(command, "retry"))
            return 0;
        else if(equal(command, "usb"))
            usb();
        else if(equal(command, "log_status"))
            status = log_status();
        else if(equal(command, "log_export"))
            status = log_export();
        else if(equal(command, "help"))
            text(
                "help; ls [path]; cat path; usb; retry; sync; shutdown; reboot; log_status; "
                "log_export\n");
        else if(equal(command, "sync") || equal(command, "shutdown") || equal(command, "reboot"))
            status = os1::user::system_control(equal(command, "sync")       ? OS1_SYSTEM_SYNC
                                               : equal(command, "shutdown") ? OS1_SYSTEM_SHUTDOWN
                                                                            : OS1_SYSTEM_REBOOT);
        else
        {
            text("rescue: unknown command\n");
            continue;
        }
        if(status)
        {
            text("rescue: command failed: status=");
            if(status < 0)
                text("-");
            files::number(1, status < 0 ? uint64_t(-(status + 1)) + 1 : uint64_t(status));
            text("\n");
        }
    }
}
