#pragma once
#include <os1/observe.h>
#include <stddef.h>
namespace usb::diagnostics
{
constexpr size_t capacity = 256;  // Bounded boot topology and mount outcomes.
// BSP boot only, before the first user process. Reads never touch hardware.
void append(const Os1ObserveUsbRecord&);
size_t count();
unsigned dropped();
#ifdef OS1_HOST_TEST
void reset_for_test();
#endif
const Os1ObserveUsbRecord* at(size_t index);
}  // namespace usb::diagnostics
