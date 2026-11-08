#pragma once
#include <stdint.h>
#define OS1_SYSTEM_INTERFACE_ID UINT64_C(0x4f5331535953544d)
#define OS1_SYSTEM_INTERFACE_VERSION 1u
// Empty copied requests and replies. Requires SYSTEM_CONTROL on the true root.
enum os1_system_member
{
    OS1_SYSTEM_SYNC = 1,
    OS1_SYSTEM_SHUTDOWN = 2,
    OS1_SYSTEM_REBOOT = 3,
};
