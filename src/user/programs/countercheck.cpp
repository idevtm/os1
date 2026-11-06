#include <os1/ipc.hpp>
#include <os1/startup.hpp>

#include "interface/counter.hpp"
using namespace os1::user;
using namespace os1::counter;
#define CHECK(c, m)                                                  \
    do                                                               \
    {                                                                \
        if(!(c))                                                     \
        {                                                            \
            constexpr char s[] = "[user/countercheck] fail " m "\n"; \
            write(2, s, sizeof(s) - 1);                              \
            return 1;                                                \
        }                                                            \
    } while(0)
uint64_t deadline(uint64_t delta = 2000000000ull)
{
    return static_cast<uint64_t>(ipc_clock()) + delta;
}
void launch_base(os1_launch_request& r)
{
    r = {};
    r.version = OS1_STARTUP_VERSION;
    r.size = sizeof(r);
    r.startup.version = OS1_STARTUP_VERSION;
    r.startup.size = sizeof(r.startup);
    r.startup.root_fd = r.startup.cwd_fd = -1;
}
long start(const char* path, int endpoint, const char* mode = nullptr)
{
    long image =
        open(path, OS1_RIGHT_EXECUTE | OS1_RIGHT_READ | OS1_RIGHT_SEEK | OS1_RIGHT_INSPECT);
    if(image < 0)
        return image;
    os1_launch_request r{};
    launch_base(r);
    r.grant_count = 1;
    r.grants[0] = {endpoint, 3, OS1_RIGHT_READ | OS1_RIGHT_WRITE, 0, 0, 0};
    if(mode)
    {
        r.startup.argc = 1;
        unsigned n = 0;
        do
        {
            r.startup.strings[n] = mode[n];
        } while(mode[n++]);
        r.startup.string_bytes = n;
    }
    else
    {
        os1::constructor::Metadata schema{};
        const char* options[] = {"--initial", "10", "--ceiling", "100"};
        if(os1::user::constructor_metadata(image, schema) != 1 ||
           !os1::constructor::parse(schema, 4, options, r.startup.constructor))
        {
            close(image);
            return OS1_ERR_INVALID;
        }
    }
    constexpr char env[] = "LABEL=private";
    r.startup.envc = 1;
    r.startup.envp[0] = r.startup.string_bytes;
    for(unsigned i = 0; i < sizeof(env); ++i)
        r.startup.strings[r.startup.string_bytes++] = env[i];
    long pid = launch(image, OS1_LAUNCH_PRIVATE_PROVIDER, r);
    close(image);
    return pid;
}
os1_ipc_message message(unsigned member, uint64_t amount = 0)
{
    os1_ipc_message m{};
    m.version = 1;
    m.size = sizeof(m);
    m.interface_id = interface_id;
    m.interface_version = 1;
    m.member = member;
    if(member == Add)
    {
        m.payload_bytes = 8;
        os1::constructor::encode(m.payload, 8, amount);
    }
    return m;
}
long invoke(int fd, unsigned member, os1_ipc_result& result, uint64_t amount = 0)
{
    auto m = message(member, amount);
    long op = ipc(OS1_IPC_SUBMIT, fd, &m);
    if(op < 0)
        return op;
    long status = ipc_result_until(op, result, deadline());
    close(op);
    return status;
}
bool success(const os1_ipc_result& result, uint64_t expected, unsigned bytes = 16)
{
    return !result.transport_status && !result.application_status &&
           result.payload_bytes == bytes && os1::constructor::decode(result.payload, 8) == expected;
}
int main(int argc, char** argv)
{
    if(argc == 1 && (os1::constructor::equal(argv[0], "hang_before") ||
                     os1::constructor::equal(argv[0], "hang_after")))
    {
        StartupContext ctx{*startup_record};
        if(!ctx.environment("LABEL") ||
           !os1::constructor::equal(ctx.environment("LABEL"), "private") ||
           read(0, nullptr, 0) != OS1_ERR_ACCESS)
            return 99;
        if(os1::constructor::equal(argv[0], "hang_after"))
        {
            os1_ipc_info info{1, sizeof(info), 0, interface_id, 1, 1};
            if(ipc(OS1_IPC_READY, 3, &info) < 0)
                return 98;
            os1_ipc_message m{};
            if(ipc_receive(3, m) < 0)
                return 97;
        }
        for(;;)
            asm volatile("pause" ::: "memory");
    }
    if(argc == 1 && os1::constructor::equal(argv[0], "listener"))
    {
        CHECK(ipc_wait_until(3, deadline()) == 0, "listener wait");
        os1_ipc_event event{};
        CHECK(ipc(OS1_IPC_EVENT_READ, 3, &event) == 0 && event.member == Changed &&
                  event.payload_bytes == 16 && os1::constructor::decode(event.payload, 8) == 15,
              "listener broadcast");
        CHECK(ipc_unsubscribe(3) == OS1_ERR_ACCESS, "listener attenuation");
        constexpr char s[] = "[user/countercheck] second client ok\n";
        write(1, s, sizeof(s) - 1);
        return 0;
    }
    os1_ipc_pair pair{};
    CHECK(ipc(OS1_IPC_CREATE, 0, &pair) == 0, "pair");
    long pid = start("/disk/bin/counter", pair.provider);
    close(pair.provider);
    CHECK(pid > 0, "launch typed provider");
    CHECK(ipc_wait_until(pair.client, deadline()) == 0, "ready");
    os1_ipc_result result{};
    CHECK(invoke(pair.client, Read, result) == 0 && success(result, 10), "typed initial");
    CHECK(invoke(pair.client, Add, result, 1) == 0 && success(result, 11),
          "pre-subscription change");
    long first = ipc_subscribe(pair.client, Changed), second = ipc_subscribe(pair.client, Changed);
    CHECK(first >= 0 && second >= 0, "subscriptions");
    os1_ipc_event event{};
    CHECK(ipc(OS1_IPC_EVENT_READ, first, &event) == OS1_ERR_WOULD_BLOCK, "no history");
    long listener_image = open("/disk/bin/countercheck", OS1_RIGHT_EXECUTE);
    CHECK(listener_image >= 0, "listener image");
    os1_launch_request listener{};
    launch_base(listener);
    listener.grant_count = 3;
    listener.grants[0] = {static_cast<int32_t>(second), 3, OS1_RIGHT_READ, 0, 0, 0};
    listener.grants[1] = {1, 1, OS1_RIGHT_WRITE, 0, 0, 0};
    listener.grants[2] = {2, 2, OS1_RIGHT_WRITE, 0, 0, 0};
    listener.startup.argc = 1;
    constexpr char name[] = "listener";
    listener.startup.string_bytes = sizeof(name);
    for(unsigned i = 0; i < sizeof(name); ++i)
        listener.startup.strings[i] = name[i];
    long listener_pid = launch(listener_image, OS1_LAUNCH_SPAWN, listener);
    close(listener_image);
    CHECK(listener_pid > 0, "second client launch");
    CHECK(ipc_stop_child(listener_pid) == OS1_ERR_ACCESS, "stop ordinary child denied");
    auto add = message(Add, 4);
    long operation = ipc(OS1_IPC_SUBMIT, pair.client, &add);
    CHECK(operation >= 0, "async add");
    CHECK(ipc_result_until(operation, result, deadline()) == 0 && success(result, 15),
          "async result");
    close(operation);
    CHECK(ipc(OS1_IPC_EVENT_READ, first, &event) == 0 && event.dropped == 0 &&
              os1::constructor::decode(event.payload, 8) == 15,
          "first broadcast");
    int status = -1;
    CHECK(waitpid(listener_pid, &status) == listener_pid && status == 0, "listener exit");
    close(second);
    long fast = ipc_subscribe(pair.client, Changed);
    CHECK(fast >= 0, "fast subscription");
    for(unsigned i = 0; i < 10; ++i)
    {
        CHECK(invoke(pair.client, Add, result, 1) == 0 && success(result, 16 + i), "increment");
        CHECK(ipc(OS1_IPC_EVENT_READ, fast, &event) == 0 && event.dropped == 0,
              "fast subscriber unaffected");
    }
    CHECK(ipc(OS1_IPC_EVENT_READ, first, &event) == 0 && event.dropped == 2, "slow overflow");
    CHECK(ipc_unsubscribe(first) == 0 && ipc(OS1_IPC_EVENT_READ, first, &event) == OS1_ERR_CLOSED,
          "unsubscribe discards");
    close(first);
    CHECK(ipc_unsubscribe(fast) == 0, "unsubscribe fast");
    close(fast);
    CHECK(invoke(pair.client, Add, result, 100) == 0 && result.application_status == OS1_ERR_LIMIT,
          "ceiling");
    CHECK(invoke(pair.client, Reset, result) == 0 && success(result, 0), "reset");
    CHECK(invoke(pair.client, GetValue, result) == 0 && success(result, 0, 8), "property");
    CHECK(ipc_stop_child(pid) == 0, "stop blocked provider");
    CHECK(waitpid(pid, &status) == pid && status == OS1_PROVIDER_STOP_STATUS, "stopped status");
    close(pair.client);
    for(unsigned delivered = 0; delivered < 2; ++delivered)
    {
        CHECK(ipc(OS1_IPC_CREATE, 0, &pair) == 0, "hang pair");
        pid = start(
            "/disk/bin/countercheck", pair.provider, delivered ? "hang_after" : "hang_before");
        close(pair.provider);
        CHECK(pid > 0, "hang launch");
        long pending = -1;
        if(delivered)
        {
            CHECK(ipc_wait_until(pair.client, deadline()) == 0, "hang ready");
            auto m = message(Add, 1);
            pending = ipc(OS1_IPC_SUBMIT, pair.client, &m);
            CHECK(pending >= 0, "hang submit");
            CHECK(ipc_result_until(pending, result, deadline(50000000)) == OS1_ERR_TIMED_OUT,
                  "method deadline");
        }
        else
            CHECK(ipc_wait_until(pair.client, deadline(50000000)) == OS1_ERR_TIMED_OUT,
                  "startup deadline");
        CHECK(ipc_stop_child(pid) == 0, "stop hung provider");
        CHECK(waitpid(pid, &status) == pid && status == OS1_PROVIDER_STOP_STATUS,
              "hung provider reaped");
        if(delivered)
        {
            CHECK(ipc(OS1_IPC_RESULT, pending, &result) == 0 &&
                      result.transport_status == OS1_ERR_OUTCOME_UNKNOWN,
                  "unknown outcome retained");
            close(pending);
        }
        close(pair.client);
    }
    constexpr char done[] = "[user/countercheck] ok\n";
    write(1, done, sizeof(done) - 1);
    return 0;
}
