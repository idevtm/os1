#include "drivers/usb/diagnostics.hpp"

#include <os1/input.h>

#include "sync/smp.hpp"
namespace usb::diagnostics
{
namespace
{
Os1ObserveUsbRecord records[capacity]{};
size_t used{};
unsigned lost{};
bool sealed{};
Spinlock failure_lock{"usb-input-failures"};
Os1ObserveUsbInputFailure failures[OS1_INPUT_DEVICE_LIMIT]{};
size_t failure_used{};
}  // namespace
#ifdef OS1_HOST_TEST
void reset_for_test()
{
    used = lost = failure_used = 0;
    sealed = false;
}
#endif
void input_failure(const Os1ObserveUsbInputFailure& record)
{
    if(!record.input_device || record.input_device > OS1_INPUT_DEVICE_LIMIT)
        return;
    IrqSpinGuard guard(failure_lock);
    for(size_t i = 0; i < failure_used; ++i)
        if(failures[i].input_device == record.input_device)
            return;
    // At most one immutable record for each admitted native input identity.
    if(failure_used == OS1_INPUT_DEVICE_LIMIT)
        return;
    auto& out = failures[failure_used++];
    out = record;
    out.sequence = failure_used;
}
size_t input_failure_count()
{
    IrqSpinGuard guard(failure_lock);
    return failure_used;
}
bool input_failure_at(size_t index, Os1ObserveUsbInputFailure& record)
{
    IrqSpinGuard guard(failure_lock);
    if(index >= failure_used)
        return false;
    record = failures[index];
    return true;
}
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
