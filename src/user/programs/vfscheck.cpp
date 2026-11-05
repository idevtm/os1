#include <os1/vfs.hpp>

#include "tmpfs_check.hpp"

using namespace os1::user;
#define CHECK(condition, message)                                               \
    do                                                                          \
    {                                                                           \
        if(!(condition))                                                        \
        {                                                                       \
            static const char failure[] = "[user/vfscheck] fail " message "\n"; \
            write(2, failure, sizeof(failure) - 1);                             \
            return 1;                                                           \
        }                                                                       \
    } while(0)

bool equal(const char* a, const char* b)
{
    while(*a && *a == *b)
    {
        ++a;
        ++b;
    }
    return *a == *b;
}

int main(int argc, char** argv)
{
    if(argc == 2 && equal(argv[1], "tmpfs"))
        return os1::tmpfs_check::child();
    if(argc == 2)
    {
        CHECK(equal(argv[0], "vfscheck") && equal(argv[1], "restricted"), "argv");
        CHECK(startup_record->envc == 1 &&
                  equal(startup_record->strings + startup_record->envp[0], "MODE=test"),
              "environment");
        CHECK(root() == -1 && cwd() == -1, "namespace roles");
        CHECK(open("/etc/motd", OS1_RIGHT_READ) == OS1_ERR_BAD_DESCRIPTOR, "no ambient root");
        CHECK(read(0, nullptr, 0) == OS1_ERR_BAD_DESCRIPTOR, "no implicit stdin");
        static const char success[] = "[user/vfscheck] restricted child ok\n";
        write(1, success, sizeof(success) - 1);
        return 0;
    }
    CHECK(os1::tmpfs_check::run(), "tmpfs protocol");
    constexpr char tmp_ok[] = "[user/vfscheck] tmpfs native two clients compatibility unlink ok\n";
    write(1, tmp_ok, sizeof(tmp_ok) - 1);
    const long file = open("/etc/motd", OS1_RIGHT_READ | OS1_RIGHT_SEEK | OS1_RIGHT_DELEGATE);
    CHECK(file >= 0, "open file");
    const long alias = duplicate(file, OS1_RIGHT_READ | OS1_RIGHT_SEEK);
    CHECK(alias >= 0, "duplicate file");
    char bytes[5]{};
    CHECK(read(file, bytes, 4) == 4 && equal(bytes, "os1:"), "file bytes");
    CHECK(seek(alias, 0, OS1_SEEK_CUR) == 4, "shared cursor");
    CHECK(seek(alias, 0) == 0 && read(file, bytes, 4) == 4, "seek alias");
    close(alias);
    close(file);

    const long directory = open("/bin", OS1_RIGHT_ENUMERATE);
    CHECK(directory >= 0, "open directory");
    alignas(8) char record[OS1_DIRECTORY_RECORD_MAX]{};
    uint32_t required = 0;
    CHECK(read_directory(directory, record, 1, &required) == OS1_ERR_BUFFER_TOO_SMALL &&
              required >= sizeof(os1_directory_record) && required <= sizeof(record),
          "record sizing");
    CHECK(read_directory(directory, record, required) == required, "whole record");
    CHECK(open_at(directory, "vfscheck", 8, OS1_RIGHT_EXECUTE) == OS1_ERR_ACCESS,
          "enumeration authority");
    close(directory);

    const long executable = open("/bin/vfscheck", OS1_RIGHT_EXECUTE);
    CHECK(executable >= 0, "open executable");
    os1_launch_request request{};
    request.version = OS1_STARTUP_VERSION;
    request.size = sizeof(request);
    request.startup.version = OS1_STARTUP_VERSION;
    request.startup.size = sizeof(request.startup);
    request.startup.root_fd = -1;
    request.startup.cwd_fd = -1;
    request.startup.argc = 2;
    request.startup.envc = 1;
    constexpr char arguments[] = "vfscheck\0restricted\0MODE=test";
    request.startup.string_bytes = sizeof(arguments);
    request.startup.argv[1] = 9;
    request.startup.envp[0] = 20;
    for(unsigned i = 0; i < sizeof(arguments); ++i)
        request.startup.strings[i] = arguments[i];
    request.grant_count = 2;
    request.grants[0] = {1, 1, OS1_RIGHT_WRITE, 0, 0, 0};
    request.grants[1] = {2, 2, OS1_RIGHT_WRITE, 0, 0, 0};
    const long pid = launch(executable, OS1_LAUNCH_SPAWN, request);
    close(executable);
    CHECK(pid > 0, "launch child");
    int status = -1;
    CHECK(waitpid(pid, &status) == pid && status == 0, "child status");
    static const char success[] = "[user/vfscheck] ok\n";
    write(1, success, sizeof(success) - 1);
    return 0;
}
