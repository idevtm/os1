#include "drivers/module/loader.hpp"

#include "arch/x86_64/apic/ipi.hpp"
#include "core/kernel_state.hpp"
#include "crypto/sha256.hpp"
#include "debug/debug.hpp"
#include "driver/image.hpp"
#include "driver_trust.hpp"
#include "drivers/bus/driver_registry.hpp"
#include "drivers/bus/pci_bus.hpp"
#include "drivers/module/virtio_host.hpp"
#include "fs/initrd.hpp"
#include "mm/kmem.hpp"
#include "mm/virtual_memory.hpp"
#include "platform/platform.hpp"
#include "util/string.h"
#include "vfs/filesystem.hpp"
#include "vfs/initrd.hpp"
#include "vfs/namespace.hpp"

extern PageFrameContainer page_frames;
namespace
{
const os1_driver_exports* module = nullptr;
bool attempted = false;
const PciMatch matches[] = {{.vendor_id = kDriverVendor,
                             .device_id = kDriverDevice,
                             .match_flags = uint8_t(kPciMatchVendorId | kPciMatchDeviceId)}};
bool probe(VirtualMemory&, PageFrameContainer&, const PciDevice&, size_t index, DeviceId)
{
    if(!module->probe(uint32_t(index)))
        return false;
    if(module->check())
        return true;
    (void)module->stop();
    return false;
}
bool quiesce(DeviceId)
{
    return module && module->stop();
}
void remove(DeviceId) {}
const PciDriver pci_driver = {.name = "virtio-net",
                              .matches = matches,
                              .match_count = 1,
                              .probe = probe,
                              .remove = remove,
                              .quiesce = quiesce};
long snapshot(uint8_t*& bytes, uint64_t& size)
{
    NamespaceGuard guard;
    if(!guard.held)
        return OS1_ERR_WOULD_BLOCK;
    NativeObject* object = nullptr;
    long status =
        vfs_resolve(initrd_namespace_root(), kDriverPath + 1, sizeof(kDriverPath) - 2, object);
    if(status < 0)
        return status;
    auto* file = static_cast<VfsNode*>(object);
    if(file->kind != OS1_OBJECT_FILE || !file->byte_size ||
       file->byte_size > OS1_DRIVER_FILE_LIMIT || !file->operations || !file->operations->read)
    {
        object_release(object);
        return OS1_ERR_BAD_IMAGE;
    }
    size = file->byte_size;
    bytes = static_cast<uint8_t*>(kmalloc(size));
    if(!bytes)
    {
        object_release(object);
        return OS1_ERR_NO_MEMORY;
    }
    status = file->operations->read(file, 0, bytes, size);
    if(status != static_cast<long>(size))
    {
        kfree(bytes);
        bytes = nullptr;
        status = status < 0 ? status : OS1_ERR_IO;
    }
    else
        status = 0;
    object_release(object);
    return status;
}
bool map_image(VirtualMemory& vm, const driver::Image& image, const uint8_t* bytes)
{
    for(unsigned i = 0; i < image.count; ++i)
        for(uint64_t page = 0; page < (image.segments[i].memsz + 4095) / 4096; ++page)
        {
            uint64_t physical, flags;
            if(vm.translate(image.segments[i].vaddr + page * 4096, physical, flags))
                return false;
        }
    unsigned mapped = 0;
    for(unsigned i = 0; i < image.count; ++i)
    {
        auto& p = image.segments[i];
        uint64_t pages = (p.memsz + 4095) / 4096;
        bool allocated = true;
        uint64_t page = 0;
        for(; page < pages; ++page)
            if(!vm.allocate_and_map(p.vaddr + page * 4096,
                                    1,
                                    PageFlags::Present | PageFlags::Write | PageFlags::NoExecute))
            {
                allocated = false;
                break;
            }
        if(!allocated)
        {
            for(uint64_t j = 0; j < page; ++j)
                (void)vm.free(p.vaddr + j * 4096, 1);
            break;
        }
        ++mapped;
        memcpy(reinterpret_cast<void*>(p.vaddr), bytes + p.offset, p.filesz);
        PageFlags flags = PageFlags::Present;
        if(p.flags & 2)
            flags |= PageFlags::Write;
        if(!(p.flags & 1))
            flags |= PageFlags::NoExecute;
        if(!vm.protect(p.vaddr, pages, flags))
            break;
        if(i + 1 == image.count)
        {
            ipi_retire_tlb();
            return true;
        }
    }
    for(unsigned i = 0; i < mapped; ++i)
        (void)vm.free(image.segments[i].vaddr, (image.segments[i].memsz + 4095) / 4096);
    ipi_retire_tlb();
    return false;
}
}  // namespace
void load_boot_drivers()
{
    if(attempted)
        return;
    attempted = true;
    bool match = false;
    for(size_t i = 0; i < platform_pci_device_count(); ++i)
        if(pci_driver_matches_device(pci_driver, platform_pci_devices()[i]))
            match = true;
    if(!match)
    {
        debug("driver: virtio-net not needed")();
        return;
    }
    uint8_t* bytes = nullptr;
    uint64_t size = 0;
    long status = snapshot(bytes, size);
    if(status < 0)
    {
        debug("driver: virtio-net package unavailable status=-")(uint64_t(-status))();
        return;
    }
    uint8_t digest[32];
    crypto::sha256(bytes, size, digest);
    if(memcmp(digest, kDriverDigest, sizeof(digest)))
    {
        debug("driver: virtio-net digest rejected")();
        kfree(bytes);
        return;
    }
    driver::Image image;
    if(!driver::validate_image(bytes, size, image))
    {
        debug("driver: virtio-net ELF rejected")();
        kfree(bytes);
        return;
    }
    VirtualMemory vm(page_frames, g_kernel_root_cr3);
    if(!map_image(vm, image, bytes))
    {
        debug("driver: virtio-net mapping failed")();
        kfree(bytes);
        return;
    }
    kfree(bytes);
    auto entry = reinterpret_cast<os1_driver_entry>(image.entry);
    module = entry(&driver_host_api());
    if(!driver::contains(image, reinterpret_cast<uint64_t>(module), sizeof(*module), 4) ||
       module->version != OS1_DRIVER_ABI || module->bytes != sizeof(*module) ||
       !driver::contains(image, reinterpret_cast<uint64_t>(module->probe), 1, 5) ||
       !driver::contains(image, reinterpret_cast<uint64_t>(module->check), 1, 5) ||
       !driver::contains(image, reinterpret_cast<uint64_t>(module->stop), 1, 5))
    {
        module = nullptr;
        debug("driver: virtio-net exports rejected")();
        return;
    }
    if(!driver_registry_add_pci_driver(pci_driver))
    {
        debug("driver: virtio-net registry full")();
        return;
    }
    debug("driver: loaded virtio-net from ")(kDriverPath)(" bytes=")(size)();
    (void)pci_bus_probe_driver(vm, page_frames, pci_driver);
}
