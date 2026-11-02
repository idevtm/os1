// Platform-level device probing policy. This is deliberately thin until a real
// bus/driver registry is justified by a second PCI device family.
#pragma once

class VirtualMemory;

// Probe supported devices from the normalized PCI table and publish facades.
// `enable_xhci` = false leaves xHCI controllers unbound (boot option os1.noxhci).
bool platform_probe_devices(VirtualMemory& kernel_vm, bool enable_xhci);
