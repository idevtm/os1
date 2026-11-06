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
    if(unexpected || r.result.payload_bytes != (member == counter::Identity ? 8u : 16u))
        return OS1_ERR_INVALID;
    value = constructor::decode(r.result.payload, 8);
    return 0;
}
long event_value(int sub, uint64_t& value, uint64_t revision = UINT64_MAX)
{
    long status = ipc_wait_until(sub, activation::deadline());
    if(status < 0)
        return status;
    os1_ipc_event event{};
    status = ipc(OS1_IPC_EVENT_READ, sub, &event);
    if(status < 0)
        return status;
    if(event.member != counter::Changed || event.payload_bytes != 16 || event.dropped ||
       (revision != UINT64_MAX && constructor::decode(event.payload + 8, 8) != revision))
        return OS1_ERR_INVALID;
    value = constructor::decode(event.payload, 8);
    return 0;
}
// Fault fixture: advertises the registered class but returns another interface.
// The manager must reject the reference, stop this child, and never replay it.
int invalid_provider()
{
    os1_ipc_info ready{1, sizeof(ready), 0, activation::counter_control_id, 1, 1};
    if(ipc(OS1_IPC_READY, 3, &ready) < 0)
        return 91;
    os1_ipc_message_caps m{};
    if(ipc_receive_caps(3, m) < 0)
        return 92;
    ipc_close_caps(m.capabilities);
    if(constructor::decode(m.message.payload, 8) == 30)
        return 97;  // Provider death after delivery must report unknown outcome, no replay.
    os1_ipc_pair wrong{};
    if(ipc(OS1_IPC_CREATE, 0, &wrong) < 0)
        return 93;
    ready.interface_id = 42;
    if(ipc(OS1_IPC_READY, wrong.provider, &ready) < 0)
        return 94;
    os1_ipc_result_caps result{};
    result.result = {1,
                     sizeof(result.result),
                     m.message.generation,
                     m.message.request_id,
                     0,
                     0,
                     activation::reference_bytes,
                     0,
                     {}};
    constructor::encode(result.result.payload + 16, 8, 1);
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
long start_manager(const char* class_path, os1_ipc_pair& manager, os1_ipc_pair& observer)
{
    long image = open("/disk/bin/activation", OS1_RIGHT_EXECUTE);
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
    status = ipc(OS1_IPC_CREATE, 0, &observer);
    if(status < 0)
    {
        close(manager.client);
        close(manager.provider);
        close(image);
        close(registered);
        return status;
    }
    os1_launch_request r{};
    activation::launch_record(r);
    r.grant_count = 3;
    r.grants[0] = {manager.provider, 3, OS1_RIGHT_READ | OS1_RIGHT_WRITE, 0, 0, 0};
    r.grants[1] = {
        static_cast<int32_t>(registered), 4, OS1_RIGHT_EXECUTE | OS1_RIGHT_DELEGATE, 0, 0, 0};
    r.grants[2] = {observer.provider, 5, OS1_RIGHT_READ | OS1_RIGHT_WRITE, 0, 0, 0};
    long pid = launch(image, OS1_LAUNCH_SPAWN, r);
    close(image);
    close(registered);
    close(manager.provider);
    close(observer.provider);
    if(pid < 0)
    {
        close(manager.client);
        close(observer.client);
    }
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
        CHECK(event_value(sub, value, 1) == 0 && value == 15, "second broadcast");
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
    os1_ipc_pair manager{}, observer{};
    long manager_pid = start_manager("/disk/bin/counter", manager, observer);
    os1_launch_request r{};
    long image = -1;
    CHECK(manager_pid > 0, "manager launch");
    CHECK(ipc_wait_until(manager.client, activation::deadline()) == 0, "manager ready");
    CHECK(activation::acquire_counter(manager.client, 101, 100) == OS1_ERR_INVALID,
          "invalid construction");
    // Queue two constructions before collecting either result. The manager
    // serializes activation; the class binds two sessions to one singleton.
    auto construction = activation::request(10, 100);
    long operations[2];
    for(auto& op : operations)
    {
        op = ipc(OS1_IPC_SUBMIT, manager.client, &construction);
        CHECK(op >= 0, "queued construction");
    }
    int objects[2];
    uint64_t identities[2];
    for(unsigned i = 0; i < 2; ++i)
    {
        os1_ipc_result_caps result{};
        CHECK(ipc_result_caps_until(operations[i], result, activation::deadline()) == 0 &&
                  !result.result.transport_status && !result.result.application_status &&
                  activation::validate_counter_reference(result) == 0,
              "queued result");
        objects[i] = result.capabilities.entries[0].fd;
        identities[i] = constructor::decode(result.result.payload + 16, 8);
        close(operations[i]);
    }
    os1_ipc_info original{}, parallel{};
    CHECK(ipc(OS1_IPC_INFO, objects[0], &original) == 0 &&
              ipc(OS1_IPC_INFO, objects[1], &parallel) == 0 &&
              original.generation != parallel.generation && identities[0] == identities[1],
          "independent sessions shared singleton identity");
    uint64_t value = 0;
    CHECK(counter_call(objects[0], counter::Identity, 0, value) == 0 && value == identities[0],
          "session identity metadata");
    os1_ipc_message read{};
    read.version = 1;
    read.size = sizeof(read);
    read.interface_version = 1;
    read.interface_id = counter::interface_id;
    read.member = counter::Read;
    read.payload_bytes = 0;
    long retained[4];
    for(auto& op : retained)
    {
        op = ipc(OS1_IPC_SUBMIT, objects[0], &read);
        CHECK(op >= 0, "fill first session operations");
        os1_ipc_result result{};
        CHECK(ipc_result_until(op, result, activation::deadline()) == 0 &&
                  !result.transport_status && !result.application_status,
              "retain first session completion");
    }
    CHECK(ipc(OS1_IPC_SUBMIT, objects[0], &read) == OS1_ERR_LIMIT,
          "first session operation budget");
    CHECK(counter_call(objects[1], counter::Read, 0, value) == 0 && value == 10,
          "other session survives operation saturation");
    CHECK(activation::acquire_counter(manager.client, 10, 100) == OS1_ERR_LIMIT,
          "mutable grant session budget");
    for(auto op : retained)
        close(op);
    CHECK(ipc_wait_until(observer.client, activation::deadline()) == 0, "observer grant ready");
    uint64_t observer_identity = 0;
    long observed = activation::acquire_counter(observer.client, 10, 100, &observer_identity);
    CHECK(observed >= 0 && observer_identity == identities[0], "separate observer reservation");
    CHECK(counter_call(observed, counter::Read, 0, value) == 0 && value == 10, "observer raw read");
    CHECK(counter_call(observed, counter::Add, 1, value) == OS1_ERR_ACCESS &&
              counter_call(observed, counter::Reset, 0, value) == OS1_ERR_ACCESS,
          "observer raw mutation denied");
    CHECK(counter_call(observed, counter::Construct, 0, value) == OS1_ERR_UNSUPPORTED &&
              counter_call(objects[0], counter::Construct, 0, value) == OS1_ERR_UNSUPPORTED,
          "data sessions cannot construct");
    auto forged = activation::request(10, 100);
    forged.payload_bytes = 40;
    constructor::encode(forged.payload + 32, 8, activation::mutable_access);
    os1_ipc_result_caps denied{};
    CHECK(activation::call(observer.client, forged, denied) == 0 &&
              denied.result.application_status == OS1_ERR_INVALID && !denied.capabilities.count,
          "caller cannot claim policy");
    long observer_subscription = ipc_subscribe(observed, counter::Changed);
    CHECK(observer_subscription >= 0, "observer subscription");
    long extra_subscription = ipc_subscribe(observed, counter::Changed);
    CHECK(extra_subscription >= 0 && ipc_subscribe(observed, counter::Changed) == OS1_ERR_LIMIT,
          "per session subscription budget");
    CHECK(ipc_unsubscribe(extra_subscription) == 0 &&
              ipc_subscribe(observed, counter::Changed) == OS1_ERR_LIMIT,
          "unsubscribed retained object remains charged");
    close(extra_subscription);
    extra_subscription = ipc_subscribe(observed, counter::Changed);
    CHECK(extra_subscription >= 0, "closed subscription refunds slot");
    close(extra_subscription);
    long first = objects[0];
    close(objects[1]);
    long subscription = ipc_subscribe(first, counter::Changed);
    CHECK(subscription >= 0, "first subscription");
    os1_ipc_event event{};
    CHECK(ipc(OS1_IPC_EVENT_READ, subscription, &event) == OS1_ERR_WOULD_BLOCK,
          "no initial replay");
    os1_ipc_pair sync{};
    CHECK(ipc(OS1_IPC_CREATE, 0, &sync) == 0, "sync pair");
    image = open("/disk/bin/activationcheck", OS1_RIGHT_EXECUTE);
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
    CHECK(counter_call(first, counter::Add, 5, value) == 0 && value == 15, "first mutation");
    CHECK(event_value(subscription, value, 1) == 0 && value == 15, "first broadcast");
    CHECK(event_value(subscription, value, 2) == 0 && value == 17, "second broadcast");
    CHECK(event_value(observer_subscription, value, 1) == 0 && value == 15 &&
              event_value(observer_subscription, value, 2) == 0 && value == 17,
          "observer independent broadcast");
    CHECK(ipc_unsubscribe(observer_subscription) == 0, "observer unsubscribe");
    close(observer_subscription);
    close(observed);
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
              a.generation != b.generation &&
              counter_call(again, counter::Identity, 0, value) == 0 && value == identities[0],
          "new session same object identity");
    close(again);
    long alias = duplicate(manager.client, activation::client_rights);
    CHECK(alias >= 0, "grant alias");
    for(unsigned attempt = 0; attempt < 2; ++attempt)
    {
        long transient = activation::acquire_counter(alias, 10, 100);
        CHECK(transient >= 0, "retained resource session acquire");
        long result_handle = ipc(OS1_IPC_SUBMIT, transient, &read);
        CHECK(result_handle >= 0, "retained resource operation");
        os1_ipc_result result{};
        CHECK(ipc_result_until(result_handle, result, activation::deadline()) == 0 &&
                  !result.transport_status && !result.application_status,
              "retained resource completion");
        close(transient);
        CHECK(activation::acquire_counter(manager.client, 10, 100) == OS1_ERR_LIMIT &&
                  activation::acquire_counter(alias, 10, 100) == OS1_ERR_LIMIT,
              "closed session and aliases retain admission charge");
        long independent = activation::acquire_counter(observer.client, 10, 100);
        CHECK(independent >= 0 && counter_call(independent, counter::Read, 0, value) == 0 &&
                  value == 20,
              "retained mutable charge preserves observer budget");
        close(independent);
        close(result_handle);
    }
    close(alias);
    close(observer.client);  // One grant closing does not end the other grant's scope.
    CHECK(counter_call(first, counter::Read, 0, value) == 0 && value == 20,
          "other grant survives closure");
    CHECK(ipc_unsubscribe(subscription) == 0, "unsubscribe");
    close(subscription);
    long shutdown_subscription = ipc_subscribe(first, counter::Changed);
    CHECK(shutdown_subscription >= 0, "shutdown subscription");
    close(manager.client);  // Explicitly end this test's activation-service scope.
    CHECK(waitpid(manager_pid, &status) == manager_pid && status == 0, "manager shutdown");
    CHECK(counter_call(first, counter::Read, 0, value) == OS1_ERR_PEER_CLOSED,
          "provider supervised shutdown");
    CHECK(ipc_wait_until(shutdown_subscription, activation::deadline()) == 0 &&
              ipc(OS1_IPC_EVENT_READ, shutdown_subscription, &event) == OS1_ERR_PEER_CLOSED,
          "provider shutdown wakes session subscription");
    close(shutdown_subscription);
    close(first);
    for(unsigned fault = 0; fault < 3; ++fault)
    {
        manager_pid = start_manager("/disk/bin/activationcheck", manager, observer);
        CHECK(manager_pid > 0 && ipc_wait_until(manager.client, activation::deadline()) == 0,
              "invalid provider manager");
        for(unsigned i = 0; i < 2; ++i)
            CHECK(activation::acquire_counter(manager.client, 10 + fault * 10, 100) ==
                      (fault == 2 ? OS1_ERR_OUTCOME_UNKNOWN
                       : fault    ? OS1_ERR_INVALID
                                  : OS1_ERR_UNSUPPORTED),
                  "invalid provider reply rejected without replay");
        close(manager.client);
        close(observer.client);
        CHECK(waitpid(manager_pid, &status) == manager_pid && status == 0,
              "invalid provider stopped");
    }
    constexpr char done[] = "[user/activationcheck] ok\n";
    write(1, done, sizeof(done) - 1);
    return 0;
}
