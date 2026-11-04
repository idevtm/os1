#include <os1/syscall.hpp>

int main(int, char**)
{
    os1::user::write(1, "[user/fault] boom\n", 18);
    *(volatile unsigned long*)0x1234ull = 1;
    return 0;
}
