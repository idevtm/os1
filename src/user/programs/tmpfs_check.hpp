#pragma once
#include <os1/tmpfs.hpp>
#include <os1/vfs.hpp>
namespace os1::tmpfs_check
{
using namespace os1::user;
inline int child()
{
    char bytes[4]{};
    if(fs::write(3, "bad", 3) != OS1_ERR_ACCESS)
        return 1;
    if(fs::read(3, bytes, 3) != 3 || bytes[0] != 'r' || bytes[1] != 'a' || bytes[2] != 'm')
        return 2;
    return 0;
}
inline bool run()
{
    int root = os1::user::open("/tmp", fs::directory_rights, fs::descendant_rights);
    if(root < 0)
        return false;
    int directory = fs::bind(root, true, fs::directory_rights);
    close(root);
    if(directory < 0)
        return false;
    int subdir = fs::open(
        directory, "check", fs::directory_rights, fs::descendant_rights, OS1_OBJECT_DIRECTORY);
    if(subdir < 0)
        return false;
    int subclient = fs::bind(subdir, true, fs::directory_rights);
    close(subdir);
    if(subclient < 0)
        return false;
    int file = fs::open(subclient, "message", fs::file_rights, 0, OS1_OBJECT_FILE);
    if(file < 0)
        return false;
    int client = fs::bind(file, false, fs::file_rights);
    close(file);
    if(client < 0 || fs::write(client, "ram", 3) != 3)
        return false;
    if(fs::open(subclient, "message", fs::file_rights, 0, OS1_OBJECT_FILE) != OS1_ERR_EXISTS)
        return false;
    if(fs::remove(directory, "check") != OS1_ERR_NOT_EMPTY)
        return false;
    int compatible = os1::user::open("/tmp/check/message", OS1_RIGHT_READ | OS1_RIGHT_SEEK);
    if(compatible < 0)
        return false;
    char bytes[4]{};
    if(os1::user::read(compatible, bytes, 3) != 3 || bytes[0] != 'r' || bytes[1] != 'a' ||
       bytes[2] != 'm')
        return false;
    int observer_file = fs::open(subclient, "message", OS1_RIGHT_READ | OS1_RIGHT_DELEGATE);
    if(observer_file < 0)
        return false;
    int observer = fs::bind(observer_file, false, OS1_RIGHT_READ);
    close(observer_file);
    if(observer < 0)
        return false;
    int image = os1::user::open("/bin/vfscheck", OS1_RIGHT_EXECUTE);
    if(image < 0)
        return false;
    os1_launch_request launch_args{};
    launch_args.version = OS1_STARTUP_VERSION;
    launch_args.size = sizeof(launch_args);
    auto& startup = launch_args.startup;
    startup.version = OS1_STARTUP_VERSION;
    startup.size = sizeof(startup);
    startup.root_fd = startup.cwd_fd = -1;
    startup.argc = 2;
    startup.argv[1] = 9;
    constexpr char args[] = "vfscheck\0tmpfs";
    startup.string_bytes = sizeof(args);
    for(unsigned i = 0; i < sizeof(args); ++i)
        startup.strings[i] = args[i];
    launch_args.grant_count = 1;
    launch_args.grants[0] = {observer, 3, OS1_RIGHT_READ | OS1_RIGHT_WRITE, 0, 0, 0};
    long pid = launch(image, OS1_LAUNCH_SPAWN, launch_args);
    close(image);
    close(observer);
    if(pid < 0)
        return false;
    int status = -1;
    if(waitpid(pid, &status) != pid || status)
        return false;
    if(fs::seek(client, 0, OS1_SEEK_CUR) != 3)
        return false;
    if(fs::seek(client, 4095) != 4095 || fs::write(client, "xy", 2) != 2)
        return false;
    if(fs::truncate(client, 3) || fs::truncate(client, 5))
        return false;
    if(fs::seek(client, 3) != 3 || fs::read(client, bytes, 2) != 2 || bytes[0] || bytes[1])
        return false;
    if(fs::rename(subclient, "message", "renamed"))
        return false;
    os1_directory_entry entry{};
    if(fs::next(subclient, entry) != 1 || entry.name_bytes != 7 || entry.name[0] != 'r')
        return false;
    if(fs::next(subclient, entry) != 0)
        return false;
    if(fs::remove(subclient, "renamed"))
        return false;
    if(os1::user::open("/tmp/check/renamed", OS1_RIGHT_READ) != OS1_ERR_NOT_FOUND)
        return false;
    if(os1::user::seek(compatible, 0) || os1::user::read(compatible, bytes, 3) != 3 ||
       bytes[0] != 'r')
        return false;
    if(fs::seek(client, 0))
        return false;
    long operation = fs::submit(client, false, OS1_FILE_WRITE, "end", 3);
    close(client);
    if(operation < 0)
        return false;
    os1_ipc_result result{};
    if(ipc_result(operation, result) < 0 || result.transport_status || result.application_status)
        return false;
    close(operation);
    if(os1::user::seek(compatible, 0) || os1::user::read(compatible, bytes, 3) != 3 ||
       bytes[0] != 'e')
        return false;
    close(compatible);
    close(subclient);
    if(fs::remove(directory, "check"))
        return false;
    close(directory);
    return true;
}
}  // namespace os1::tmpfs_check
