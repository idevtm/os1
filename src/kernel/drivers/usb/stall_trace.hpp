#pragma once
#include <os1/observe.h>

namespace usb::stall_trace
{
long start(unsigned input);
long stop(unsigned ticket);
bool snapshot(Os1ObserveEhciStallTrace&);
bool enabled();
uint64_t now();
void sample(
    unsigned kind, unsigned pci = 0, unsigned input = 0, uint64_t began = 0, unsigned token = 0);
}  // namespace usb::stall_trace
