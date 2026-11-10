#pragma once
#include <os1/hardware.h>
void hardware_freeze_bindings();
long hardware_query(Os1HardwareQuery& query);

void hardware_reset_bindings();
void hardware_record_probe_failure(unsigned pci_index, const char* driver);

struct BootInfo;
void hardware_capture_platform(const BootInfo&);
