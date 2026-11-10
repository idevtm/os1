#include "drivers/usb/diagnostics.hpp"
namespace usb::diagnostics
{
namespace
{
Os1ObserveUsbRecord records[capacity]{};
size_t used{};
unsigned lost{};
bool sealed{};
}  // namespace
#ifdef OS1_HOST_TEST
void reset_for_test()
{
    used = lost = 0;
    sealed = false;
}
#endif
void freeze()
{
    sealed = true;
}
void append(const Os1ObserveUsbRecord& record)
{
    if(sealed)
        return;
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
