#include <os1/file_tools.hpp>

namespace
{
using namespace os1::user;
constexpr char input_path[] = "/tmp/textcheck-input";
constexpr char output_path[] = "/tmp/textcheck-output";
constexpr char script_path[] = "/tmp/textcheck-script";
constexpr char input_bytes[] = "alpha\nbeta\n";

bool content(const char* path, const char* expected)
{
    files::Handle file(files::open_file(path, files::Mode::Existing));
    if(file.fd < 0)
        return false;
    char bytes[128];
    size_t used = 0;
    for(;;)
    {
        long n = read(file.fd, bytes + used, sizeof(bytes) - used);
        if(n < 0 || used + n == sizeof(bytes))
            return false;
        if(!n)
            break;
        used += n;
    }
    if(used != files::length(expected))
        return false;
    for(size_t i = 0; i < used; ++i)
        if(bytes[i] != expected[i])
            return false;
    return true;
}

bool check(const char* command, const char* expected, bool same = false)
{
    files::Handle input(files::open_file(input_path, files::Mode::Replace));
    files::Handle script(files::open_file(script_path, files::Mode::Replace));
    if(input.fd < 0 || script.fd < 0 ||
       !files::put(input.fd, input_bytes, sizeof(input_bytes) - 1) || seek(input.fd, 0) < 0 ||
       !files::put(script.fd, command, files::length(command)))
        return false;
    os1_resource_info output{};
    if(query(1, &output) < 0)
        return false;
    files::Handle saved(duplicate(1, output.rights));
    constexpr auto readable =
        OS1_RIGHT_READ | OS1_RIGHT_SEEK | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE;
    if(saved.fd < 0 || duplicate(input.fd, readable, 0, 1) != 1)
        return false;
    const char* arguments[] = {"sh", script_path};
    long pid = run_program("/disk/bin/sh", 2, arguments);
    // The child owns its grants. Restore this program's output before checking status.
    bool restored = duplicate(saved.fd, output.rights, output.descriptor_flags, 1) == 1;
    if(pid < 0)
        return false;
    int status = -1;
    if(waitpid(pid, &status) != pid || !restored || status != (same ? 1 : 0))
        return false;
    return content(input_path, input_bytes) && content(same ? input_path : output_path, expected);
}
}  // namespace

int main(int, char**)
{
    const struct
    {
        const char* command;
        const char* expected;
    } cases[] = {
        {"head process.streams.output lines:1 > \"/tmp/textcheck-output\"\n", "alpha\n"},
        {"tail process.streams.output lines:1 > \"/tmp/textcheck-output\"\n", "beta\n"},
        {"grep \"beta\" process.streams.output > \"/tmp/textcheck-output\"\n", "beta\n"},
        {"wc process.streams.output > \"/tmp/textcheck-output\"\n", "lines:2 words:2 bytes:11\n"},
        {"cat process.streams.output > \"/tmp/textcheck-output\"\n", input_bytes},
    };
    for(const auto& test : cases)
        if(!check(test.command, test.expected))
        {
            files::message("[user/textcheck] fail readable stdout source\n");
            return 1;
        }
    if(!check("wc process.streams.output > \"/tmp/textcheck-input\"\n", input_bytes, true))
    {
        files::message("[user/textcheck] fail same-object rejection\n");
        return 1;
    }
    constexpr char success[] = "[user/textcheck] readable stdout input retained ok\n";
    return files::put(1, success, sizeof(success) - 1) ? 0 : 1;
}
