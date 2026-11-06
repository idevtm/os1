#pragma once
#include "vfs/initrd.hpp"
struct BlockDevice;
// Bootstrap-only mount. Missing devices/partitions return NOT_FOUND, without writes.
long ext2_mount(NativeObject* root, BlockDevice& device);
