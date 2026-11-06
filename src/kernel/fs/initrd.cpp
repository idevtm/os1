// CPIO newc initrd parser. It exposes a tiny archive traversal API until the
// future VFS can provide path lookup and filesystem-backed exec.
#include "fs/initrd.hpp"

#include "debug/debug.hpp"
#include "drivers/block/virtio_blk.hpp"
#include "drivers/usb/diagnostics.hpp"
#include "drivers/usb/storage_devices.hpp"
#include "handoff/memory_layout.h"
#include "storage/block_device.hpp"
#include "util/fixed_string.hpp"
#include "vfs/ext2.hpp"
#include "vfs/initrd.hpp"
#include "vfs/tmpfs.hpp"

namespace
{
// The boot path still publishes the initrd as a boot module rather than a
// filesystem-backed device, so the parser holds only a borrowed BootInfo view.
const BootInfo* g_boot_info = nullptr;

struct InitrdLookupContext
{
    const char* path;
    const uint8_t* data;
    uint64_t size;
};

bool match_initrd_file(const char* archive_name,
                       const uint8_t* file_data,
                       uint64_t file_size,
                       void* context)
{
    auto* lookup = static_cast<InitrdLookupContext*>(context);
    if((nullptr == lookup) || !cpio_newc_paths_equal(archive_name, lookup->path))
    {
        return true;
    }

    lookup->data = file_data;
    lookup->size = file_size;
    return false;
}
}  // namespace

void bind_initrd_boot_info(const BootInfo* boot_info)
{
    g_boot_info = boot_info;
}

bool for_each_initrd_file(InitrdFileVisitor visitor, void* context)
{
    if((nullptr == visitor) || (nullptr == g_boot_info) || (0 == g_boot_info->module_count))
    {
        return false;
    }

    const BootModuleInfo& module = g_boot_info->modules[0];
    const uint8_t* cursor = kernel_physical_pointer<const uint8_t>(module.physical_start);
    return for_each_cpio_newc_file(cursor, module.length, visitor, context);
}

bool find_initrd_file(const char* path, const uint8_t*& data, uint64_t& size)
{
    InitrdLookupContext lookup{path, nullptr, 0};
    (void)for_each_initrd_file(match_initrd_file, &lookup);
    data = lookup.data;
    size = lookup.size;
    return (nullptr != data);
}

void copy_initrd_path(char* destination, size_t destination_size, const char* archive_name)
{
    if((nullptr == destination) || (0 == destination_size))
    {
        return;
    }

    copy_cpio_newc_path(destination, destination_size, archive_name);
}

namespace
{
NativeObject* g_initrd_root = nullptr;
}
long initialize_initrd_namespace()
{
    if(g_initrd_root)
        return OS1_ERR_INVALID;
    if(!g_boot_info || !g_boot_info->module_count)
        return OS1_ERR_BAD_IMAGE;
    const auto& module = g_boot_info->modules[0];
    NativeObject* root = nullptr;
    long status = vfs_import_initrd(
        kernel_physical_pointer<const uint8_t>(module.physical_start), module.length, root);
    if(status < 0)
        return status;
    status = tmpfs_mount(root);
    if(status == 0)
    {
        for(size_t candidate = 0;; ++candidate)
        {
            auto* device = candidate == 0 ? const_cast<BlockDevice*>(virtio_blk_block_device())
                                          : usb_storage_block_device(candidate - 1);
            if(!device)
            {
                if(candidate == 0)
                    continue;
                break;
            }
            long mounted = ext2_mount(root, *device);
            if(candidate)
            {
                Os1ObserveUsbRecord record{};
                record.kind = OS1_OBSERVE_USB_MOUNT;
                record.pci_index = OS1_OBSERVE_INDEX_NONE;
                record.status = static_cast<uint32_t>(mounted);
                record.detail = static_cast<uint32_t>(candidate - 1);
                record.sectors = device->sector_count;
                copy_fixed_string(record.stage,
                                  sizeof(record.stage),
                                  mounted == 0 ? "mounted" : "mount-rejected");
                usb::diagnostics::append(record);
            }
            if(mounted == 0)
            {
                debug("ext2 mounted /disk")();
                break;
            }
            if(mounted != OS1_ERR_NOT_FOUND)
                debug("ext2 mount rejected ")(mounted)();
        }
    }
    if(status < 0)
        object_release(root);
    else
        g_initrd_root = root;
    return status;
}
NativeObject* initrd_namespace_root()
{
    return g_initrd_root;
}
#if defined(OS1_HOST_TEST)
void reset_initrd_namespace_for_test()
{
    object_release(g_initrd_root);
    g_initrd_root = nullptr;
}
#endif
