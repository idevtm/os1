#pragma once
#include <os1/hardware.h>
#include <os1/input.h>

#include "ipc/kernel_provider.hpp"
namespace native_input
{
uint64_t attach(unsigned kind, const char* name, unsigned catalog_id = 0);
void remove(uint64_t device);
void keyboard(uint64_t device, const uint8_t keys[32]);
void boot_keyboard(uint64_t device, const uint8_t* report, unsigned bytes);
void pointer(uint64_t device, unsigned buttons, int dx, int dy, int wheel = 0);
void boot_pointer(uint64_t device, const uint8_t* report, unsigned bytes);
bool inspect(uint64_t device, os1_input_device& info);
long query(Os1HardwareQuery& request);
const KernelProvider* provider(uint64_t id, unsigned version);
#ifdef OS1_HOST_TEST
void reset_for_test();
#endif
}  // namespace native_input
