#include <os1/file_tools.hpp>
#include <os1/ipc.hpp>

extern "C" int os1_typed_entry(const os1_startup_record*)
{
    os1::user::files::message("faulting shell start ns=");
    os1::user::files::number(2, os1::user::ipc_clock());
    os1::user::files::message("\n");
    asm volatile("ud2");
    return 0;
}
