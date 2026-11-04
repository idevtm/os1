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
