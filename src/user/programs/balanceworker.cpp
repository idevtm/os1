#include <os1/syscall.hpp>

int main(int, char**)
{
    for(unsigned i = 0; i < 16384; ++i)
    {
        os1::user::yield();
    }

    return 0;
}
