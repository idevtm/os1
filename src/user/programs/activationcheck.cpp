#include <os1/activation.hpp>
using namespace os1::user;
namespace counter = os1::counter;
namespace constructor = os1::constructor;
#define CHECK(c, m)                                                     \
    do                                                                  \
    {                                                                   \
        if(!(c))                                                        \
        {                                                               \
            constexpr char s[] = "[user/activationcheck] fail " m "\n"; \
            write(2, s, sizeof(s) - 1);                                 \
            return 1;                                                   \
        }                                                               \
    } while(0)
long counter_call(int fd, unsigned member, uint64_t amount, uint64_t& value)
{
    os1_ipc_message m{};
    m.version = 1;
    m.size = sizeof(m);
    m.interface_id = counter::interface_id;
    m.interface_version = 1;
    m.member = member;
    if(member == counter::Add)
    {
        m.payload_bytes = 8;
        constructor::encode(m.payload, 8, amount);
    }
    os1_ipc_result_caps r{};
    long status = activation::call(fd, m, r);
    if(status < 0)
        return status;
    bool unexpected = r.capabilities.count;
    ipc_close_caps(r.capabilities);
    if(r.result.transport_status)
        return r.result.transport_status;
    if(r.result.application_status)
        return r.result.application_status;
    if(unexpected || r.result.payload_bytes != 16)
        return OS1_ERR_INVALID;
    value = constructor::decode(r.result.payload, 8);
    return 0;
}
long event_value(int sub, uint64_t& value)
{
    long status = ipc_wait_until(sub, activation::deadline());
    if(status < 0)
        return status;
    os1_ipc_event event{};
    status = ipc(OS1_IPC_EVENT_READ, sub, &event);
    if(status < 0)
        return status;
    if(event.member != counter::Changed || event.payload_bytes != 16 || event.dropped)
        return OS1_ERR_INVALID;
    value = constructor::decode(event.payload, 8);
    return 0;
}
// Fault fixture: advertises the registered class but returns another interface.
// The manager must reject the reference, stop this child, and never replay it.
int invalid_provider()
{
    os1_ipc_info ready{1, sizeof(ready), 0, counter::interface_id, 1, 1};
    if(ipc(OS1_IPC_READY, 3, &ready) < 0)
        return 91;
    os1_ipc_message_caps m{};
    if(ipc_receive_caps(3, m) < 0)
        return 92;
    ipc_close_caps(m.capabilities);
    os1_ipc_pair wrong{};
    if(ipc(OS1_IPC_CREATE, 0, &wrong) < 0)
        return 93;
    ready.interface_id = 42;
    if(ipc(OS1_IPC_READY, wrong.provider, &ready) < 0)
        return 94;
    os1_ipc_result_caps result{};
    result.result = {
        1, sizeof(result.result), m.message.generation, m.message.request_id, 0, 0, 16, 0, {}};
    result.capabilities.count = 1;
    result.capabilities.entries[0] = {wrong.client, activation::client_rights, 0, 0};
    if(constructor::decode(m.message.payload, 8) == 20)
        result.result.application_status = 7;  // Must never be mistaken for fd 7.
    if(ipc(OS1_IPC_REPLY_CAPS, 3, &result) < 0)
        return 95;
    for(;;)
    {
        if(ipc_wait(3) < 0)
            return 96;
    }
}
long start_manager(const char* class_path, os1_ipc_pair& manager)
{
    long image = open("/bin/activation", OS1_RIGHT_EXECUTE);
    if(image < 0)
        return image;
    long registered = open(class_path, OS1_RIGHT_EXECUTE | OS1_RIGHT_DELEGATE);
    if(registered < 0)
    {
        close(image);
        return registered;
    }
    long status = ipc(OS1_IPC_CREATE, 0, &manager);
    if(status < 0)
    {
        close(image);
        close(registered);
        return status;
    }
    os1_launch_request r{};
    activation::launch_record(r);
    r.grant_count = 2;
    r.grants[0] = {manager.provider, 3, OS1_RIGHT_READ | OS1_RIGHT_WRITE, 0, 0, 0};
    r.grants[1] = {
        static_cast<int32_t>(registered), 4, OS1_RIGHT_EXECUTE | OS1_RIGHT_DELEGATE, 0, 0, 0};
    long pid = launch(image, OS1_LAUNCH_SPAWN, r);
    close(image);
    close(registered);
    close(manager.provider);
    if(pid < 0)
        close(manager.client);
    return pid;
}
int main(int argc, char** argv)
{
    StartupContext context{*startup_record};
    if(context.environment("OS1_ACTIVATION"))
        return invalid_provider();
    if(argc == 1 && constructor::equal(argv[0], "client"))
    {
        long object = activation::acquire_counter(3, 10, 100);
        CHECK(object >= 0, "second acquire");
        long sub = ipc_subscribe(object, counter::Changed);
        CHECK(sub >= 0, "second subscription");
        os1_ipc_info ready{1, sizeof(ready), 0, activation::interface_id, 1, 1};
        CHECK(ipc(OS1_IPC_READY, 4, &ready) == 0, "second ready");
        uint64_t value = 0;
        CHECK(event_value(sub, value) == 0 && value == 15, "second broadcast");
        CHECK(counter_call(object, counter::Read, 0, value) == 0 && value == 15, "shared read");
        CHECK(counter_call(object, counter::Add, 2, value) == 0 && value == 17, "second mutation");
        CHECK(ipc_unsubscribe(sub) == 0, "second unsubscribe");
        close(sub);
        close(object);
        close(3);
        constexpr char done[] = "[user/activationcheck] second client ok\n";
        write(1, done, sizeof(done) - 1);
        return 0;
    }
    os1_ipc_pair manager{};
    long manager_pid = start_manager("/bin/counter", manager);
    os1_launch_request r{};
    long image = -1;
    CHECK(manager_pid > 0, "manager launch");
    CHECK(ipc_wait_until(manager.client, activation::deadline()) == 0, "manager ready");
    CHECK(activation::acquire_counter(manager.client, 101, 100) == OS1_ERR_INVALID,
          "invalid construction");
    // Queue two constructions before collecting either result. The manager
    // serializes activation; the class returns the same singleton twice.
    auto construction = activation::request(10, 100);
    long operations[2];
    for(auto& op : operations)
    {
        op = ipc(OS1_IPC_SUBMIT, manager.client, &construction);
        CHECK(op >= 0, "queued construction");
    }
    int objects[2];
    for(unsigned i = 0; i < 2; ++i)
    {
        os1_ipc_result_caps result{};
        CHECK(ipc_result_caps_until(operations[i], result, activation::deadline()) == 0 &&
                  !result.result.transport_status && !result.result.application_status &&
                  activation::validate_counter_reference(result) == 0,
              "queued result");
        objects[i] = result.capabilities.entries[0].fd;
        close(operations[i]);
    }
    os1_ipc_info original{}, parallel{};
    CHECK(ipc(OS1_IPC_INFO, objects[0], &original) == 0 &&
              ipc(OS1_IPC_INFO, objects[1], &parallel) == 0 &&
              original.generation == parallel.generation,
          "queued singleton identity");
    long first = objects[0];
    close(objects[1]);
    long subscription = ipc_subscribe(first, counter::Changed);
    CHECK(subscription >= 0, "first subscription");
    os1_ipc_event event{};
    CHECK(ipc(OS1_IPC_EVENT_READ, subscription, &event) == OS1_ERR_WOULD_BLOCK,
          "no initial replay");
    os1_ipc_pair sync{};
    CHECK(ipc(OS1_IPC_CREATE, 0, &sync) == 0, "sync pair");
    image = open("/bin/activationcheck", OS1_RIGHT_EXECUTE);
    CHECK(image >= 0, "client image");
    activation::launch_record(r);
    r.grant_count = 4;
    r.grants[0] = {manager.client, 3, OS1_RIGHT_READ | OS1_RIGHT_WRITE, 0, 0, 0};
    r.grants[1] = {sync.provider, 4, OS1_RIGHT_READ | OS1_RIGHT_WRITE, 0, 0, 0};
    r.grants[2] = {1, 1, OS1_RIGHT_WRITE, 0, 0, 0};
    r.grants[3] = {2, 2, OS1_RIGHT_WRITE, 0, 0, 0};
    constexpr char name[] = "client";
    r.startup.argc = 1;
    r.startup.string_bytes = sizeof(name);
    for(unsigned i = 0; i < sizeof(name); ++i)
        r.startup.strings[i] = name[i];
    long child = launch(image, OS1_LAUNCH_SPAWN, r);
    close(image);
    close(sync.provider);
    CHECK(child > 0, "second launch");
    CHECK(ipc_wait_until(sync.client, activation::deadline()) == 0, "subscribed barrier");
    uint64_t value = 0;
    CHECK(counter_call(first, counter::Add, 5, value) == 0 && value == 15, "first mutation");
    CHECK(event_value(subscription, value) == 0 && value == 15, "first broadcast");
    CHECK(event_value(subscription, value) == 0 && value == 17, "second broadcast");
    int status = -1;
    CHECK(waitpid(child, &status) == child && status == 0, "second exit");
    close(sync.client);
    CHECK(counter_call(first, counter::Add, 3, value) == 0 && value == 20,
          "independent disconnect");
    CHECK(activation::acquire_counter(manager.client, 11, 100) == OS1_ERR_INVALID,
          "class rejects conflicting args");
    long again = activation::acquire_counter(manager.client, 10, 100);
    CHECK(again >= 0, "reacquire");
    CHECK(counter_call(again, counter::Read, 0, value) == 0 && value == 20,
          "singleton preserves state");
    os1_ipc_info a{}, b{};
    CHECK(ipc(OS1_IPC_INFO, first, &a) == 0 && ipc(OS1_IPC_INFO, again, &b) == 0 &&
              a.generation == b.generation,
          "same object identity");
    close(again);
    CHECK(ipc_unsubscribe(subscription) == 0, "unsubscribe");
    close(subscription);
    close(manager.client);  // Explicitly end this test's activation-service scope.
    CHECK(waitpid(manager_pid, &status) == manager_pid && status == 0, "manager shutdown");
    CHECK(counter_call(first, counter::Read, 0, value) == OS1_ERR_PEER_CLOSED,
          "provider supervised shutdown");
    close(first);
    for(unsigned fault = 0; fault < 2; ++fault)
    {
        manager_pid = start_manager("/bin/activationcheck", manager);
        CHECK(manager_pid > 0 && ipc_wait_until(manager.client, activation::deadline()) == 0,
              "invalid provider manager");
        for(unsigned i = 0; i < 2; ++i)
            CHECK(activation::acquire_counter(manager.client, fault ? 20 : 10, 100) ==
                      (fault ? OS1_ERR_INVALID : OS1_ERR_UNSUPPORTED),
                  "invalid provider reply rejected without replay");
        close(manager.client);
        CHECK(waitpid(manager_pid, &status) == manager_pid && status == 0,
              "invalid provider stopped");
    }
    constexpr char done[] = "[user/activationcheck] ok\n";
    write(1, done, sizeof(done) - 1);
    return 0;
}
