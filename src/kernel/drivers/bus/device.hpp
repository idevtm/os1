// Bound device records published by the minimal driver core.
#pragma once

#include <stddef.h>

#include "platform/types.hpp"

enum class BindingAdmission
{
    Published,
    Existing,
    NoMemory
};
BindingAdmission device_binding_begin_probe(DeviceId id,
                                            uint16_t pci_index,
                                            const char* driver_name);
bool device_binding_publish(DeviceId id,
                            uint16_t pci_index,
                            const char* driver_name,
                            void* driver_state);
bool device_binding_finish_probe(DeviceId id, bool success);
bool device_binding_begin_remove(DeviceId id);
bool device_binding_finish_remove(DeviceId id, bool retired);
bool device_binding_set_state(DeviceId id, DeviceState state);
void device_binding_remove(DeviceId id);
DeviceBinding* device_binding_find(DeviceId id);
size_t device_binding_count();
const DeviceBinding* device_bindings();
