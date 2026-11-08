#pragma once
#include "vfs/initrd.hpp"
struct BlockDevice;
// Bootstrap-only mount. Missing devices/partitions return NOT_FOUND, without writes.
long ext2_mount(NativeObject* root, BlockDevice& device);

// System transitions seal the mounted volume under its admission guard.
long ext2_system_sync(bool freeze);
void ext2_system_thaw();
