#include <os1/file_tools.hpp>

int main(int, char**)
{
    bool first = true;
    for(;;)
    {
        const char* arguments[] = {"sh", "--boot"};
        // The bootstrap shell takes argv, not a typed class constructor.
        // Avoid repeated ELF metadata reads before the first interactive prompt.
        const os1_constructor_args constructor{};
        long child = first ? os1::user::run_program("/disk/bin/sh",
                                                    2,
                                                    arguments,
                                                    false,
                                                    0,
                                                    nullptr,
                                                    nullptr,
                                                    nullptr,
                                                    -1,
                                                    &constructor)
                           : os1::user::spawn("/disk/bin/sh");
        first = false;
        int status = 0;
        long result = child < 0 ? child : os1::user::waitpid(child, &status);
        if(child >= 0 && result == child)
            continue;
        static const char error[] =
            "init: cannot start /disk/bin/sh; starting rescue\n"
            "init: deploy programs to ext2 with os1.sh, then boot again\n";
        os1::user::write(2, error, sizeof(error) - 1);
        os1::user::files::message("init: launch/wait status=-");
        os1::user::files::number(2, uint64_t(0) - uint64_t(result));
        os1::user::files::message("\n");
        child = os1::user::spawn("/bin/rescue");
        if(child >= 0 && os1::user::waitpid(child, &status) == child)
            continue;
        static const char failed[] = "init: rescue unavailable; retrying\n";
        os1::user::write(2, failed, sizeof(failed) - 1);
        for(unsigned i = 0; i < 10000; ++i)
            os1::user::yield();
    }
}
