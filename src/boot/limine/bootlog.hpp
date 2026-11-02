#pragma once

#include <stddef.h>
#include <stdint.h>

namespace limine_shim
{
// The shim keeps a copy of its serial output and hands it to the kernel as the
// "shimlog" boot module; the kernel copies it into the real-hardware boot log
// once that log is attached, so shim progress is visible without a serial port.
void shim_log_write_char(char value);
// Physical address and capacity of the (NUL-terminated) shim log buffer.
bool shim_log_physical(uint64_t& physical_address, uint64_t& capacity);
}  // namespace limine_shim
