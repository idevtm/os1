#include <os1/log.hpp>
using namespace os1::user;
#define CHECK(c, m)                                         \
    do                                                      \
    {                                                       \
        if(!(c))                                            \
        {                                                   \
            files::message("[user/logcheck] fail " m "\n"); \
            return 1;                                       \
        }                                                   \
    } while(false)
int main()
{
    CHECK(logs::context(OS1_LOG_CONTEXT_OPEN) == 0, "history open");
    os1_log_command command{};
    command.version = 1;
    command.size = sizeof(command);
    command.text[0] = 'x';
    command.cwd[0] = '/';
    CHECK(logs::context(OS1_LOG_COMMAND_BEGIN, &command) == 0 && command.command_id, "begin");
    os1_ipc_pair pair{};
    CHECK(ipc(OS1_IPC_CREATE, 0, &pair) == 0, "pair");
    os1_ipc_info ready{1, sizeof(ready), 0, 42, 1, 1};
    CHECK(ipc(OS1_IPC_READY, pair.provider, &ready) == 0, "ready");
    os1_ipc_message message{};
    message.version = 1;
    message.size = sizeof(message);
    message.interface_id = 42;
    message.interface_version = 1;
    message.member = 1;
    long op = ipc(OS1_IPC_SUBMIT, pair.client, &message);
    CHECK(op >= 0, "submit");
    CHECK(logs::context(OS1_LOG_COMMAND_END, &command) == 0, "end before reply");
    CHECK(ipc(OS1_IPC_RECEIVE, pair.provider, &message) == 0, "receive");
    os1_ipc_result reply{};
    reply.version = 1;
    reply.size = sizeof(reply);
    reply.generation = message.generation;
    reply.request_id = message.request_id;
    CHECK(ipc(OS1_IPC_REPLY, pair.provider, &reply) == 0, "reply");
    close(op);
    close(pair.client);
    close(pair.provider);
    int client = logs::bind();
    CHECK(client >= 0, "bind");
    os1_log_query q{};
    q.version = 1;
    q.size = sizeof(q);
    q.command_id = command.command_id;
    q.kind = OS1_LOG_IPC_END;
    os1_log_record r{};
    CHECK(logs::call(client, OS1_LOG_READ, &q, sizeof(q), &r, sizeof(r)) == sizeof(r),
          "retained completion");
    CHECK(r.command_id == command.command_id && r.request_id && r.pid == uint64_t(getpid()),
          "origin");
    os1_log_history_query h{};
    h.query.version = 1;
    h.query.size = sizeof(h.query);
    CHECK(logs::context(OS1_LOG_HISTORY_READ, &h) == 1, "history read");
    CHECK(h.result.text[0] == 'x' && !(h.result.record.flags & OS1_LOG_PENDING),
          "history completion");
    int reduced = duplicate(root(), OS1_RIGHT_LOOKUP);
    CHECK(reduced >= 0, "attenuation");
    os1_ipc_kernel_binding binding{1, sizeof(binding), OS1_LOG_INTERFACE_ID, 1, OS1_RIGHT_LOG_READ};
    CHECK(ipc(OS1_IPC_BIND_KERNEL, reduced, &binding) == OS1_ERR_ACCESS, "read authority");
    close(reduced);
    close(client);
    constexpr char done[] = "[user/logcheck] correlation authority history ok\n";
    files::put(1, done, sizeof(done) - 1);
    return 0;
}
