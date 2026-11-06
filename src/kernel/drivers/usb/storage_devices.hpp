#pragma once
#include <stddef.h>
struct BlockDevice;
// Boot-discovered USB block devices, independent of their host controller.
BlockDevice* usb_storage_block_device(size_t index);
