#include "drivers/module/loader.hpp"

#include "arch/x86_64/apic/ipi.hpp"
#include "core/kernel_state.hpp"
#include "crypto/sha256.hpp"
#include "debug/debug.hpp"
#include "display_modules.hpp"
#include "driver/image.hpp"
#include "driver_trust.hpp"
#include "drivers/bus/driver_registry.hpp"
#include "drivers/bus/pci_bus.hpp"
#include "drivers/bus/resource.hpp"
#include "drivers/display/service.hpp"
#include "drivers/module/virtio_host.hpp"
#include "drivers/timer/tsc_clock.hpp"
#include "fs/initrd.hpp"
#include "handoff/memory_layout.h"
#include "mm/boot_mapping.hpp"
#include "mm/kmem.hpp"
#include "mm/virtual_memory.hpp"
#include "platform/pci_config.hpp"
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
long snapshot(const char* path, size_t path_bytes, uint8_t*& bytes, uint64_t& size)
{
    NamespaceGuard guard;
    if(!guard.held)
        return OS1_ERR_WOULD_BLOCK;
    NativeObject* object = nullptr;
    long status = vfs_resolve(initrd_namespace_root(), path + 1, path_bytes - 1, object);
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
bool load_image(VirtualMemory& vm,
                const char* path,
                size_t path_bytes,
                const uint8_t* expected,
                uint64_t base,
                driver::Image& image,
                uint64_t& size)
{
    uint8_t* bytes = nullptr;
    long status = snapshot(path, path_bytes, bytes, size);
    if(status < 0)
    {
        debug("driver: ")(path)(" package unavailable status=-")(uint64_t(-status))();
        return false;
    }
    uint8_t digest[32];
    crypto::sha256(bytes, size, digest);
    if(memcmp(digest, expected, sizeof(digest)))
    {
        debug("driver: ")(path)(" digest rejected")();
        kfree(bytes);
        return false;
    }
    if(!driver::validate_image(bytes, size, image, base))
    {
        debug("driver: ")(path)(" ELF rejected")();
        kfree(bytes);
        return false;
    }
    if(!map_image(vm, image, bytes))
    {
        debug("driver: ")(path)(" mapping failed")();
        kfree(bytes);
        return false;
    }
    kfree(bytes);
    return true;
}
// Load each display driver package that this machine needs. The generated table names the
// packages. A package with a PCI identity needs that display device. A package without one
// drives the boot framebuffer.
void load_display_modules()
{
    for(const auto& package : kDisplayModules)
    {
        bool needed = !package.vendor && native_display::compatible();
        for(uint16_t i = 0; package.vendor && i < platform_pci_device_count(); ++i)
        {
            const auto& pci = platform_pci_devices()[i];
            needed = needed || (pci.vendor_id == package.vendor &&
                                pci.device_id == package.device && pci.class_code == 3);
        }
        if(!needed)
            continue;
        VirtualMemory vm(page_frames, g_kernel_root_cr3);
        driver::Image image;
        uint64_t size = 0;
        size_t path_bytes = 0;
        while(package.path[path_bytes])
            ++path_bytes;
        if(!load_image(vm, package.path, path_bytes, package.digest, package.base, image, size))
            continue;
        const auto inside = [&](const void* function, bool optional) {
            return (optional && !function) ||
                   driver::contains(image, reinterpret_cast<uint64_t>(function), 1, 5);
        };
        auto* exports = reinterpret_cast<os1_display_entry>(image.entry)();
        if(!driver::contains(image, reinterpret_cast<uint64_t>(exports), sizeof(*exports), 4) ||
           exports->version != OS1_DISPLAY_MODULE_ABI || exports->bytes != sizeof(*exports) ||
           !inside(reinterpret_cast<const void*>(exports->probe), false) ||
           !inside(reinterpret_cast<const void*>(exports->text), false) ||
           !inside(reinterpret_cast<const void*>(exports->mode), false) ||
           !inside(reinterpret_cast<const void*>(exports->set_mode), false) ||
           !inside(reinterpret_cast<const void*>(exports->restore), false) ||
           !inside(reinterpret_cast<const void*>(exports->fill), true) ||
           !inside(reinterpret_cast<const void*>(exports->draw), true) ||
           !inside(reinterpret_cast<const void*>(exports->cursor), true) ||
           !inside(reinterpret_cast<const void*>(exports->reflect), true) ||
           !native_display::add(*exports))
        {
            debug("driver: ")(package.name)(" rejected")();
            continue;
        }
        debug("driver: loaded ")(package.name)(" from ")(package.path)(" bytes=")(size)();
    }
}
}  // namespace
void load_boot_drivers()
{
    if(attempted)
        return;
    attempted = true;
    load_display_modules();
    bool match = false;
    for(size_t i = 0; i < platform_pci_device_count(); ++i)
        if(pci_driver_matches_device(pci_driver, platform_pci_devices()[i]))
            match = true;
    if(!match)
    {
        debug("driver: virtio-net not needed")();
        return;
    }
    VirtualMemory vm(page_frames, g_kernel_root_cr3);
    driver::Image image;
    uint64_t size = 0;
    if(!load_image(vm,
                   kDriverPath,
                   sizeof(kDriverPath) - 1,
                   kDriverDigest,
                   OS1_DRIVER_MODULE_BASE,
                   image,
                   size))
        return;
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
