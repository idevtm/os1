#include "drivers/usb/diagnostics.hpp"
namespace usb::diagnostics
{
namespace
{
Os1ObserveUsbRecord records[capacity]{};
size_t used{};
}  // namespace
void append(const Os1ObserveUsbRecord& record)
{
    if(used < capacity)
        records[used++] = record;
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
