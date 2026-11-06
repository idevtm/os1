#pragma once
#include <os1/ipc.h>

#include "resource/descriptor.hpp"

// Fixed trusted providers. Handlers receive retained resources and copied records,
// never caller pointers. This first adapter accepts only bounded byte envelopes.
struct KernelProvider
{
    uint64_t interface_id;
    uint32_t interface_version, object_kind;
    uint32_t allowed_rights;
    bool (*accepts)(const NativeObject*);
    void (*invoke)(OpenInstance&,
                   uint32_t rights,
                   uint32_t ceiling,
                   const os1_ipc_message&,
                   os1_ipc_result&,
                   DescriptorEntry&);
};
const KernelProvider* kernel_provider(uint64_t interface_id, uint32_t version);
