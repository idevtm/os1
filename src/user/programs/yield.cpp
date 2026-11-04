#include <os1/syscall.hpp>

int main(int, char**)
{
    static const char* messages[] = {
        "[user/yield] tick 0\n",
        "[user/yield] tick 1\n",
        "[user/yield] tick 2\n",
    };

    for(unsigned i = 0; i < sizeof(messages) / sizeof(messages[0]); ++i)
    {
        os1::user::write(1, messages[i], 20);
        os1::user::yield();
    }

    return 0;
}
