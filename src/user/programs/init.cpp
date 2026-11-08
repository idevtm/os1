#include <os1/syscall.hpp>

int main(int, char**)
{
    for(;;)
    {
        long child = os1::user::spawn("/bin/sh");
        int status = 0;
        if(child >= 0 && os1::user::waitpid(child, &status) == child)
            continue;
        static const char error[] = "init: shell restart failed; retrying\n";
        os1::user::write(2, error, sizeof(error) - 1);
        for(unsigned i = 0; i < 10000; ++i)
            os1::user::yield();
    }
}
