#include <os1/ipc.hpp>
#include <os1/vfs.hpp>

#include "interface/textprocessor.hpp"
#include "kernel_console_check.hpp"
using namespace os1::user;
using namespace os1::textprocessor;
#define CHECK(c, m)                                              \
    do                                                           \
    {                                                            \
        if(!(c))                                                 \
        {                                                        \
            constexpr char s[] = "[user/ipccheck] fail " m "\n"; \
            write(2, s, sizeof(s) - 1);                          \
            return 1;                                            \
        }                                                        \
    } while(0)
void say(const char* s)
{
    unsigned n = 0;
    while(s[n])
        ++n;
    write(1, s, n);
}
os1_ipc_message message(uint32_t member)
{
    os1_ipc_message r{};
    r.version = 1;
    r.size = sizeof(r);
    r.interface_id = interface_id;
    r.interface_version = interface_version;
    r.member = member;
    return r;
}
template<class T>
void payload(os1_ipc_message& m, const T& value)
{
    auto* s = reinterpret_cast<const uint8_t*>(&value);
    m.payload_bytes = sizeof(value);
    for(unsigned i = 0; i < sizeof(value); ++i)
        m.payload[i] = s[i];
}
template<class T>
T value(const os1_ipc_result& r)
{
    T out{};
    auto* d = reinterpret_cast<uint8_t*>(&out);
    for(unsigned i = 0; i < sizeof(out); ++i)
        d[i] = r.payload[i];
    return out;
}
bool ok(const os1_ipc_result& r, unsigned size)
{
    return !r.transport_status && !r.application_status && r.payload_bytes == size;
}
int main(int argc, char**)
{
    if(argc == 2)
        return os1::kernel_console_check::child();
    CHECK(os1::kernel_console_check::run(), "kernel console protocol");
    say("[user/ipccheck] kernel console two clients ok\n");
    os1_ipc_pair pair{};
    CHECK(ipc(OS1_IPC_CREATE, 0, &pair) == 0, "pair");
    long image = open("/bin/textprocessor", OS1_RIGHT_EXECUTE);
    CHECK(image >= 0, "image");
    os1_launch_request launch_request{};
    launch_request.version = OS1_STARTUP_VERSION;
    launch_request.size = sizeof(launch_request);
    launch_request.startup.version = OS1_STARTUP_VERSION;
    launch_request.startup.size = sizeof(launch_request.startup);
    launch_request.startup.root_fd = launch_request.startup.cwd_fd = -1;
    launch_request.grant_count = 1;
    launch_request.grants[0] = {pair.provider, 3, OS1_RIGHT_READ | OS1_RIGHT_WRITE, 0, 0, 0};
    long pid = launch(image, OS1_LAUNCH_PRIVATE_PROVIDER, launch_request);
    close(image);
    close(pair.provider);
    CHECK(pid > 0, "private launch");
    CHECK(ipc_wait(pair.client) == 0, "ready wait");
    os1_ipc_info info{};
    CHECK(ipc(OS1_IPC_INFO, pair.client, &info) == 0 && info.ready &&
              info.interface_id == interface_id && info.interface_version == 1,
          "ready identity");
    long events = ipc_subscribe(pair.client, Progress);
    CHECK(events >= 0, "progress subscription");
    auto request = message(Lowercase);
    Text input{5, {'H', 'e', 'L', 'L', 'O'}};
    payload(request, input);
    os1_ipc_result result{};
    CHECK(ipc_call(pair.client, request, result) == 0 && ok(result, sizeof(Text)), "sync");
    os1_ipc_event event{};
    CHECK(ipc(OS1_IPC_EVENT_READ, events, &event) == 0 && event.member == Progress &&
              event.payload_bytes == sizeof(ProgressValue),
          "progress event");
    CHECK(ipc_unsubscribe(events) == 0, "progress unsubscribe");
    close(events);
    auto lower = value<Text>(result);
    CHECK(lower.length == 5 && lower.bytes[0] == 'h' && lower.bytes[2] == 'l', "lowercase");
    say("lowercase: ");
    write(1, lower.bytes, lower.length);
    say("\n");
    request = message(Uppercase);
    payload(request, input);
    long operation = ipc(OS1_IPC_SUBMIT, pair.client, &request);
    CHECK(operation >= 0, "async submit");
    request.payload[4] = 'x';  // Admission copied the original bytes.
    CHECK(ipc_result(operation, result) == 0 && ok(result, sizeof(Text)), "async result");
    auto upper = value<Text>(result);
    CHECK(upper.length == 5 && upper.bytes[0] == 'H' && upper.bytes[1] == 'E', "copied uppercase");
    say("uppercase: ");
    write(1, upper.bytes, upper.length);
    say("\n");
    CHECK(ipc(OS1_IPC_RESULT, operation, &result) == 0 && ok(result, sizeof(Text)),
          "retained result");
    close(operation);
    request = message(GetEnabled);
    CHECK(ipc_call(pair.client, request, result) == 0 && ok(result, sizeof(Settings)),
          "get enabled");
    auto settings = value<Settings>(result);
    CHECK(settings.enabled == 1 && settings.revision == 1, "initial enabled");
    settings.enabled = 0;
    request = message(SetEnabled);
    payload(request, settings);
    CHECK(ipc_call(pair.client, request, result) == 0 && ok(result, sizeof(Settings)),
          "set enabled");
    CHECK(ipc_call(pair.client, request, result) == 0 &&
              result.application_status == revision_conflict,
          "revision conflict");
    request = message(Lowercase);
    payload(request, input);
    CHECK(ipc_call(pair.client, request, result) == 0 && ok(result, sizeof(Text)) &&
              value<Text>(result).bytes[0] == 'H',
          "disabled conversion");
    request = message(GetProcessed);
    CHECK(ipc_call(pair.client, request, result) == 0 && ok(result, 8) &&
              value<uint64_t>(result) == 3,
          "processed");
    request = message(Progress);
    CHECK(ipc_call(pair.client, request, result) == 0 &&
              result.application_status == OS1_ERR_UNSUPPORTED,
          "event is not a method");
    say("enabled: false; processed: 3; progress: volatile event\n");
    close(pair.client);
    int status = -1;
    CHECK(waitpid(pid, &status) == pid && status == 0, "provider exit");
    say("[user/ipccheck] ok\n");
    return 0;
}
