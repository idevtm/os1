#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/cpu/cpu.hpp"
#include "bootlog.hpp"
#include "freestanding/string.hpp"
#include "elf_loader.hpp"
#include "handoff/boot_info.hpp"
#include "handoff/memory_layout.h"
#include "handoff_builder.hpp"
#include "limine.h"
#include "paging.hpp"
#include "serial.hpp"
#include "util/align.hpp"

// The shared kernel now enters in the higher-half window, but phase 1 still
// keeps a low handoff stack while the kernel owns the final CR3 transition.
// Keep this as a real assembler symbol rather than a naked C++ function:
// GCC's naked-function semantics are compiler-sensitive, and the GitHub runner
// uses a newer cross compiler than local `act` runs.
constexpr size_t kLimineShimStackBytes = 16 * 1024;

extern "C" [[noreturn]] void limine_enter_kernel(void (*)(BootInfo*, cpu*),
                                                  BootInfo*,
                                                  cpu*,
                                                  uint64_t);
extern "C" [[noreturn]] void limine_start_main(void);
extern "C"
{
alignas(16) constinit uint8_t g_limine_shim_stack[kLimineShimStackBytes]{};
}

asm(R"ASM(
.section .text
.global limine_enter_kernel
.type limine_enter_kernel, @function
limine_enter_kernel:
	cli
	cld
	mov %rdi, %rax
    mov %rcx, %rsp
	and $-16, %rsp
	mov %rsi, %rdi
	mov %rdx, %rsi
	call *%rax
1:
	cli
	hlt
	jmp 1b
.size limine_enter_kernel, .-limine_enter_kernel
)ASM");

namespace
{
constexpr uint64_t kLimineBaseRevisionRequested = 6;
constexpr const char* kKernelModuleName = "kernel.elf";
constexpr const char* kInitrdModuleName = "initrd.cpio";

__attribute__((used,
               section(".limine_requests_start"))) volatile uint64_t g_limine_requests_start[4] =
    LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests"))) volatile uint64_t g_limine_base_revision[3] =
    LIMINE_BASE_REVISION(kLimineBaseRevisionRequested);

__attribute__((used, section(".limine_requests"))) volatile limine_hhdm_request g_hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

__attribute__((used,
               section(".limine_requests"))) volatile limine_memmap_request g_memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

__attribute__((
    used,
    section(".limine_requests"))) volatile limine_framebuffer_request g_framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

__attribute__((used, section(".limine_requests"))) volatile limine_bootloader_info_request
    g_bootloader_info_request = {
        .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID,
        .revision = 0,
        .response = nullptr,
};

__attribute__((
    used,
    section(".limine_requests"))) volatile limine_executable_cmdline_request g_cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

__attribute__((used,
               section(".limine_requests"))) volatile limine_module_request g_module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
    .internal_module_count = 0,
    .internal_modules = nullptr,
};

__attribute__((used, section(".limine_requests"))) volatile limine_rsdp_request g_rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

__attribute__((used,
               section(".limine_requests"))) volatile limine_smbios_request g_smbios_request = {
    .id = LIMINE_SMBIOS_REQUEST_ID,
    .revision = 0,
    .response = nullptr,
};

__attribute__((used, section(".limine_requests_end"))) volatile uint64_t g_limine_requests_end[2] =
    LIMINE_REQUESTS_END_MARKER;

[[noreturn]] void halt_forever()
{
    for(;;)
    {
        asm volatile("cli");
        asm volatile("hlt");
    }
}

[[nodiscard]] const limine_file* find_module(const char* name)
{
    const auto* response = g_module_request.response;
    if(!limine_shim::limine_pointer_mapped(response) ||
       !limine_shim::limine_pointer_mapped(response->modules))
    {
        return nullptr;
    }

    for(uint64_t i = 0; i < response->module_count; ++i)
    {
        const limine_file* module = response->modules[i];
        if(!limine_shim::limine_pointer_mapped(module))
        {
            continue;
        }

        const char* path = limine_shim::limine_pointer_mapped(module->path) ? module->path : nullptr;
        const char* string =
            limine_shim::limine_pointer_mapped(module->string) ? module->string : nullptr;
        if((nullptr == path) && (nullptr == string))
        {
            continue;
        }

        if(freestanding::path_ends_with(path, name) || freestanding::strings_equal(string, name) ||
           freestanding::path_ends_with(string, name))
        {
            return module;
        }
    }

    return nullptr;
}
}  // namespace

asm(R"ASM(
.section .text
.global _start
.type _start, @function
_start:
	movabs $g_limine_shim_stack + 16384, %rax
	mov %rax, %rsp
	and $-16, %rsp
	call limine_start_main
1:
	cli
	hlt
	jmp 1b
.size _start, .-_start
)ASM");

