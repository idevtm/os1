#include <os1/syscall.h>
int main(int, char**)
{
    os1_resource_info info{};
    if(os1_resource_query(3, &info) != OS1_ERR_BAD_DESCRIPTOR ||
       os1_resource_query(4, &info) != 0 || info.descriptor_flags != 0 ||
       os1_write(0, nullptr, 0) != OS1_ERR_ACCESS || os1_resource_query(1, &info) != 0)
    {
        static const char failure[] = "[user/fdcheck] fail exec cleanup\n";
        os1_write(2, failure, sizeof(failure) - 1);
        return 1;
    }
    static const char success[] = "[user/fdcheck] ok\n";
    os1_write(4, success, sizeof(success) - 1);
    return 0;  // exit cleanup closes the surviving duplicate
}
