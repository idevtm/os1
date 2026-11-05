// Initrd-backed ELF64 user-program loader. It owns user address-space creation,
// segment permission mapping, initial stack setup, and exec image replacement.
#include "proc/user_program.hpp"

#include "debug/debug.hpp"
#include "elf/elf64.hpp"
#include "fs/initrd.hpp"
#include "handoff/memory_layout.h"
#include "mm/user_copy.hpp"
#include "mm/virtual_memory.hpp"
#include "proc/startup.hpp"
#include "proc/user_elf.hpp"
#include "util/memory.h"
#include "vfs/initrd.hpp"

namespace
{
long load_user_elf(PageFrameContainer& frames,
                   uint64_t kernel_root_cr3,
                   const uint8_t* image,
                   uint64_t image_size,
                   uint64_t& cr3,
                   uint64_t& entry,
                   uint64_t& user_rsp)
{
    if((nullptr == image) || (image_size < sizeof(elf::Elf64Header)))
    {
        return OS1_ERR_BAD_IMAGE;
    }

    const auto* header = elf::header_from_image(image, image_size);
    if((nullptr == header) || !user_elf::validate_user_elf_image(*header, image, image_size))
    {
        return OS1_ERR_BAD_IMAGE;
    }

    VirtualMemory vm(frames);
    if(!vm.clone_kernel_mappings(kernel_root_cr3))
    {
        return OS1_ERR_NO_MEMORY;
    }

    for(uint16_t i = 0; i < header->phnum; ++i)
    {
        const auto* program = elf::program_header_from_image(*header, image, image_size, i);
        if(nullptr == program)
        {
            destroy_user_address_space(frames, vm.root());
            return OS1_ERR_BAD_IMAGE;
        }

        if(elf::kProgramTypeLoad != program->type)
        {
            continue;
        }
        if(0 == program->memsz)
        {
            continue;
        }
        user_elf::LoadSegmentPlan segment{};
        if(!user_elf::plan_load_segment(*program, image_size, segment))
        {
            destroy_user_address_space(frames, vm.root());
            return OS1_ERR_BAD_IMAGE;
        }

        if(!vm.allocate_and_map(
               segment.segment_start,
               (segment.segment_end - segment.segment_start) / kPageSize,
               segment.page_flags))
        {
            destroy_user_address_space(frames, vm.root());
            return OS1_ERR_NO_MEMORY;
        }

        if(!copy_into_address_space(vm, program->vaddr, image + program->offset, program->filesz))
        {
            destroy_user_address_space(frames, vm.root());
            return OS1_ERR_BAD_IMAGE;
        }
    }

    const uint64_t user_stack_base = user_elf::stack_base();
    if(!vm.allocate_and_map(
           user_stack_base,
           kUserStackPages,
           PageFlags::Present | PageFlags::Write | PageFlags::User | PageFlags::NoExecute))
    {
        destroy_user_address_space(frames, vm.root());
        return OS1_ERR_NO_MEMORY;
    }

    uint64_t stack_physical = 0;
    uint64_t stack_flags = 0;
    if(!vm.translate(kUserStackTop - 8, stack_physical, stack_flags))
    {
        debug("user stack translation missing at 0x")(kUserStackTop - 8, 16)();
        destroy_user_address_space(frames, vm.root());
        return OS1_ERR_BAD_IMAGE;
    }
    cr3 = vm.root();
    entry = header->entry;
    // Like kernel threads, first user entry reaches `_start` via `iretq`, so we
    // reserve one dummy slot to match the SysV function-entry stack shape.
    user_rsp = user_elf::initial_stack_pointer();
    return 0;
}
}  // namespace

bool destroy_user_address_space(PageFrameContainer& frames, uint64_t cr3)
{
    if(0 == cr3)
    {
        return false;
    }
    VirtualMemory vm(frames, cr3);
    vm.destroy_user_slot(kUserPml4Index);
    return frames.free(cr3);
}

long load_user_object_image(PageFrameContainer& frames,
                            uint64_t kernel_root_cr3,
                            OpenInstance* instance,
                            uint64_t& user_cr3,
                            uint64_t& entry,
                            uint64_t& user_rsp)
{
    if(!instance || instance->object->kind != OS1_OBJECT_FILE)
        return OS1_ERR_UNSUPPORTED;
    const auto* file = static_cast<VfsNode*>(instance->object);
    return load_user_elf(
        frames, kernel_root_cr3, file->bytes, file->byte_size, user_cr3, entry, user_rsp);
}

void prepare_user_thread_entry(Thread* thread, uint64_t entry, uint64_t user_rsp)
{
    if(nullptr == thread)
    {
        return;
    }

    thread->exit_status = 0;
    clear_thread_wait(thread);
    thread->frame = {};
    thread->frame.rip = entry;
    thread->frame.cs = kUserCodeSegment;
    thread->frame.rflags = 0x202;
    thread->frame.rsp = user_rsp;
    thread->frame.ss = kUserDataSegment;
    if(thread->process)
    {
        thread->process->exit_status = 0;
    }
}

Thread* load_user_program(PageFrameContainer& frames,
                          uint64_t kernel_root_cr3,
                          const char* path,
                          Process* parent,
                          bool start_ready)
{
    uint64_t user_cr3 = 0;
    uint64_t entry = 0;
    uint64_t user_rsp = 0;
    OpenInstance* image = nullptr;
    if(resolve_process_image(parent, path, image) < 0)
        return nullptr;
    InstanceRef image_pin(image);
    if(load_user_object_image(frames, kernel_root_cr3, image, user_cr3, entry, user_rsp) < 0)
    {
        return nullptr;
    }

    Process* process = create_user_process(path, user_cr3);
    if(nullptr == process)
    {
        destroy_user_address_space(frames, user_cr3);
        return nullptr;
    }
    process->parent = parent;

    long grants = parent ? descriptor_inherit_standard(parent->descriptors, process->descriptors)
                         : descriptor_bootstrap_console(process->descriptors);
    if(grants >= 0)
        grants = inherit_process_namespace(parent, process);
    if(grants < 0)
    {
        reap_process(process, frames);
        return nullptr;
    }
    os1_startup_record startup{};
    startup.version = OS1_STARTUP_VERSION;
    startup.size = sizeof(startup);
    startup.root_fd = process->root_fd;
    startup.cwd_fd = process->cwd_fd;
    size_t length = 0;
    while(length < sizeof(startup.strings) && path[length])
        ++length;
    if(length == sizeof(startup.strings))
    {
        reap_process(process, frames);
        return nullptr;
    }
    memcpy(startup.strings, path, length + 1);
    startup.argc = 1;
    startup.string_bytes = length + 1;
    uint64_t argument = 0;
    if(!install_startup_record(frames, user_cr3, startup, user_rsp, argument))
    {
        reap_process(process, frames);
        return nullptr;
    }
    Thread* thread = create_user_thread(process, entry, user_rsp, frames, false);
    if(nullptr == thread)
    {
        reap_process(process, frames);
        return nullptr;
    }
    thread->frame.rdi = argument;
    if(start_ready)
        mark_thread_ready(thread, thread->scheduler_cpu);
    return thread;
}
