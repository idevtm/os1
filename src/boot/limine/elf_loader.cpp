#include "elf_loader.hpp"

#include "elf/elf64.hpp"
#include "handoff/memory_layout.h"
#include "paging.hpp"
#include "serial.hpp"
#include "freestanding/string.hpp"

namespace limine_shim
{
namespace
{
[[nodiscard]] const elf::Elf64Header* validate_kernel_file(const limine_file& kernel_file)
{
    return elf::header_from_image(kernel_file.address, kernel_file.size);
}
}  // namespace

[[nodiscard]] bool inspect_kernel_image(const limine_file& kernel_file,
                                        std::span<const BootMemoryRegion> memory_map,
                                        uint64_t& entry_point,
                                        uint64_t& kernel_physical_start,
                                        uint64_t& kernel_physical_end)
{
    const auto* header = validate_kernel_file(kernel_file);
    if(nullptr == header)
    {
        return false;
    }

    write_serial("[limine-shim] kernel module @ 0x");
    write_serial_hex(reinterpret_cast<uint64_t>(kernel_file.address));
    write_serial(" size=0x");
    write_serial_hex(kernel_file.size);
    write_serial_ln("");

    kernel_physical_start = ~0ull;
    kernel_physical_end = 0;
    bool entry_mapped = false;
    for(uint16_t i = 0; i < header->phnum; ++i)
    {
        const auto* program =
            elf::program_header_from_image(*header, kernel_file.address, kernel_file.size, i);
        if(nullptr == program)
        {
            return false;
        }

        if(elf::kProgramTypeLoad != program->type)
        {
            continue;
        }
        if(!elf::loadable_segment_bounds_valid(*program, kernel_file.size))
        {
            return false;
        }
        if(program->paddr < kKernelReservedPhysicalStart ||
           program->paddr > kKernelReservedPhysicalEnd ||
           program->memsz > kKernelReservedPhysicalEnd - program->paddr ||
           (program->memsz &&
            !boot_memory_range_is_usable(memory_map, program->paddr, program->memsz)))
        {
            write_serial("[limine-shim] PT_LOAD destination is not usable RAM at 0x");
            write_serial_hex(program->paddr);
            write_serial_ln("");
            return false;
        }
        if(program->vaddr != (program->paddr + kKernelVirtualOffset))
        {
            return false;
        }

        write_serial("[limine-shim] PT_LOAD vaddr=0x");
        write_serial_hex(program->vaddr);
        write_serial(" paddr=0x");
        write_serial_hex(program->paddr);
        write_serial(" filesz=0x");
        write_serial_hex(program->filesz);
        write_serial(" memsz=0x");
        write_serial_hex(program->memsz);
        write_serial_ln("");

        if(program->paddr < kernel_physical_start)
        {
            kernel_physical_start = program->paddr;
        }
        if((program->paddr + program->memsz) > kernel_physical_end)
        {
            kernel_physical_end = program->paddr + program->memsz;
        }
        if((header->entry >= program->vaddr) && (header->entry < (program->vaddr + program->memsz)))
        {
            entry_mapped = true;
        }
    }

    write_serial("[limine-shim] loaded range 0x");
    write_serial_hex(kernel_physical_start);
    write_serial("-0x");
    write_serial_hex(kernel_physical_end);
    write_serial_ln("");

    if((~0ull == kernel_physical_start) || (kernel_physical_end <= kernel_physical_start))
    {
        return false;
    }

    entry_point = header->entry;
    return (entry_point >= kKernelVirtualOffset) && entry_mapped;
}

[[nodiscard]] bool load_kernel_segments(const limine_file& kernel_file)
{
    const auto* header = validate_kernel_file(kernel_file);
    if(nullptr == header)
    {
        return false;
    }

    for(uint16_t i = 0; i < header->phnum; ++i)
    {
        const auto* program =
            elf::program_header_from_image(*header, kernel_file.address, kernel_file.size, i);
        if(nullptr == program)
        {
            return false;
        }

        if(elf::kProgramTypeLoad != program->type)
        {
            continue;
        }
        if(!elf::loadable_segment_bounds_valid(*program, kernel_file.size))
        {
            return false;
        }

        uint8_t* segment = map_physical_pointer<uint8_t>(program->paddr);
        if(nullptr == segment)
        {
            return false;
        }
        freestanding::zero_bytes(segment, static_cast<size_t>(program->memsz));
        freestanding::copy_bytes(segment,
                                 static_cast<const uint8_t*>(kernel_file.address) + program->offset,
                                 static_cast<size_t>(program->filesz));
    }

    return true;
}
}  // namespace limine_shim