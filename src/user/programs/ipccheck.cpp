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
// The child of the byte stream check. It has a reader end at descriptor 3 and a writer end at
// descriptor 4. Each step waits in the kernel for the parent.
constexpr unsigned stream_flood = 20000;
char flood_byte(unsigned index)
{
    return char(index * 5);
}
int stream_child()
{
    char byte = 0;
    // Wait in a read for the first byte of the parent.
    if(read(3, &byte, 1) != 1 || byte != 'g')
        return 1;
    // Write more bytes than the stream holds. The write waits for the parent to read.
    static char flood[stream_flood];
    for(unsigned i = 0; i < stream_flood; ++i)
        flood[i] = flood_byte(i);
    for(unsigned sent = 0; sent < stream_flood;)
    {
        const long count = write(4, flood + sent, stream_flood - sent);
        if(count <= 0)
            return 2;
        sent += unsigned(count);
    }
    // The parent closes its writer end. The read then gives the end of the stream.
    return read(3, &byte, 1) == 0 ? 0 : 3;
}

int main(int argc, char**)
{
    if(argc == 3)
        return stream_child();
    if(argc == 2)
        return os1::kernel_console_check::child();
    CHECK(os1::kernel_console_check::run(), "kernel console protocol");
    say("[user/ipccheck] kernel console two clients ok\n");
    // A byte stream moves bytes from its writer end to its reader end.
    os1_stream_pair stream{};
    CHECK(stream_create(stream) == 0, "stream");
    os1_resource_info stream_info{};
    CHECK(query(stream.reader, &stream_info) == 0 && stream_info.kind == OS1_OBJECT_STREAM,
          "stream kind");
    const int stream_reader[] = {stream.reader};
    CHECK(ipc_wait_many(stream_reader, 1, 0) == OS1_ERR_TIMED_OUT, "empty stream is not ready");
    CHECK(write(stream.writer, "abc", 3) == 3 && write(stream.reader, "x", 1) < 0 &&
              ipc_wait_many(stream_reader, 1, 0) == 0,
          "stream write");
    char stream_bytes[8]{};
    CHECK(read(stream.writer, stream_bytes, 1) < 0 &&
              read(stream.reader, stream_bytes, sizeof(stream_bytes)) == 3 &&
              stream_bytes[0] == 'a' && stream_bytes[2] == 'c',
          "stream read");
    // The stream takes its full size and gives the bytes back in order.
    static char stream_block[4096];
    for(unsigned i = 0; i < sizeof(stream_block); ++i)
        stream_block[i] = char(i * 7);
    for(unsigned i = 0; i < OS1_STREAM_BYTES / sizeof(stream_block); ++i)
        CHECK(write(stream.writer, stream_block, sizeof(stream_block)) == sizeof(stream_block),
              "stream fill");
    for(unsigned i = 0; i < OS1_STREAM_BYTES / sizeof(stream_block); ++i)
    {
        static char back[sizeof(stream_block)];
        CHECK(read(stream.reader, back, sizeof(back)) == sizeof(back) && back[1] == 7 &&
                  back[4095] == char(4095 * 7),
              "stream drain");
    }
    // A closed writer end gives the end of the stream. A closed reader end refuses a write.
    close(stream.writer);
    CHECK(ipc_wait_many(stream_reader, 1, 0) == 0 &&
              read(stream.reader, stream_bytes, sizeof(stream_bytes)) == 0,
          "stream end");
    close(stream.reader);
    CHECK(stream_create(stream) == 0 && close(stream.reader) == 0 &&
              write(stream.writer, "x", 1) == OS1_ERR_CLOSED,
          "stream without reader");
    close(stream.writer);
    {
        // A second process holds the other ends. Each side waits in the kernel for the other.
        os1_stream_pair down{}, up{};
        CHECK(stream_create(down) == 0 && stream_create(up) == 0, "stream pairs");
        long program = open("/bin/ipccheck", OS1_RIGHT_EXECUTE);
        CHECK(program >= 0, "stream child image");
        os1_launch_request request{};
        request.version = OS1_STARTUP_VERSION;
        request.size = sizeof(request);
        request.startup.version = OS1_STARTUP_VERSION;
        request.startup.size = sizeof(request.startup);
        request.startup.root_fd = request.startup.cwd_fd = -1;
        request.startup.argc = 3;
        constexpr char args[] = "ipccheck\0--stream-child\0x";
        request.startup.argv[1] = 9;
        request.startup.argv[2] = 24;
        request.startup.string_bytes = sizeof(args);
        for(unsigned i = 0; i < sizeof(args); ++i)
            request.startup.strings[i] = args[i];
        request.grant_count = 2;
        request.grants[0] = {down.reader, 3, OS1_RIGHT_READ, 0, 0, 0};
        request.grants[1] = {up.writer, 4, OS1_RIGHT_WRITE, 0, 0, 0};
        const long child = launch(program, OS1_LAUNCH_SPAWN, request);
        close(program);
        close(down.reader);
        close(up.writer);
        CHECK(child > 0, "stream child");
        // The child waits in its read. It has written nothing, and it has not ended.
        const int up_reader[] = {up.reader};
        long now = ipc_clock();
        CHECK(now >= 0 &&
                  ipc_wait_many(up_reader, 1, uint64_t(now) + 50'000'000) == OS1_ERR_TIMED_OUT,
              "stream child waits for a byte");
        int child_status = -1;
        CHECK(waitpid_now(child, &child_status) == OS1_ERR_WOULD_BLOCK, "stream child runs");
        CHECK(write(down.writer, "g", 1) == 1, "byte for the stream child");
        // Give the child time to fill the stream and to wait in its write.
        CHECK(ipc_wait_many(up_reader, 1) == 0, "stream child output");
        now = ipc_clock();
        while(ipc_clock() < now + 50'000'000)
            yield();
        static char back[4096];
        for(unsigned received = 0; received < stream_flood;)
        {
            const long count = read(up.reader, back, sizeof(back));
            CHECK(count > 0 && received + unsigned(count) <= stream_flood, "stream flood read");
            for(long i = 0; i < count; ++i)
                CHECK(back[i] == flood_byte(received + unsigned(i)), "stream flood order");
            received += unsigned(count);
        }
        // The child waits in its second read. A closed writer end ends that read.
        close(down.writer);
        CHECK(read(up.reader, back, 1) == 0, "stream child end");
        close(up.reader);
        CHECK(waitpid(child, &child_status) == child && child_status == 0, "stream child status");
        CHECK(waitpid_now(child, &child_status) == -1, "wait without a child");
    }
    say("[user/ipccheck] byte stream ok\n");
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
    // A wait without a wait returns at once for a child that still runs.
    int running = -1;
    CHECK(waitpid_now(pid, &running) == OS1_ERR_WOULD_BLOCK && running == -1, "wait without wait");
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
