#include <os1/syscall.hpp>

#ifndef DEPLOY_VERSION
#error DEPLOY_VERSION must identify this test build
#endif

int main(int, char**)
{
    constexpr char message[] = "deployed program version " DEPLOY_VERSION "\n";
    return os1::user::write(1, message, sizeof(message) - 1) == sizeof(message) - 1 ? 0 : 1;
}
