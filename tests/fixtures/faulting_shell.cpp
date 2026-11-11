#include <os1/file_tools.hpp>
#include <os1/ipc.hpp>

int main(int, char**)
{
    os1::user::files::message("faulting shell start ns=");
    os1::user::files::number(2, os1::user::ipc_clock());
    os1::user::files::message("\n");
    asm volatile("ud2");
    return 0;
}
