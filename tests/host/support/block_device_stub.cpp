#include "storage/block_device.hpp"
const BlockDevice* virtio_blk_block_device()
{
    return nullptr;
}

BlockDevice* usb_storage_block_device(size_t)
{
    return nullptr;
}
