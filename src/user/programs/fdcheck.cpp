#include <os1/syscall.h>

#define CHECK(condition, message)                                              \
    do                                                                         \
    {                                                                          \
        if(!(condition))                                                       \
        {                                                                      \
            static const char failure[] = "[user/fdcheck] fail " message "\n"; \
            os1_write(2, failure, sizeof(failure) - 1);                        \
            return 1;                                                          \
        }                                                                      \
    } while(0)

int main(int, char**)
{
    constexpr uint32_t rights = OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE;
    os1_resource_info info{};
    CHECK(os1_resource_query(1, &info) == 0, "query stdout");
    CHECK(info.interfaces == OS1_INTERFACE_BYTE_OUTPUT && info.rights == rights, "stdout rights");
    long copy = os1_duplicate(1, rights, OS1_DESCRIPTOR_CLOEXEC, -1);
    CHECK(copy == 3, "duplicate stdout");
    CHECK(os1_close(1) == 0, "close stdout");
    CHECK(os1_write(1, nullptr, 0) == OS1_ERR_BAD_DESCRIPTOR, "closed stdout");
    static const char continued[] = "[user/fdcheck] duplicate output\n";
    CHECK(os1_write(copy, continued, sizeof(continued) - 1) == sizeof(continued) - 1,
          "retained output");
    CHECK(os1_duplicate(copy, rights, 0, 1) == 1, "replace stdout");
    CHECK(os1_duplicate(copy, OS1_RIGHT_READ, 0, -1) == OS1_ERR_ACCESS, "rights escalation");
    long weak = os1_duplicate(copy, OS1_RIGHT_INSPECT, OS1_DESCRIPTOR_CLOEXEC, -1);
    CHECK(weak == 4, "attenuated copy");
    CHECK(os1_write(weak, continued, 1) == OS1_ERR_ACCESS, "write denied");
    CHECK(os1_duplicate(weak, OS1_RIGHT_INSPECT, 0, -1) == OS1_ERR_ACCESS, "delegation denied");
    CHECK(os1_close(weak) == 0 && os1_close(weak) == OS1_ERR_BAD_DESCRIPTOR, "double close");
    CHECK(os1_status_flags(0, OS1_STATUS_NONBLOCK) == 0, "nonblocking input");
    char byte;
    CHECK(os1_read(0, &byte, 1) == OS1_ERR_WOULD_BLOCK, "would block");
    CHECK(os1_status_flags(0, 0) == 0, "restore input status");
    CHECK(os1_exec("/bin/does-not-exist") < 0, "failed exec");
    CHECK(os1_resource_query(3, &info) == 0, "failed exec preserves descriptors");
    CHECK(os1_duplicate(copy, rights, 0, 4) == 4, "exec survivor");
    // The next image verifies CLOEXEC removal and the unmarked duplicate.
    os1_exec("/disk/bin/fdexec");
    CHECK(false, "exec returned");
}
