#include <os1/buffer.hpp>
#include <os1/ipc.hpp>
#include <os1/vfs.hpp>

#include "interface/textprocessor.hpp"
using namespace os1::user;
using namespace os1::textprocessor;
constexpr unsigned rights =
    OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE;
#define CHECK(c, m)                   \
    do                                \
    {                                 \
        if(!(c))                      \
            return failure(__LINE__); \
    } while(0)
void say(const char* s)
{
    unsigned n = 0;
    while(s[n])
        ++n;
    write(1, s, n);
}
void number(uint64_t value)
{
    char text[20];
    unsigned i = sizeof(text);
    do
    {
        text[--i] = '0' + value % 10;
        value /= 10;
    } while(value);
    write(1, text + i, sizeof(text) - i);
}
int failure(unsigned line)
{
    say("[user/buffercheck] fail line=");
    number(line);
    say("\n");
    return 1;
}
template<class T>
void encode(os1_ipc_message& m, const T& value)
{
    m.payload_bytes = sizeof(value);
    for(unsigned i = 0; i < sizeof(value); ++i)
        m.payload[i] = reinterpret_cast<const uint8_t*>(&value)[i];
}
os1_ipc_message message(unsigned member)
{
    os1_ipc_message m{};
    m.version = 1;
    m.size = sizeof(m);
    m.interface_id = interface_id;
    m.interface_version = interface_version;
    m.member = member;
    return m;
}
os1_ipc_message_buffers request(int fd,
                                unsigned member,
                                uint64_t offset = 0,
                                uint64_t length = 4097)
{
    os1_ipc_message_buffers m{};
    m.message = message(member);
    encode(m.message, BufferRange{0, 0, offset, length});
    m.buffers.count = 1;
    m.buffers.entries[0] = {fd, OS1_BUFFER_LOAN};
    return m;
}
os1_ipc_result reply(const os1_ipc_message& m)
{
    os1_ipc_result r{};
    r.version = 1;
    r.size = sizeof(r);
    r.generation = m.generation;
    r.request_id = m.request_id;
    return r;
}
// Equal logical bytes: copied calls use 252-byte chunks; Buffer calls use one loan.
int benchmark(int client)
{
    os1_measurement begin{}, end{};
    CHECK(ipc_measure(begin) == 0, "measurement ABI");
    if(begin.source == OS1_MEASUREMENT_UNAVAILABLE)
    {
        say("[user/buffercheck] timing unavailable high-resolution clock\n");
        return 0;
    }
    CHECK(begin.source == OS1_MEASUREMENT_HPET && begin.resolution_ns, "clock source");
    uint64_t overhead = UINT64_MAX;
    for(unsigned i = 0; i < 32; ++i)
    {
        CHECK(ipc_measure(begin) == 0 && ipc_measure(end) == 0 && end.source == begin.source &&
                  end.nanoseconds >= begin.nanoseconds,
              "timestamp monotonicity");
        const auto elapsed = end.nanoseconds - begin.nanoseconds;
        if(elapsed < overhead)
            overhead = elapsed;
    }
    say("[user/buffercheck] clock hpet resolution_ns=");
    number(end.resolution_ns);
    say(" pair_min_ns=");
    number(overhead);
    say("\n");
    long fd = buffer_create(65536);
    CHECK(fd >= 0, "benchmark allocation");
    void* view = nullptr;
    CHECK(buffer_map(fd, view) == 0, "benchmark mapping");
    auto* data = static_cast<uint8_t*>(view);
    const unsigned sizes[] = {252u, 4096u, 65536u};
    for(unsigned size : sizes)
    {
        const unsigned repeats = size == 252 ? 256 : (size == 4096 ? 64 : 8);
        for(unsigned sample = 0; sample < 3; ++sample)
        {
            uint64_t times[2]{};
            // Alternate order to reduce a consistent first-transport bias.
            for(unsigned order = 0; order < 2; ++order)
            {
                const unsigned transport = (order + sample) % 2;
                for(unsigned i = 0; i < size; ++i)
                    data[i] = 'A';
                CHECK(ipc_measure(begin) == 0 && begin.source, "start clock");
                for(unsigned run = 0; run < repeats; ++run)
                {
                    if(transport == 0)
                    {
                        for(unsigned offset = 0; offset < size; offset += 252)
                        {
                            Text text{};
                            text.length = size - offset > 252 ? 252 : size - offset;
                            for(unsigned i = 0; i < text.length; ++i)
                                text.bytes[i] = data[offset + i];
                            auto call = message(Lowercase);
                            encode(call, text);
                            os1_ipc_result result{};
                            CHECK(ipc_call(client, call, result) == 0 && !result.transport_status &&
                                      !result.application_status &&
                                      result.payload_bytes == sizeof(Text),
                                  "copied benchmark");
                            const auto* output = reinterpret_cast<const Text*>(result.payload);
                            CHECK(output->length == text.length, "copied length");
                            for(unsigned i = 0; i < text.length; ++i)
                                data[offset + i] = output->bytes[i];
                        }
                    }
                    else
                    {
                        auto call = request(fd, LowercaseBuffer, 0, size);
                        os1_ipc_result_buffers result{};
                        CHECK(ipc_call_buffers(client, call, result) == 0 &&
                                  !result.result.transport_status &&
                                  !result.result.application_status && result.buffers.count == 1,
                              "Buffer benchmark");
                        ipc_close_buffers(result.buffers);
                    }
                }
                CHECK(ipc_measure(end) == 0 && end.source == begin.source &&
                          end.nanoseconds >= begin.nanoseconds,
                      "end clock");
                times[transport] = end.nanoseconds - begin.nanoseconds;
                for(unsigned i = 0; i < size; ++i)
                    CHECK(data[i] == 'a', "benchmark output");
            }
            say("[user/buffercheck] timing bytes=");
            number(size);
            say(" repeats=");
            number(repeats);
            say(" sample=");
            number(sample);
            say(" copy_calls=");
            number(((size + 251) / 252) * repeats);
            say(" buffer_calls=");
            number(repeats);
            say(" copied_ns=");
            number(times[0]);
            say(" buffer_ns=");
            number(times[1]);
            say("\n");
        }
    }
    CHECK(buffer_unmap(fd, view) == 0, "benchmark unmap");
    close(fd);
    return 0;
}
long provider(const char* path, os1_ipc_pair& pair)
{
    if(ipc(OS1_IPC_CREATE, 0, &pair) < 0)
        return -1;
    long image = open(path, OS1_RIGHT_EXECUTE);
    if(image < 0)
        return image;
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
    return pid > 0 && ipc_wait(pair.client) == 0 ? pid : -1;
}
// A separate process holds a loan until the caller sends a control request.
int held_provider()
{
    os1_ipc_info info{1, sizeof(info), 0, interface_id, interface_version, 1};
    if(ipc(OS1_IPC_READY, 3, &info) < 0)
        return 2;
    os1_ipc_message_buffers held{};
    if(ipc_receive_buffers(3, held) < 0 || held.buffers.count != 1)
        return 3;
    int fd = held.buffers.entries[0].fd;
    void* mapping = nullptr;
    if(buffer_map(fd, mapping) < 0)
        return 4;
    static_cast<uint8_t*>(mapping)[0] = 'z';
    os1_ipc_event event{};
    event.version = 1;
    event.size = sizeof(event);
    event.member = Progress;
    if(ipc(OS1_IPC_EMIT, 3, &event) < 0)
        return 5;
    os1_ipc_message control{};
    if(ipc_receive(3, control) < 0)
        return 6;
    if(control.member == 3)
    {
        close(3);
        return 0;
    }
    if(control.member == 6)
        reinterpret_cast<void (*)()>(mapping)();
    os1_ipc_result_buffers result{};
    result.result = reply(held.message);
    result.buffers.count = 1;
    result.buffers.entries[0] = {fd, OS1_BUFFER_RETURN};
    if(ipc_reply_buffers(3, result) < 0)
        return 7;
    if(control.member == 4)
        return *static_cast<volatile uint8_t*>(mapping);
    if(control.member == 5)
        *static_cast<volatile uint8_t*>(mapping) = 'x';
    void* stale = nullptr;
    if(buffer_map(fd, stale) != OS1_ERR_ACCESS)
        return 8;
    if(ipc(OS1_IPC_REPLY, 3, mapping) != OS1_ERR_FAULT)
        return 10;
    auto control_reply = reply(control);
    if(ipc(OS1_IPC_REPLY, 3, &control_reply) < 0)
        return 9;
    close(fd);
    close(3);
    return 0;
}
int main()
{
    if(root() == -1)
        return held_provider();
    CHECK(buffer_create(0) == OS1_ERR_INVALID && buffer_create(UINT64_MAX) == OS1_ERR_INVALID &&
              buffer_create(OS1_BUFFER_BYTES_MAX + 1) == OS1_ERR_INVALID,
          "size bounds");
    os1_buffer_create bad_create{1, sizeof(bad_create), 1, 2, 0};
    CHECK(buffer_control(0, OS1_RESOURCE_BUFFER_CREATE, &bad_create) == OS1_ERR_INVALID, "mode");
    long fd = buffer_create(4097);
    CHECK(fd >= 0, "create");
    os1_buffer_info original{};
    CHECK(buffer_info(fd, original) == 0 && original.bytes == 4097 && original.owned &&
              original.mode == OS1_BUFFER_EXCLUSIVE,
          "info");
    os1_resource_info resource{};
    CHECK(query(fd, &resource) == 0 && resource.kind == OS1_OBJECT_BUFFER &&
              (resource.interfaces & OS1_INTERFACE_BUFFER) &&
              resource.descriptor_flags == OS1_DESCRIPTOR_CLOEXEC &&
              !(resource.rights & OS1_RIGHT_EXECUTE),
          "resource");
    CHECK(duplicate(fd, rights | OS1_RIGHT_EXECUTE) == OS1_ERR_ACCESS, "execute right");
    void* mapping = nullptr;
    CHECK(buffer_map(fd, mapping) == 0, "map");
    auto* bytes = static_cast<uint8_t*>(mapping);
    for(unsigned i = 0; i < 8192; ++i)
        CHECK(bytes[i] == 0, "zero storage and padding");
    long alias = duplicate(fd, rights);
    long readonly = duplicate(fd, OS1_RIGHT_READ);
    CHECK(alias >= 0 && readonly >= 0, "aliases");
    void* extra = nullptr;
    CHECK(buffer_map(readonly, extra) == OS1_ERR_ACCESS &&
              buffer_info(readonly, original) == OS1_ERR_ACCESS,
          "rights");
    CHECK(buffer_info(fd, original) == 0, "original identity");
    void* maps[3]{};
    for(auto& m : maps)
        CHECK(buffer_map(alias, m, false) == 0, "map aliases");
    CHECK(buffer_map(fd, extra) == OS1_ERR_LIMIT, "mapping quota");
    for(unsigned i = 1; i < 3; ++i)
        CHECK(buffer_unmap(alias, maps[i]) == 0, "unmap aliases");
    CHECK(buffer_unmap(fd, bytes + 1) == OS1_ERR_INVALID, "exact unmap");
    long allocated[7];
    for(auto& a : allocated)
        CHECK((a = buffer_create(1)) >= 0, "object quota fill");
    CHECK(buffer_create(1) == OS1_ERR_LIMIT, "object quota");
    for(auto a : allocated)
        close(a);
    for(unsigned i = 0; i < 4097; ++i)
        bytes[i] = 'A' + i % 26;
    os1_ipc_pair pair{};
    long pid = provider("/bin/textprocessor", pair);
    CHECK(pid > 0, "text provider");
    auto input = request(fd, LowercaseBuffer);
    auto invalid = input;
    invalid.buffers.count = 2;
    invalid.buffers.entries[1] = {static_cast<int>(alias), OS1_BUFFER_LOAN};
    CHECK(ipc_submit_buffers(pair.client, invalid) == OS1_ERR_INVALID && bytes[0] == 'A',
          "duplicate batch atomicity");
    invalid = input;
    invalid.buffers.entries[0].fd = readonly;
    CHECK(ipc_submit_buffers(pair.client, invalid) == OS1_ERR_ACCESS && bytes[0] == 'A',
          "loan rights");
    os1_ipc_message_caps caps{};
    caps.message = message(Lowercase);
    caps.capabilities.count = 1;
    caps.capabilities.entries[0] = {static_cast<int>(fd), rights, rights, 0};
    CHECK(ipc(OS1_IPC_SUBMIT_CAPS, pair.client, &caps) < 0 && bytes[0] == 'A',
          "legacy rejects Buffer");
    os1_ipc_result_buffers result{};
    CHECK(ipc_call_buffers(pair.client, input, result) == 0 && !result.result.transport_status &&
              !result.result.application_status && result.buffers.count == 1,
          "sync in place");
    os1_buffer_info returned{};
    CHECK(buffer_info(result.buffers.entries[0].fd, returned) == 0 &&
              returned.identity == original.identity,
          "same backing identity");
    for(unsigned i = 0; i < 4097; ++i)
        CHECK(bytes[i] == 'a' + i % 26 && static_cast<uint8_t*>(maps[0])[i] == bytes[i],
              "restored original mapping and alias");
    ipc_close_buffers(result.buffers);
    say("[user/buffercheck] sync same identity restored address ok\n");
    CHECK(benchmark(pair.client) == 0, "benchmark");
    input = request(fd, LowercaseBuffer, UINT64_MAX, 2);
    CHECK(ipc_call_buffers(pair.client, input, result) == 0 &&
              result.result.application_status == OS1_ERR_INVALID && !result.buffers.count &&
              bytes[0] == 'a',
          "range error returns loan");
    input = request(fd, UppercaseBuffer);
    long operation = ipc_submit_buffers(pair.client, input);
    CHECK(operation >= 0, "async distinct submit");
    long operation_alias = duplicate(operation, OS1_RIGHT_READ | OS1_RIGHT_INSPECT);
    CHECK(operation_alias >= 0, "operation alias");
    CHECK(ipc_wait(operation) == 0, "async wait");
    CHECK(ipc(OS1_IPC_RESULT_BUFFERS, operation, reinterpret_cast<void*>(1)) == OS1_ERR_FAULT,
          "claim copy fault");
    long filled[OS1_DESCRIPTOR_LIMIT];
    unsigned filled_count = 0;
    long filled_status = 0;
    while(filled_count < OS1_DESCRIPTOR_LIMIT &&
          (filled_status = duplicate(alias, OS1_RIGHT_READ)) >= 0)
        filled[filled_count++] = filled_status;
    CHECK(filled_status == OS1_ERR_LIMIT &&
              ipc(OS1_IPC_RESULT_BUFFERS, operation, &result) == OS1_ERR_LIMIT,
          "claim descriptor exhaustion");
    while(filled_count)
        close(filled[--filled_count]);
    CHECK(ipc_result_buffers(operation, result) == 0 && !result.result.transport_status &&
              !result.result.application_status && result.buffers.count == 1,
          "distinct result retry");
    int output = result.buffers.entries[0].fd;
    CHECK(buffer_info(output, returned) == 0 && returned.identity != original.identity &&
              returned.bytes == 4097,
          "distinct identity");
    CHECK(buffer_map(output, extra) == 0 && bytes[0] == 'a', "distinct ownership and input return");
    for(unsigned i = 0; i < 4097; ++i)
        CHECK(static_cast<uint8_t*>(extra)[i] == 'A' + i % 26, "output bytes");
    CHECK(ipc_result_buffers(operation_alias, result) == 0 && !result.buffers.count,
          "one claim across aliases");
    CHECK(
        ipc(OS1_IPC_RESULT_BUFFERS, operation_alias, reinterpret_cast<void*>(1)) == OS1_ERR_FAULT &&
            buffer_info(output, returned) == 0 && returned.owned &&
            static_cast<uint8_t*>(extra)[0] == 'A',
        "failed repeated claim preserves output");
    buffer_unmap(output, extra);
    close(output);
    close(operation_alias);
    close(operation);
    close(pair.client);
    int status = -1;
    CHECK(waitpid(pid, &status) == pid && status == 0, "text provider exit");
    say("[user/buffercheck] async distinct output one claim ok\n");
    // A notification confirms delivery before each timeout, closure, or failure check.
    for(unsigned mode = 0; mode < 6; ++mode)
    {
        pid = provider("/bin/buffercheck", pair);
        CHECK(pid > 0, "held provider");
        long events = ipc_subscribe(pair.client, Progress);
        CHECK(events >= 0, "held subscription");
        input = request(fd, 1);
        operation = ipc_submit_buffers(pair.client, input);
        CHECK(operation >= 0 && ipc_wait(events) == 0, "held delivery");
        os1_ipc_event event{};
        CHECK(ipc(OS1_IPC_EVENT_READ, events, &event) == 0, "held notification");
        CHECK(buffer_map(alias, extra) == OS1_ERR_ACCESS &&
                  buffer_map(readonly, extra, false) == OS1_ERR_ACCESS,
              "all aliases revoked");
        CHECK(write(1, bytes, 1) == OS1_ERR_FAULT, "kernel copy rejects revoked mapping");
        CHECK(ipc_result_buffers_until(operation, result, ipc_clock()) == OS1_ERR_TIMED_OUT &&
                  buffer_map(fd, extra) == OS1_ERR_ACCESS,
              "timeout retains loan");
        if(mode == 2)
        {
            CHECK(ipc_stop_child(pid) == 0, "stop provider");
            CHECK(ipc_result_buffers(operation, result) == 0 &&
                      result.result.transport_status == OS1_ERR_OUTCOME_UNKNOWN,
                  "provider failure outcome");
        }
        else
        {
            if(mode == 1)
                close(operation);
            auto control = message(mode >= 3 ? mode + 1 : (mode == 1 ? 3 : 2));
            os1_ipc_result control_result{};
            CHECK(ipc_call(pair.client, control, control_result) == 0, "release held request");
            if(mode >= 3)
            {
                CHECK(control_result.transport_status == OS1_ERR_OUTCOME_UNKNOWN,
                      "fault closes provider");
                CHECK(
                    ipc_result_buffers(operation, result) == 0 &&
                        result.result.transport_status == (mode == 5 ? OS1_ERR_OUTCOME_UNKNOWN : 0),
                    "fault restores input");
                ipc_close_buffers(result.buffers);
            }
            if(mode == 0)
            {
                CHECK(!control_result.transport_status, "stale borrower checks");
                CHECK(ipc_result_buffers(operation, result) == 0 && result.buffers.count == 1,
                      "held result");
                ipc_close_buffers(result.buffers);
            }
        }
        CHECK(buffer_info(fd, returned) == 0 && returned.owned &&
                  returned.identity == original.identity && bytes[0] == 'z',
              "failure restores modified bytes without rollback");
        if(mode != 1)
            close(operation);
        close(events);
        close(pair.client);
        CHECK(waitpid(pid, &status) == pid &&
                  status == (mode >= 3 ? 14 : (mode == 2 ? OS1_PROVIDER_STOP_STATUS : 0)),
              "held provider exit");
    }
    say("[user/buffercheck] revoked read write and execute faults recovered\n");
    CHECK(buffer_unmap(alias, maps[0]) == 0 && buffer_unmap(fd, mapping) == 0, "final unmap");
    close(readonly);
    close(alias);
    close(fd);
    say("[user/buffercheck] timeout aliases provider failure recovery ok\n[user/buffercheck] ok\n");
    return 0;
}