extern "C" [[noreturn]] void limine_start_main()
{
    limine_shim::init_serial();
    asm volatile("cli");
    asm volatile("cld");
    limine_shim::write_serial_ln("[limine-shim] start");
    if(!LIMINE_BASE_REVISION_SUPPORTED(g_limine_base_revision))
    {
        limine_shim::write_serial_ln("[limine-shim] unsupported base revision");
        halt_forever();
    }

    const auto* hhdm = g_hhdm_request.response;
    if(nullptr == hhdm)
    {
        limine_shim::write_serial_ln("[limine-shim] missing HHDM response");
        halt_forever();
    }
    limine_shim::set_hhdm_offset(hhdm->offset);

    const limine_file* kernel_file = find_module(kKernelModuleName);
    const limine_file* initrd_file = find_module(kInitrdModuleName);
    if((nullptr == kernel_file) || (nullptr == initrd_file))
    {
        limine_shim::write_serial_ln("[limine-shim] missing required modules");
        halt_forever();
    }
    limine_shim::write_serial_ln("[limine-shim] modules discovered");
    const limine_file kernel_module = *kernel_file;
    const limine_file initrd_module = *initrd_file;

    BootMemoryRegion memory_map_storage[kBootInfoMaxMemoryRegions];
    const auto memory_map =
        limine_shim::read_memory_map(g_memmap_request.response, memory_map_storage);
    if(memory_map.empty())
    {
        limine_shim::write_serial_ln("[limine-shim] missing or invalid memory map");
        halt_forever();
    }

    uint64_t entry_point = 0;
    uint64_t kernel_physical_start = 0;
    uint64_t kernel_physical_end = 0;
    uint64_t boot_info_storage_physical = 0;
    uint64_t bootstrap_stack_top = 0;
    cpu* cpu_boot = nullptr;
    if(!limine_shim::inspect_kernel_image(
           kernel_module, memory_map, entry_point, kernel_physical_start, kernel_physical_end))
    {
        limine_shim::write_serial_ln("[limine-shim] kernel inspect failed");
        halt_forever();
    }
    if(!limine_shim::prepare_kernel_handoff(memory_map,
                                            kernel_physical_end,
                                            cpu_boot,
                                            boot_info_storage_physical,
                                            bootstrap_stack_top))
    {
        limine_shim::write_serial_ln("[limine-shim] kernel handoff prep failed");
        halt_forever();
    }

    limine_shim::write_serial_ln("[limine-shim] building BootInfo");
    const limine_shim::LimineBootResponses responses{
        .bootloader_info = g_bootloader_info_request.response,
        .cmdline = g_cmdline_request.response,
        .rsdp = g_rsdp_request.response,
        .smbios = g_smbios_request.response,
        .framebuffer = g_framebuffer_request.response,
    };
    BootInfo* boot_info = limine_shim::build_boot_info(responses,
                                                       initrd_module,
                                                       memory_map,
                                                       kernel_physical_start,
                                                       kernel_physical_end,
                                                       boot_info_storage_physical);
    if(nullptr == boot_info)
    {
        limine_shim::write_serial_ln("[limine-shim] BootInfo build failed");
        halt_forever();
    }
    if(!limine_shim::load_kernel_segments(kernel_module))
    {
        limine_shim::write_serial_ln("[limine-shim] kernel load failed");
        halt_forever();
    }

    const uint64_t cpu_boot_end =
        align_up(reinterpret_cast<uint64_t>(cpu_boot) + limine_shim::kPageSize,
                 limine_shim::kPageSize);
    const uint64_t bitmap_end = kPageFrameBitmapBaseAddress + kPageFrameBitmapSizeBytes;
    const uint64_t low_handoff_end = (cpu_boot_end > bitmap_end) ? cpu_boot_end : bitmap_end;
    if(!limine_shim::ensure_bootstrap_low_window(low_handoff_end))
    {
        limine_shim::write_serial_ln("[limine-shim] low identity map failed");
        halt_forever();
    }
    if(!limine_shim::ensure_kernel_higher_half_window(kernel_physical_end))
    {
        limine_shim::write_serial_ln("[limine-shim] high kernel map failed");
        halt_forever();
    }
    limine_shim::write_serial("[limine-shim] loaded kernel entry=0x");
    limine_shim::write_serial_hex(entry_point);
    limine_shim::write_serial(" cpu=0x");
    limine_shim::write_serial_hex(reinterpret_cast<uint64_t>(cpu_boot));
    limine_shim::write_serial_ln("");
    limine_shim::write_serial_ln("[limine-shim] entering kernel_main");

    limine_enter_kernel(reinterpret_cast<void (*)(BootInfo*, cpu*)>(entry_point),
                        boot_info,
                        cpu_boot,
                        bootstrap_stack_top);
}