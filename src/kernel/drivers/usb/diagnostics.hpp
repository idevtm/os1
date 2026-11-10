#pragma once
#include <os1/observe.h>
#include <stddef.h>
namespace usb::diagnostics
{
constexpr size_t capacity = 256;  // Bounded boot topology and mount outcomes.
// First cause per input identity. Publication is bounded and does not perform I/O.
void input_failure(const Os1ObserveUsbInputFailure&);
size_t input_failure_count();
bool input_failure_at(size_t index, Os1ObserveUsbInputFailure&);
// BSP boot only, before the first user process. Reads never touch hardware.
// Seal before publishing the runtime worker or userspace. Later appends are ignored.
void freeze();
void append(const Os1ObserveUsbRecord&);
size_t count();
unsigned dropped();
#ifdef OS1_HOST_TEST
void reset_for_test();
#endif
const Os1ObserveUsbRecord* at(size_t index);
}  // namespace usb::diagnostics
