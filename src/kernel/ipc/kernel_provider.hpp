#pragma once
#include <os1/ipc.h>

#include "resource/descriptor.hpp"

// Fixed trusted providers. Handlers receive retained resources and copied records,
// never caller pointers. This first adapter accepts only bounded byte envelopes.
struct KernelProvider
{
    uint64_t interface_id;
    uint32_t interface_version, object_kind;
    void (*invoke)(OpenInstance&, uint32_t rights, const os1_ipc_message&, os1_ipc_result&);
};
const KernelProvider* kernel_provider(uint64_t interface_id, uint32_t version);
