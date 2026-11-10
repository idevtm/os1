#pragma once
#include <stdint.h>
#define OS1_SYSTEM_INTERFACE_ID UINT64_C(0x4f5331535953544d)
#define OS1_SYSTEM_INTERFACE_VERSION 1u
// Requires SYSTEM_CONTROL on the true root. Trace controls use a copied u32.
enum os1_system_member
{
    OS1_SYSTEM_SYNC = 1,
    OS1_SYSTEM_SHUTDOWN = 2,
    OS1_SYSTEM_REBOOT = 3,
    OS1_SYSTEM_EHCI_TRACE_START = 4,
    OS1_SYSTEM_EHCI_TRACE_STOP = 5,
};
