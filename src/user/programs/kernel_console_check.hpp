#pragma once
#include <os1/console.hpp>
#include <os1/vfs.hpp>

namespace os1::kernel_console_check
{
using namespace os1::user;
inline int child()
{
    // This child receives only an inspect-only Console client at fd 3.
    // Transport WRITE permits submission but cannot grant console write authority.
    constexpr char denied[] = "must not be printed";
    if(console_write(3, denied, sizeof(denied) - 1) != OS1_ERR_ACCESS)
        return 1;
    auto request = console_request(OS1_CONSOLE_GET_INFO);
    os1_ipc_result result{};
    if(ipc_call(3, request, result) < 0 || result.transport_status || result.application_status ||
       result.payload_bytes != sizeof(os1_console_info))
        return 2;
    return result.payload[0] == OS1_RIGHT_INSPECT ? 0 : 3;
}
inline bool run()
{
    int client = console_bind(1, OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT);
    int observer = console_bind(1, OS1_RIGHT_INSPECT);
    if(client < 0 || observer < 0)
        return false;
    int image = open("/disk/bin/ipccheck", OS1_RIGHT_EXECUTE);
    if(image < 0)
        return false;
    os1_launch_request launch_request{};
    launch_request.version = OS1_STARTUP_VERSION;
    launch_request.size = sizeof(launch_request);
    auto& startup = launch_request.startup;
    startup.version = OS1_STARTUP_VERSION;
    startup.size = sizeof(startup);
    startup.root_fd = startup.cwd_fd = -1;
    startup.argc = 2;
    constexpr char args[] = "ipccheck\0--kernel-console-child";
    startup.argv[1] = 9;
    startup.string_bytes = sizeof(args);
    for(unsigned i = 0; i < sizeof(args); ++i)
        startup.strings[i] = args[i];
    launch_request.grant_count = 1;
    launch_request.grants[0] = {observer, 3, OS1_RIGHT_READ | OS1_RIGHT_WRITE, 0, 0, 0};
    long pid = launch(image, OS1_LAUNCH_SPAWN, launch_request);
    close(image);
    close(observer);
    if(pid < 0)
        return false;
    constexpr char sync[] = "[user/ipccheck] kernel console sync\n";
    if(console_write(client, sync, sizeof(sync) - 1) != sizeof(sync) - 1)
        return false;
    constexpr char async[] = "[user/ipccheck] kernel console async\n";
    long operation = console_write_async(client, async, sizeof(async) - 1);
    if(operation < 0)
        return false;
    close(client);  // Accepted work retains the console independently.
    os1_ipc_result result{};
    if(ipc_result(operation, result) < 0 || result.transport_status || result.application_status ||
       result.payload_bytes != sizeof(os1_console_written))
        return false;
    os1_ipc_result again{};
    if(ipc(OS1_IPC_RESULT, operation, &again) < 0 || again.request_id != result.request_id ||
       again.payload_bytes != result.payload_bytes)
        return false;
    close(operation);
    int status = -1;
    return waitpid(pid, &status) == pid && status == 0;
}
}  // namespace os1::kernel_console_check
