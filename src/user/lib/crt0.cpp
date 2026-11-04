#include <os1/vfs.hpp>
int main(int argc, char** argv);
namespace os1::user
{
const os1_startup_record* startup_record = nullptr;
}
extern "C" [[noreturn]] void _start(const os1_startup_record* startup)
{
    if(!startup || startup->version != OS1_STARTUP_VERSION || startup->size != sizeof(*startup) ||
       startup->argc > OS1_ARGUMENT_LIMIT)
        os1::user::exit(127);
    os1::user::startup_record = startup;
    char* argv[OS1_ARGUMENT_LIMIT + 1]{};
    for(unsigned i = 0; i < startup->argc; ++i)
        argv[i] = const_cast<char*>(startup->strings + startup->argv[i]);
    os1::user::exit(main(startup->argc, argv));
    __builtin_unreachable();
}
