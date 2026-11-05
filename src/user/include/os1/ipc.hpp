#pragma once
#include <os1/ipc.h>

#include <os1/syscall.hpp>
namespace os1::user
{
inline long ipc(unsigned operation, int fd, void* argument = nullptr)
{
    return os1_syscall3(os1_sys_ipc, operation, fd, reinterpret_cast<uint64_t>(argument));
}
inline long ipc_wait(int fd)
{
    return ipc(OS1_IPC_WAIT, fd);
}
inline long ipc_clock()
{
    return ipc(OS1_IPC_CLOCK, 0);
}
inline long ipc_wait_until(int fd, uint64_t deadline)
{
    os1_ipc_deadline record{1, sizeof(record), deadline};
    return ipc(OS1_IPC_WAIT_UNTIL, fd, &record);
}
inline long ipc_wait_many(const int* fds, unsigned count, uint64_t deadline = UINT64_MAX)
{
    if(!fds || !count || count > OS1_IPC_WAIT_LIMIT)
        return OS1_ERR_INVALID;
    os1_ipc_wait_many record{1, sizeof(record), count, 0, deadline, {}};
    for(unsigned i = 0; i < count; ++i)
        record.fds[i] = fds[i];
    return ipc(OS1_IPC_WAIT_MANY, 0, &record);
}
inline long ipc_account_create(int provider_or_root, unsigned sessions)
{
    os1_ipc_account record{1, sizeof(record), sessions, 0};
    return ipc(OS1_IPC_ACCOUNT_CREATE, provider_or_root, &record);
}
inline long ipc_session_create(int account, os1_ipc_pair& pair)
{
    return ipc(OS1_IPC_SESSION_CREATE, account, &pair);
}
inline long ipc_stop_child(uint64_t pid)
{
    return os1_syscall3(os1_sys_ipc, OS1_IPC_STOP_CHILD, 0, pid);
}
inline long ipc_subscribe(int client, uint32_t member)
{
    os1_ipc_subscription record{1, sizeof(record), member, 0};
    return ipc(OS1_IPC_SUBSCRIBE, client, &record);
}
inline long ipc_unsubscribe(int subscription)
{
    return ipc(OS1_IPC_UNSUBSCRIBE, subscription);
}
inline long ipc_result_until(int operation, os1_ipc_result& result, uint64_t deadline)
{
    for(;;)
    {
        long r = ipc(OS1_IPC_RESULT, operation, &result);
        if(r != OS1_ERR_WOULD_BLOCK)
            return r;
        r = ipc_wait_until(operation, deadline);
        if(r < 0)
            return r;
    }
}
inline long ipc_result_caps_until(int operation, os1_ipc_result_caps& result, uint64_t deadline)
{
    for(;;)
    {
        long r = ipc(OS1_IPC_RESULT_CAPS, operation, &result);
        if(r != OS1_ERR_WOULD_BLOCK)
            return r;
        r = ipc_wait_until(operation, deadline);
        if(r < 0)
            return r;
    }
}
inline void ipc_close_caps(os1_ipc_capabilities& caps)
{
    for(unsigned i = 0; i < caps.count; ++i)
        close(caps.entries[i].fd);
    caps = {};
}
inline long ipc_receive(int fd, os1_ipc_message& message)
{
    for(;;)
    {
        long r = ipc(OS1_IPC_RECEIVE, fd, &message);
        if(r != OS1_ERR_WOULD_BLOCK)
            return r;
        r = ipc_wait(fd);
        if(r < 0)
            return r;
    }
}
inline long ipc_receive_caps(int fd, os1_ipc_message_caps& message)
{
    for(;;)
    {
        long r = ipc(OS1_IPC_RECEIVE_CAPS, fd, &message);
        if(r != OS1_ERR_WOULD_BLOCK)
            return r;
        r = ipc_wait(fd);
        if(r < 0)
            return r;
    }
}
inline long ipc_result(int operation, os1_ipc_result& result)
{
    for(;;)
    {
        long r = ipc(OS1_IPC_RESULT, operation, &result);
        if(r != OS1_ERR_WOULD_BLOCK)
            return r;
        r = ipc_wait(operation);
        if(r < 0)
            return r;
    }
}
inline long ipc_submit_buffers(int client, os1_ipc_message_buffers& request)
{
    return ipc(OS1_IPC_SUBMIT_BUFFERS, client, &request);
}
inline long ipc_reply_buffers(int provider, os1_ipc_result_buffers& result)
{
    return ipc(OS1_IPC_REPLY_BUFFERS, provider, &result);
}
inline long ipc_receive_buffers(int provider, os1_ipc_message_buffers& message)
{
    for(;;)
    {
        long status = ipc(OS1_IPC_RECEIVE_BUFFERS, provider, &message);
        if(status != OS1_ERR_WOULD_BLOCK)
            return status;
        status = ipc_wait(provider);
        if(status < 0)
            return status;
    }
}
inline long ipc_result_buffers(int operation, os1_ipc_result_buffers& result)
{
    for(;;)
    {
        long status = ipc(OS1_IPC_RESULT_BUFFERS, operation, &result);
        if(status != OS1_ERR_WOULD_BLOCK)
            return status;
        status = ipc_wait(operation);
        if(status < 0)
            return status;
    }
}
inline long ipc_result_buffers_until(int operation,
                                     os1_ipc_result_buffers& result,
                                     uint64_t deadline)
{
    for(;;)
    {
        long status = ipc(OS1_IPC_RESULT_BUFFERS, operation, &result);
        if(status != OS1_ERR_WOULD_BLOCK)
            return status;
        status = ipc_wait_until(operation, deadline);
        if(status < 0)
            return status;
    }
}
inline void ipc_close_buffers(os1_ipc_buffers& buffers)
{
    for(unsigned i = 0; i < buffers.count; ++i)
        close(buffers.entries[i].fd);
    buffers = {};
}
inline long ipc_call_buffers(int client,
                             os1_ipc_message_buffers& request,
                             os1_ipc_result_buffers& result)
{
    const long operation = ipc_submit_buffers(client, request);
    if(operation < 0)
        return operation;
    const long status = ipc_result_buffers(operation, result);
    close(operation);
    return status;
}
inline long ipc_call(int client, os1_ipc_message& request, os1_ipc_result& result)
{
    const long operation = ipc(OS1_IPC_SUBMIT, client, &request);
    if(operation < 0)
        return operation;
    const long status = ipc_result(operation, result);
    close(operation);
    return status;
}
}  // namespace os1::user
