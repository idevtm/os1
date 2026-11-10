#include <os1/file_tools.hpp>
#include <os1/system.hpp>
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
    if(argc == 2 && equal(argv[1], "system"))
    {
        os1_resource_info info{};
        os1_file_info detail{};
        CHECK(query(root(), &info) == 0 && file_info(root(), &detail) == 0, "system root info");
        CHECK(open("/", OS1_RIGHT_SYSTEM_CONTROL) == OS1_ERR_ACCESS,
              "power not recovered by lookup");
        files::Handle limited(duplicate_scoped(
            root(), info.rights & ~OS1_RIGHT_SYSTEM_CONTROL, detail.descendant_rights));
        CHECK(limited.fd >= 0, "limited system root");
        const auto* original = startup_record;
        os1_startup_record attenuated = *original;
        attenuated.root_fd = limited.fd;
        startup_record = &attenuated;
        CHECK(system_control(OS1_SYSTEM_SHUTDOWN) == OS1_ERR_ACCESS, "shutdown authority denied");
        CHECK(system_control(OS1_SYSTEM_REBOOT) == OS1_ERR_ACCESS, "reboot authority denied");
        startup_record = original;
        files::Handle script(files::open_file("/tmp/system-script", files::Mode::Replace));
        CHECK(script.fd >= 0, "system script");
        constexpr char text[] = "echo \"no inspect sync\" > \"/disk/no-inspect\"\n";
        CHECK(write(script.fd, text, sizeof(text) - 1) == sizeof(text) - 1, "system script write");
        files::Handle shell(open("/disk/bin/sh", OS1_RIGHT_READ | OS1_RIGHT_EXECUTE));
        CHECK(shell.fd >= 0, "system shell");
        os1_launch_request request{};
        request.version = OS1_STARTUP_VERSION;
        request.size = sizeof(request);
        request.startup.version = OS1_STARTUP_VERSION;
        request.startup.size = sizeof(request.startup);
        request.startup.root_fd = root();
        request.startup.cwd_fd = cwd();
        request.startup.argc = 2;
        constexpr char arguments[] = "/disk/bin/sh\0/tmp/system-script";
        request.startup.string_bytes = sizeof(arguments);
        request.startup.argv[1] = sizeof("/disk/bin/sh");
        for(unsigned i = 0; i < sizeof(arguments); ++i)
            request.startup.strings[i] = arguments[i];
        request.grant_count = 5;
        for(int fd = 0; fd < 3; ++fd)
        {
            os1_resource_info stream{};
            CHECK(query(fd, &stream) == 0, "system stream info");
            request.grants[fd] = {fd, fd, stream.rights, 0, 0, 0};
        }
        request.grants[3] = {
            root(), root(), info.rights & ~OS1_RIGHT_INSPECT, 0, detail.descendant_rights, 0};
        os1_resource_info cwd_info{};
        os1_file_info cwd_detail{};
        CHECK(query(cwd(), &cwd_info) == 0 && file_info(cwd(), &cwd_detail) == 0,
              "system cwd info");
        request.grants[4] = {cwd(), cwd(), cwd_info.rights, 0, cwd_detail.descendant_rights, 0};
        long child = launch(shell.fd, OS1_LAUNCH_SPAWN, request);
        int status = -1;
        CHECK(child > 0 && waitpid(child, &status) == child && status == 0, "no-inspect child");
        constexpr char done[] = "[user/vfscheck] no-inspect child complete\n";
        write(1, done, sizeof(done) - 1);
        char input[2];
        CHECK(read(0, input, sizeof(input)) > 0, "system check acknowledgement");
        return 0;
    }
    if(argc == 2 && equal(argv[1], "times"))
    {
        CHECK(open("/", OS1_RIGHT_LOG_READ) == OS1_ERR_ACCESS, "logs not recovered by lookup");
        // Exercise the real user helpers with the filesystem-only ceiling.
        files::Handle limited(
            open("/",
                 OS1_RIGHT_LOOKUP | OS1_RIGHT_ENUMERATE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE,
                 OS1_RIGHT_ALL &
                     ~(OS1_RIGHT_SET_TIMES | OS1_RIGHT_SYSTEM_CONTROL | OS1_RIGHT_LOG_READ)));
        CHECK(limited.fd >= 0, "time restricted root");
        os1_startup_record context = *startup_record;
        context.root_fd = limited.fd;
        startup_record = &context;
        current_cwd = limited.fd;
        files::Handle directory(open("/tmp", fs::directory_rights, fs::descendant_rights));
        CHECK(directory.fd >= 0, "time independent directory authority");
        files::Handle keep(files::open_file("/tmp/rights-note", files::Mode::Keep));
        CHECK(keep.fd >= 0, "time independent ensure authority");
        CHECK(write(keep.fd, "abc", 3) == 3, "time independent write");
        files::Handle append(files::open_file("/tmp/rights-note", files::Mode::Append));
        CHECK(append.fd >= 0 && write(append.fd, "d", 1) == 1, "time independent append");
        files::Handle replace(files::open_file("/tmp/rights-note", files::Mode::Replace));
        CHECK(replace.fd >= 0, "time independent replacement");
        CHECK(open("/tmp/rights-note", OS1_RIGHT_SET_TIMES) == OS1_ERR_ACCESS,
              "time mutation denied");
        constexpr char success[] = "[user/vfscheck] time authority ok\n";
        write(1, success, sizeof(success) - 1);
        return 0;
    }
    if(argc == 2 && equal(argv[1], "tmpfs"))
        return os1::tmpfs_check::child();
    if(argc == 2)
    {
        CHECK(equal(argv[0], "vfscheck") && equal(argv[1], "restricted"), "argv");
        CHECK(startup_record->envc == 1 &&
                  equal(startup_record->strings + startup_record->envp[0], "MODE=test"),
              "environment");
        CHECK(root() == -1 && cwd() == -1, "namespace roles");
        CHECK(open("/disk/etc/motd", OS1_RIGHT_READ) == OS1_ERR_BAD_DESCRIPTOR, "no ambient root");
        CHECK(read(0, nullptr, 0) == OS1_ERR_BAD_DESCRIPTOR, "no implicit stdin");
        static const char success[] = "[user/vfscheck] restricted child ok\n";
        write(1, success, sizeof(success) - 1);
        return 0;
    }
    CHECK(os1::tmpfs_check::run(), "tmpfs protocol");
    constexpr char tmp_ok[] = "[user/vfscheck] tmpfs native two clients compatibility unlink ok\n";
    write(1, tmp_ok, sizeof(tmp_ok) - 1);
    const long file = open("/disk/etc/motd", OS1_RIGHT_READ | OS1_RIGHT_SEEK | OS1_RIGHT_DELEGATE);
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

    const long executable = open("/disk/bin/vfscheck", OS1_RIGHT_EXECUTE);
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
