#include <os1/syscall.hpp>

int main(int, char**)
{
    static const char kShellPath[] = "/bin/sh";
    static const char kExecFailed[] = "[user/init] exec /bin/sh failed\n";

    const long result = os1::user::exec(kShellPath);
    os1::user::write(1, kExecFailed, sizeof(kExecFailed) - 1);
    return (result < 0) ? 1 : (int)result;
}
