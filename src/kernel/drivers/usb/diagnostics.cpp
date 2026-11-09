#include "drivers/usb/diagnostics.hpp"
namespace usb::diagnostics
{
namespace
{
Os1ObserveUsbRecord records[capacity]{};
size_t used{};
unsigned lost{};
}  // namespace
#ifdef OS1_HOST_TEST
void reset_for_test()
{
    used = lost = 0;
}
#endif
void append(const Os1ObserveUsbRecord& record)
{
    if(used < capacity)
        records[used++] = record;
    else
        ++lost;
}
unsigned dropped()
{
    return lost;
}
size_t count()
{
    return used;
}
const Os1ObserveUsbRecord* at(size_t index)
{
    return index < used ? &records[index] : nullptr;
}
}  // namespace usb::diagnostics
