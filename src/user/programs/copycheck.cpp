#include <os1/observe.h>
#include <stdint.h>

#include <os1/syscall.hpp>

static const uint8_t kReadOnlyObserveBuffer[sizeof(struct Os1ObserveHeader) +
                                            sizeof(struct Os1ObserveSystemRecord)] = {0};

#define OS1_COPYCHECK_FAIL(message)                          \
    do                                                       \
    {                                                        \
        static const char kFailure[] = message;              \
        os1::user::write(1, kFailure, sizeof(kFailure) - 1); \
        os1::user::exit(1);                                  \
    } while(0)

int main(void)
{
    uint8_t observe_buffer[sizeof(struct Os1ObserveHeader) +
                           sizeof(struct Os1ObserveSystemRecord)] = {0};

    const long observe_ok =
        os1::user::observe(OS1_OBSERVE_SYSTEM, observe_buffer, sizeof(observe_buffer));
    if(observe_ok < (long)sizeof(struct Os1ObserveHeader))
    {
        OS1_COPYCHECK_FAIL("[user/copycheck] fail positive observe\n");
    }

    if(-1 != os1::user::observe(OS1_OBSERVE_SYSTEM, (void*)0x1000, sizeof(observe_buffer)))
    {
        OS1_COPYCHECK_FAIL("[user/copycheck] fail kernel observe pointer\n");
    }

    if(-1 != os1::user::observe(
                 OS1_OBSERVE_SYSTEM, (void*)kReadOnlyObserveBuffer, sizeof(kReadOnlyObserveBuffer)))
    {
        OS1_COPYCHECK_FAIL("[user/copycheck] fail readonly observe pointer\n");
    }

    if(OS1_ERR_FAULT != os1::user::write(1, (const void*)0x1000, 1))
    {
        OS1_COPYCHECK_FAIL("[user/copycheck] fail kernel write buffer\n");
    }

    if(-1 != os1::user::spawn((const char*)0x1000))
    {
        OS1_COPYCHECK_FAIL("[user/copycheck] fail kernel spawn path\n");
    }

    static const char kSuccess[] = "[user/copycheck] ok\n";
    os1::user::write(1, kSuccess, sizeof(kSuccess) - 1);
    return 0;
}
