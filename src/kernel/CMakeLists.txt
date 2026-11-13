set(KERNEL_CORE_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/linker/kernel_core.ld")
set(KERNEL_LIMINE_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/linker/kernel_limine.ld")
set(KERNEL_ARCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/arch/x86_64")

set(KERNEL_ARCH_SOURCES
  arch/x86_64/apic/ipi.cpp
  arch/x86_64/apic/ioapic.cpp
  arch/x86_64/apic/lapic.cpp
  arch/x86_64/apic/pic.cpp
  arch/x86_64/cpu/cpu.cpp
  arch/x86_64/cpu/syscall.cpp
  arch/x86_64/interrupt/interrupt.cpp
  arch/x86_64/interrupt/vector_allocator.cpp
)

set(KERNEL_HANDOFF_SOURCES
  handoff/boot_info.cpp
)

set(KERNEL_MM_SOURCES
  mm/boot_mapping.cpp
  mm/boot_reserve.cpp
  mm/dma.cpp
  mm/kmem.cpp
  mm/page_frame.cpp
  mm/user_copy.cpp
  mm/virtual_memory.cpp
)

set(KERNEL_CONSOLE_SOURCES
  console/console.cpp
  console/console_input.cpp
  console/terminal.cpp
  console/terminal_switcher.cpp
)

set(KERNEL_DRIVER_SOURCES
  drivers/block/virtio_blk.cpp
  drivers/bus/device.cpp
  drivers/bus/driver_registry.cpp
  drivers/bus/pci_bus.cpp
  drivers/bus/resource.cpp
  drivers/display/text_display.cpp
  drivers/display/service.cpp
  drivers/display/reflection.cpp
  drivers/input/service.cpp
  drivers/input/ps2_keyboard.cpp
  drivers/input/ps2_console.cpp
  drivers/module/loader.cpp
  drivers/module/virtio_host.cpp
  drivers/timer/pit.cpp
  drivers/timer/tsc_clock.cpp
  drivers/watchdog/intel_tco.cpp
  drivers/usb/diagnostics.cpp
  drivers/usb/stall_trace.cpp
  drivers/usb/ehci.cpp
  drivers/usb/ehci_controller.cpp
  drivers/usb/hub.cpp
  drivers/usb/storage_devices.cpp
  drivers/usb/mass_storage.cpp
  drivers/usb/hid_keyboard.cpp
  drivers/usb/worker.cpp
  drivers/usb/xhci.cpp
  drivers/usb/xhci_controller.cpp
  drivers/virtio/pci_transport.cpp
  drivers/virtio/virtqueue.cpp
)

set(KERNEL_FS_SOURCES
  fs/cpio_newc.cpp
  fs/initrd.cpp
)

set(KERNEL_STORAGE_SOURCES
  storage/block_device.cpp
)

set(KERNEL_CORE_SOURCES
  core/fault.cpp
  core/irq_dispatch.cpp
  core/kernel_main.cpp
  core/kernel_state.cpp
  core/panic.cpp
  core/timer_source.cpp
  core/wall_clock.cpp
  core/trap_dispatch.cpp
)

set(KERNEL_PLATFORM_SOURCES
  platform/acpi.cpp
  platform/acpi_aml.cpp
  platform/acpica_integration.cpp
  platform/acpica_osl.cpp
  platform/device_probe.cpp
  platform/hpet.cpp
  platform/init.cpp
  platform/irq_routing.cpp
  platform/irq_registry.cpp
  platform/power.cpp
  platform/system_control.cpp
  platform/system_power.cpp
  platform/acpica_ec.cpp
  platform/rtc.cpp
  platform/hardware.cpp
  platform/pci.cpp
  platform/pci_capability.cpp
  platform/pci_config.cpp
  platform/pci_msi.cpp
  platform/state.cpp
  platform/topology.cpp
)

set(KERNEL_PROC_SOURCES
  buffer/buffer.cpp
  ipc/channel.cpp
  ipc/kernel_provider.cpp
  vfs/initrd.cpp
  vfs/tmpfs.cpp
  vfs/ext2.cpp
  fs/ext2.cpp
  syscall/vfs.cpp
  proc/startup.cpp
  resource/descriptor.cpp
  syscall/resource.cpp
  proc/process.cpp
  proc/reaper.cpp
  proc/kernel_stack.cpp
  proc/thread.cpp
  proc/user_elf.cpp
  proc/user_program.cpp
)

set(KERNEL_SCHED_SOURCES
  sched/idle.cpp
  sched/scheduler.cpp
  sched/thread_queue.cpp
)

set(KERNEL_SYSCALL_SOURCES
  syscall/console_read.cpp
  syscall/dispatch.cpp
  syscall/observe.cpp
  syscall/process.cpp
  syscall/wait.cpp
)

set(KERNEL_SYNC_SOURCES
  sync/smp.cpp
  sync/wait_queue.cpp
)

set(KERNEL_DEBUG_SOURCES
  debug/bootlog.cpp
  debug/debug.cpp
  debug/event_ring.cpp
  debug/log.cpp
  debug/log_export.cpp
)

set(KERNEL_UTIL_SOURCES
  util/string.cpp
)

set(KERNEL_LIMINE_SOURCES
  "${PROJECT_SOURCE_DIR}/src/boot/limine/bootlog.cpp"
  "${PROJECT_SOURCE_DIR}/src/boot/limine/elf_loader.cpp"
  "${PROJECT_SOURCE_DIR}/src/boot/limine/entry.cpp"
  "${PROJECT_SOURCE_DIR}/src/boot/limine/handoff_builder.cpp"
  "${PROJECT_SOURCE_DIR}/src/boot/limine/paging.cpp"
  "${PROJECT_SOURCE_DIR}/src/boot/limine/serial.cpp"
)

set(KERNEL_CXX_SOURCES
  ${KERNEL_ARCH_SOURCES}
  ${KERNEL_HANDOFF_SOURCES}
  ${KERNEL_MM_SOURCES}
  ${KERNEL_CONSOLE_SOURCES}
  ${KERNEL_DRIVER_SOURCES}
  ${KERNEL_FS_SOURCES}
  ${KERNEL_STORAGE_SOURCES}
  ${KERNEL_CORE_SOURCES}
  ${KERNEL_PLATFORM_SOURCES}
  ${KERNEL_PROC_SOURCES}
  ${KERNEL_SCHED_SOURCES}
  ${KERNEL_SYSCALL_SOURCES}
  ${KERNEL_SYNC_SOURCES}
  ${KERNEL_DEBUG_SOURCES}
  ${KERNEL_UTIL_SOURCES}
  "${PROJECT_SOURCE_DIR}/src/libc/stdlib.cpp"
)

set(OS1_REPO_ROOT "${PROJECT_SOURCE_DIR}")
include("${PROJECT_SOURCE_DIR}/cmake/acpica_sources.cmake")

set(OS1_KERNEL_ACPICA_SOURCES ${OS1_ACPICA_SOURCES})
set(OS1_ACPICA_UTCLIB_SOURCE
  "${OS1_ACPICA_ROOT}/source/components/utilities/utclib.c"
)

add_library(os1_acpica STATIC ${OS1_KERNEL_ACPICA_SOURCES})
set_source_files_properties(
  "${OS1_ACPICA_UTCLIB_SOURCE}"
  PROPERTIES
    COMPILE_DEFINITIONS "memcpy=os1_acpica_memcpy;memset=os1_acpica_memset"
)
target_include_directories(os1_acpica PRIVATE
  "${OS1_ACPICA_CUSTOM_INCLUDE_DIR}"
  "${OS1_ACPICA_INCLUDE_DIR}"
)
set_target_properties(
  os1_acpica
  PROPERTIES
    C_STANDARD 99
    C_EXTENSIONS ON
)
target_compile_options(
  os1_acpica
  PRIVATE
    -ffreestanding
    -mno-red-zone -fstack-clash-protection
    -march=x86-64
    -mtune=generic
    -mgeneral-regs-only
    -mcmodel=kernel
    -fno-pic
    -fno-pie
    -O1
    -fno-asynchronous-unwind-tables
    -fno-unwind-tables
    -w
)

set(OS1_SOURCE_REVISION "" CACHE STRING "Revision label for source archives without Git metadata")
add_custom_target(os1_log_build_identity
  COMMAND "${OS1_PYTHON_EXECUTABLE}" -E "${PROJECT_SOURCE_DIR}/cmake/scripts/log_build_identity.py"
          --root "${PROJECT_SOURCE_DIR}" --output "${OS1_GENERATED_DIR}/log_build_identity.hpp"
          "--archive-revision=${OS1_SOURCE_REVISION}"
          --profile "${CMAKE_BUILD_TYPE}:${CMAKE_CXX_COMPILER_ID}:${CMAKE_CXX_COMPILER_VERSION}:${CMAKE_CXX_FLAGS}:${OS1_KMEM_DEBUG}:${OS1_QEMU_DEBUGCON}:${OS1_BOOT_DIAGNOSTICS}:${OS1_USB_KEYBOARD_DIAGNOSTICS}"
  BYPRODUCTS "${OS1_GENERATED_DIR}/log_build_identity.hpp"
  VERBATIM)
# The disk module has no imported ELF symbols. Kernel services enter through one ABI table.
set(OS1_DRIVER_ELF "${OS1_ARTIFACT_DIR}/drivers/virtio-net.elf")
set(OS1_DRIVER_METADATA "${OS1_GENERATED_DIR}/driver-package.json")
add_custom_command(OUTPUT "${OS1_DRIVER_ELF}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ARTIFACT_DIR}/drivers"
  COMMAND "${CMAKE_CXX_COMPILER}" -std=gnu++20 -ffreestanding -mno-red-zone -fstack-clash-protection
    -mgeneral-regs-only -mcmodel=kernel -fno-pic -fno-pie -no-pie -Os
    -fno-stack-protector -fno-exceptions -fno-rtti -fno-threadsafe-statics
    -fno-asynchronous-unwind-tables -fno-unwind-tables -nostdlib -nostartfiles
    "-I${PROJECT_SOURCE_DIR}/src/common" "-I${OS1_GENERATED_DIR}"
    "-Wl,-T,${PROJECT_SOURCE_DIR}/src/drivers/virtio-net/linker.ld"
    -Wl,--build-id=none -Wl,-z,max-page-size=4096
    "${PROJECT_SOURCE_DIR}/src/drivers/virtio-net/driver.cpp" -o "${OS1_DRIVER_ELF}"
  DEPENDS os1_log_build_identity "${OS1_GENERATED_DIR}/log_build_identity.hpp"
    "${PROJECT_SOURCE_DIR}/src/drivers/virtio-net/driver.cpp"
    "${PROJECT_SOURCE_DIR}/src/drivers/virtio-net/linker.ld"
    "${PROJECT_SOURCE_DIR}/src/common/driver/abi.h"
  VERBATIM)
add_custom_command(OUTPUT "${OS1_GENERATED_DIR}/driver_trust.hpp" "${OS1_DRIVER_METADATA}"
  COMMAND "${OS1_PYTHON_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/cmake/scripts/driver_package.py"
    --image "${OS1_DRIVER_ELF}" --identity "${OS1_GENERATED_DIR}/log_build_identity.hpp"
    --header "${OS1_GENERATED_DIR}/driver_trust.hpp" --metadata "${OS1_DRIVER_METADATA}"
  DEPENDS "${OS1_DRIVER_ELF}" "${OS1_GENERATED_DIR}/log_build_identity.hpp"
    "${PROJECT_SOURCE_DIR}/cmake/scripts/driver_package.py"
  VERBATIM)
add_custom_target(os1_driver_package ALL
  DEPENDS "${OS1_GENERATED_DIR}/driver_trust.hpp" "${OS1_DRIVER_METADATA}")
set(OS1_DRIVER_ELF "${OS1_DRIVER_ELF}" PARENT_SCOPE)
set(OS1_DRIVER_METADATA "${OS1_DRIVER_METADATA}" PARENT_SCOPE)
set(OS1_DISPLAY_DRIVER_ELF "${OS1_ARTIFACT_DIR}/drivers/firmware-framebuffer.elf")
set(OS1_DISPLAY_DRIVER_METADATA "${OS1_GENERATED_DIR}/display-driver-package.json")
add_custom_command(OUTPUT "${OS1_DISPLAY_DRIVER_ELF}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ARTIFACT_DIR}/drivers"
  COMMAND "${CMAKE_CXX_COMPILER}" -std=gnu++20 -ffreestanding -mno-red-zone -fstack-clash-protection
    -mgeneral-regs-only -mcmodel=kernel -fno-pic -fno-pie -no-pie -Os
    -fno-stack-protector -fno-exceptions -fno-rtti -fno-threadsafe-statics
    -fno-asynchronous-unwind-tables -fno-unwind-tables -nostdlib -nostartfiles
    "-I${PROJECT_SOURCE_DIR}/src/common" "-I${PROJECT_SOURCE_DIR}/src/uapi" "-I${OS1_GENERATED_DIR}"
    "-Wl,-T,${PROJECT_SOURCE_DIR}/src/drivers/firmware-framebuffer/linker.ld"
    -Wl,--build-id=none -Wl,-z,max-page-size=4096
    "${PROJECT_SOURCE_DIR}/src/drivers/firmware-framebuffer/driver.cpp" -o "${OS1_DISPLAY_DRIVER_ELF}"
  DEPENDS os1_log_build_identity "${OS1_GENERATED_DIR}/log_build_identity.hpp"
    "${PROJECT_SOURCE_DIR}/src/drivers/firmware-framebuffer/driver.cpp"
    "${PROJECT_SOURCE_DIR}/src/drivers/firmware-framebuffer/linker.ld"
    "${PROJECT_SOURCE_DIR}/src/common/driver/display.h"
    "${PROJECT_SOURCE_DIR}/src/common/driver/draw.hpp"
    "${PROJECT_SOURCE_DIR}/src/uapi/os1/display.h"
    "${PROJECT_SOURCE_DIR}/src/uapi/os1/hardware.h"
  VERBATIM)
add_custom_command(OUTPUT "${OS1_GENERATED_DIR}/display_driver_trust.hpp" "${OS1_DISPLAY_DRIVER_METADATA}"
  COMMAND "${OS1_PYTHON_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/cmake/scripts/driver_package.py"
    --display --image "${OS1_DISPLAY_DRIVER_ELF}" --identity "${OS1_GENERATED_DIR}/log_build_identity.hpp"
    --header "${OS1_GENERATED_DIR}/display_driver_trust.hpp" --metadata "${OS1_DISPLAY_DRIVER_METADATA}"
  DEPENDS "${OS1_DISPLAY_DRIVER_ELF}" "${OS1_GENERATED_DIR}/log_build_identity.hpp"
    "${PROJECT_SOURCE_DIR}/cmake/scripts/driver_package.py"
  VERBATIM)
add_custom_target(os1_display_driver_package ALL
  DEPENDS "${OS1_GENERATED_DIR}/display_driver_trust.hpp" "${OS1_DISPLAY_DRIVER_METADATA}")
set(OS1_DISPLAY_DRIVER_ELF "${OS1_DISPLAY_DRIVER_ELF}" PARENT_SCOPE)
set(OS1_DISPLAY_DRIVER_METADATA "${OS1_DISPLAY_DRIVER_METADATA}" PARENT_SCOPE)
set(OS1_GM206_DRIVER_ELF "${OS1_ARTIFACT_DIR}/drivers/gm206.elf")
set(OS1_GM206_DRIVER_METADATA "${OS1_GENERATED_DIR}/gm206-driver-package.json")
add_custom_command(OUTPUT "${OS1_GM206_DRIVER_ELF}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ARTIFACT_DIR}/drivers"
  COMMAND "${CMAKE_CXX_COMPILER}" -std=gnu++20 -ffreestanding -mno-red-zone -fstack-clash-protection
    -mgeneral-regs-only -mcmodel=kernel -fno-pic -fno-pie -no-pie -Os
    -fno-stack-protector -fno-exceptions -fno-rtti -fno-threadsafe-statics
    -fno-asynchronous-unwind-tables -fno-unwind-tables -nostdlib -nostartfiles
    "-I${PROJECT_SOURCE_DIR}/src/common" "-I${PROJECT_SOURCE_DIR}/src/uapi" "-I${OS1_GENERATED_DIR}"
    "-Wl,-T,${PROJECT_SOURCE_DIR}/src/drivers/gm206/linker.ld"
    -Wl,--build-id=none -Wl,-z,max-page-size=4096
    "${PROJECT_SOURCE_DIR}/src/drivers/gm206/driver.cpp"
    "${PROJECT_SOURCE_DIR}/src/drivers/gm206/display.cpp"
    "${PROJECT_SOURCE_DIR}/src/drivers/gm206/reflection.cpp" -o "${OS1_GM206_DRIVER_ELF}"
  DEPENDS os1_log_build_identity "${OS1_GENERATED_DIR}/log_build_identity.hpp"
    "${PROJECT_SOURCE_DIR}/src/drivers/gm206/driver.cpp"
    "${PROJECT_SOURCE_DIR}/src/drivers/gm206/display.cpp"
    "${PROJECT_SOURCE_DIR}/src/drivers/gm206/reflection.cpp"
    "${PROJECT_SOURCE_DIR}/src/drivers/gm206/linker.ld"
    "${PROJECT_SOURCE_DIR}/src/common/driver/gm206.h"
    "${PROJECT_SOURCE_DIR}/src/common/driver/display.h"
    "${PROJECT_SOURCE_DIR}/src/common/freestanding/string.hpp"
    "${PROJECT_SOURCE_DIR}/src/uapi/os1/display.h"
    "${PROJECT_SOURCE_DIR}/src/uapi/os1/hardware.h"
    "${PROJECT_SOURCE_DIR}/src/uapi/os1/resource.h"
    "${PROJECT_SOURCE_DIR}/src/uapi/os1/vfs.h"
  VERBATIM)
add_custom_command(OUTPUT "${OS1_GENERATED_DIR}/gm206_driver_trust.hpp" "${OS1_GM206_DRIVER_METADATA}"
  COMMAND "${OS1_PYTHON_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/cmake/scripts/driver_package.py"
    --gm206 --image "${OS1_GM206_DRIVER_ELF}" --identity "${OS1_GENERATED_DIR}/log_build_identity.hpp"
    --header "${OS1_GENERATED_DIR}/gm206_driver_trust.hpp" --metadata "${OS1_GM206_DRIVER_METADATA}"
  DEPENDS "${OS1_GM206_DRIVER_ELF}" "${OS1_GENERATED_DIR}/log_build_identity.hpp"
    "${PROJECT_SOURCE_DIR}/cmake/scripts/driver_package.py"
  VERBATIM)
add_custom_command(OUTPUT "${OS1_GENERATED_DIR}/display_modules.hpp"
  COMMAND "${OS1_PYTHON_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/cmake/scripts/display_module_table.py"
    --header "${OS1_GENERATED_DIR}/display_modules.hpp"
    "${OS1_DISPLAY_DRIVER_METADATA}" "${OS1_GM206_DRIVER_METADATA}"
  DEPENDS "${OS1_DISPLAY_DRIVER_METADATA}" "${OS1_GM206_DRIVER_METADATA}"
    "${PROJECT_SOURCE_DIR}/cmake/scripts/display_module_table.py"
  VERBATIM)
add_custom_target(os1_gm206_driver_package ALL
  DEPENDS "${OS1_GENERATED_DIR}/gm206_driver_trust.hpp" "${OS1_GM206_DRIVER_METADATA}"
    "${OS1_GENERATED_DIR}/display_modules.hpp")
# The table rule reads the metadata of the framebuffer package. Its target owns that file.
add_dependencies(os1_gm206_driver_package os1_display_driver_package)
set(OS1_GM206_DRIVER_ELF "${OS1_GM206_DRIVER_ELF}" PARENT_SCOPE)
set(OS1_GM206_DRIVER_METADATA "${OS1_GM206_DRIVER_METADATA}" PARENT_SCOPE)
add_library(kernel_core_objects OBJECT ${KERNEL_CXX_SOURCES})
add_dependencies(kernel_core_objects os1_log_build_identity os1_driver_package os1_display_driver_package os1_gm206_driver_package)
target_include_directories(kernel_core_objects PRIVATE
  "${PROJECT_SOURCE_DIR}/src/common"
  "${OS1_ACPICA_CUSTOM_INCLUDE_DIR}"
  "${OS1_GENERATED_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${KERNEL_ARCH_DIR}"
  "${PROJECT_SOURCE_DIR}/src/libc"
  "${PROJECT_SOURCE_DIR}/src/uapi"
  "${PROJECT_SOURCE_DIR}/third_party/font8x16"
)
target_include_directories(kernel_core_objects SYSTEM PRIVATE
  "${OS1_ACPICA_INCLUDE_DIR}"
)
set_target_properties(
  kernel_core_objects
  PROPERTIES
    C_STANDARD 99
    C_EXTENSIONS ON
    CXX_STANDARD 20
    CXX_EXTENSIONS ON
)
target_compile_options(
  kernel_core_objects
  PRIVATE
    # Size optimization keeps the BIOS kernel and bundled rich-parameter tools
    # in their bounded low-memory staging windows without loader changes.
    # Kernel code runs with asynchronous IRQs on the current stack, so the
    # SysV red zone is unsafe and must stay disabled across both boot paths.
    # The kernel does not enable or context-switch SIMD/FPU state yet. Force
    # integer-register-only codegen so newer cross-compilers cannot introduce
    # XMM/AVX instructions that trap as #UD on the CI runner's virtual CPU.
    $<$<COMPILE_LANGUAGE:C>:-ffreestanding -mno-red-zone -fstack-clash-protection -march=x86-64 -mtune=generic -mgeneral-regs-only -mcmodel=kernel -fno-pic -fno-pie -O1 -Wall -Wextra -fno-asynchronous-unwind-tables -fno-unwind-tables>
    $<$<COMPILE_LANGUAGE:CXX>:-ffreestanding -mno-red-zone -fstack-clash-protection -march=x86-64 -mtune=generic -mgeneral-regs-only -mcmodel=kernel -fno-pic -fno-pie -Os -Wall -Wextra -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-exceptions -fno-rtti>
)
if(OS1_KMEM_DEBUG)
  target_compile_definitions(kernel_core_objects PRIVATE OS1_KMEM_DEBUG=1)
endif()
if(OS1_QEMU_DEBUGCON)
  target_compile_definitions(kernel_core_objects PRIVATE OS1_QEMU_DEBUGCON=1)
endif()

add_library(kernel_limine_frontend OBJECT ${KERNEL_LIMINE_SOURCES})
target_include_directories(kernel_limine_frontend PRIVATE
  "${PROJECT_SOURCE_DIR}/src/common"
  "${OS1_GENERATED_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${KERNEL_ARCH_DIR}"
  "${PROJECT_SOURCE_DIR}/src/libc"
  "${PROJECT_SOURCE_DIR}/src/uapi"
  "${PROJECT_SOURCE_DIR}/third_party/limine/v11.4.0"
)
set_target_properties(
  kernel_limine_frontend
  PROPERTIES
    CXX_STANDARD 20
    CXX_EXTENSIONS ON
)
target_compile_options(
  kernel_limine_frontend
  PRIVATE
    -ffreestanding
    -mno-red-zone
    -march=x86-64
    -mtune=generic
    -mgeneral-regs-only
    -mcmodel=large
    -O0
    -Wall
    -Wextra
    -fno-asynchronous-unwind-tables
    -fno-unwind-tables
    -fno-exceptions
    -fno-rtti
)

file(GLOB KERNEL_ASM_DEPENDENCIES CONFIGURE_DEPENDS
  "${KERNEL_ARCH_DIR}/apic/*.asm"
  "${KERNEL_ARCH_DIR}/asm/*.asm"
  "${KERNEL_ARCH_DIR}/include/*.inc"
  "${KERNEL_ARCH_DIR}/interrupt/*.asm"
  "${CMAKE_CURRENT_SOURCE_DIR}/handoff/*.inc"
  "${CMAKE_CURRENT_SOURCE_DIR}/proc/*.inc"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.inc"
)
list(SORT KERNEL_ASM_DEPENDENCIES)

function(os1_add_nasm_object out_var source_name format)
  get_filename_component(source_stem "${source_name}" NAME_WE)
  set(output_path "${CMAKE_CURRENT_BINARY_DIR}/${source_stem}.o")
  add_custom_command(
    OUTPUT "${output_path}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}"
    COMMAND "${OS1_NASM_EXECUTABLE}" "-f${format}"
      -I "${OS1_GENERATED_DIR}/"
      -I "${KERNEL_ARCH_DIR}/include/"
      -I "${CMAKE_CURRENT_SOURCE_DIR}/handoff/"
      -I "${CMAKE_CURRENT_SOURCE_DIR}/proc/"
      -I "${CMAKE_CURRENT_SOURCE_DIR}/"
      "${CMAKE_CURRENT_SOURCE_DIR}/${source_name}"
      -o "${output_path}"
    DEPENDS ${KERNEL_ASM_DEPENDENCIES}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM
  )
  set("${out_var}" "${output_path}" PARENT_SCOPE)
endfunction()

os1_add_nasm_object(KERNEL_MEMORY_OBJECT "arch/x86_64/asm/memory.asm" "elf64")
os1_add_nasm_object(KERNEL_IRQHANDLER_OBJECT "arch/x86_64/interrupt/irqhandler.asm" "elf64")
os1_add_nasm_object(KERNEL_INTHANDLER_OBJECT "arch/x86_64/interrupt/inthandler.asm" "elf64")
os1_add_nasm_object(KERNEL_MULTITASK_OBJECT "arch/x86_64/asm/multitask.asm" "elf64")
os1_add_nasm_object(KERNEL_CPU_START_OBJECT "arch/x86_64/asm/cpu_start.asm" "elf64")

set(KERNEL_ASM_OBJECTS
  "${KERNEL_MEMORY_OBJECT}"
  "${KERNEL_IRQHANDLER_OBJECT}"
  "${KERNEL_INTHANDLER_OBJECT}"
  "${KERNEL_MULTITASK_OBJECT}"
  "${KERNEL_CPU_START_OBJECT}"
)

if(BUILD_TESTING AND OS1_QEMU_EXECUTABLE)
  set(OS1_HANDOFF_LINK_OBJECTS ${KERNEL_ASM_OBJECTS})
  list(REMOVE_ITEM OS1_HANDOFF_LINK_OBJECTS "${KERNEL_MULTITASK_OBJECT}")
  file(GENERATE OUTPUT "${OS1_GENERATED_DIR}/handoff-test-objects.txt"
    CONTENT "$<JOIN:$<TARGET_OBJECTS:kernel_core_objects>,\n>\n$<JOIN:${OS1_HANDOFF_LINK_OBJECTS},\n>\n$<TARGET_FILE:os1_acpica>\n")
  add_test(NAME os1_interrupt_handoff_contract
    COMMAND "${OS1_PYTHON_EXECUTABLE}" -E -B "${PROJECT_SOURCE_DIR}/tests/scripts/test_interrupt_handoff.py"
      --root "${PROJECT_SOURCE_DIR}"
      --objects "${OS1_GENERATED_DIR}/handoff-test-objects.txt"
      --linker "${CMAKE_LINKER}" --link-script "${KERNEL_CORE_LINKER_SCRIPT}"
      --nasm "${OS1_NASM_EXECUTABLE}" --strip "${OS1_STRIP_EXECUTABLE}"
      --physical-base "${OS1_KERNEL_RESERVED_PHYSICAL_START}"
      --virtual-offset "${OS1_KERNEL_VIRTUAL_OFFSET}"
      --image "${OS1_RAW_IMAGE}" --qemu "${OS1_QEMU_EXECUTABLE}"
      --program-disk "${OS1_ARTIFACT_DIR}/program-test-disk.raw"
      --kernel-lba "${OS1_KERNEL_IMAGE_START_LBA}" --kernel-sectors "${OS1_KERNEL_IMAGE_SECTOR_COUNT}"
      --log "${OS1_ARTIFACT_DIR}/interrupt-handoff.log")
  set_tests_properties(os1_interrupt_handoff_contract PROPERTIES TIMEOUT 90)
endif()

# Pack file offsets tightly; kernel_core.ld still page-aligns memory boundaries.
add_custom_command(
  OUTPUT "${OS1_KERNEL_ELF}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ARTIFACT_DIR}"
  COMMAND
    "${CMAKE_LINKER}"
    -T "${KERNEL_CORE_LINKER_SCRIPT}"
    "--defsym=OS1_KERNEL_PHYSICAL_BASE=${OS1_KERNEL_RESERVED_PHYSICAL_START}"
    "--defsym=OS1_KERNEL_VIRTUAL_OFFSET=${OS1_KERNEL_VIRTUAL_OFFSET}"
    -z max-page-size=0x100 -z common-page-size=0x100
    $<TARGET_OBJECTS:kernel_core_objects>
    ${KERNEL_ASM_OBJECTS}
    "$<TARGET_FILE:os1_acpica>"
    -o "${OS1_KERNEL_ELF}"
  DEPENDS
    os1_acpica
    kernel_core_objects
    $<TARGET_OBJECTS:kernel_core_objects>
    "${KERNEL_CORE_LINKER_SCRIPT}"
    ${KERNEL_ASM_OBJECTS}
  COMMAND_EXPAND_LISTS
  VERBATIM
)

add_custom_command(
  OUTPUT "${OS1_KERNEL_LIMINE_ELF}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ARTIFACT_DIR}"
  COMMAND
    "${CMAKE_LINKER}"
    -T "${KERNEL_LIMINE_LINKER_SCRIPT}"
    "--defsym=OS1_KERNEL_SHIM_VIRTUAL_BASE=${OS1_KERNEL_SHIM_VIRTUAL_BASE}"
    -z max-page-size=0x1000
    $<TARGET_OBJECTS:kernel_limine_frontend>
    -o "${OS1_KERNEL_LIMINE_ELF}"
  DEPENDS
    kernel_limine_frontend
    $<TARGET_OBJECTS:kernel_limine_frontend>
    "${KERNEL_LIMINE_LINKER_SCRIPT}"
  COMMAND_EXPAND_LISTS
  VERBATIM
)

add_custom_command(
  OUTPUT "${OS1_CPU_START_BIN}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ARTIFACT_DIR}"
  COMMAND "${OS1_NASM_EXECUTABLE}" -fbin
    -I "${OS1_GENERATED_DIR}/"
    -I "${KERNEL_ARCH_DIR}/include/"
    -I "${CMAKE_CURRENT_SOURCE_DIR}/handoff/"
    -I "${CMAKE_CURRENT_SOURCE_DIR}/proc/"
    -I "${CMAKE_CURRENT_SOURCE_DIR}/"
    "${KERNEL_ARCH_DIR}/asm/cpu_start.asm"
    -o "${OS1_CPU_START_BIN}"
  DEPENDS ${KERNEL_ASM_DEPENDENCIES}
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM
)

# os1 EFI pre-loader (\EFI\BOOT\BOOTX64.EFI on the USB stick): saves the
# previous boot's RAM log to the stick, then chain-loads Limine. The ELF is
# position-independent and wrapped into PE32+ by elf_to_pe.py.
set(OS1_EFI_PRELOADER_LINKER_SCRIPT "${PROJECT_SOURCE_DIR}/src/boot/efi/preloader.ld")
set(OS1_EFI_PRELOADER_ELF "${OS1_ARTIFACT_DIR}/os1-preloader.elf")
add_library(os1_efi_preloader_objects OBJECT "${PROJECT_SOURCE_DIR}/src/boot/efi/preloader.cpp")
target_include_directories(os1_efi_preloader_objects PRIVATE
  "${PROJECT_SOURCE_DIR}/src/common"
  "${OS1_GENERATED_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${KERNEL_ARCH_DIR}"
  "${PROJECT_SOURCE_DIR}/src/libc"
  "${PROJECT_SOURCE_DIR}/src/uapi"
)
set_target_properties(os1_efi_preloader_objects PROPERTIES CXX_STANDARD 20 CXX_EXTENSIONS ON)
target_compile_options(
  os1_efi_preloader_objects
  PRIVATE
    -ffreestanding
    -fpie
    -fvisibility=hidden
    -mno-red-zone
    -march=x86-64
    -mgeneral-regs-only
    -fno-stack-protector
    -O2
    -Wall
    -Wextra
    -fno-asynchronous-unwind-tables
    -fno-unwind-tables
    -fno-exceptions
    -fno-rtti
)
add_custom_command(
  OUTPUT "${OS1_EFI_PRELOADER_ELF}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ARTIFACT_DIR}"
  COMMAND
    "${CMAKE_LINKER}"
    -T "${OS1_EFI_PRELOADER_LINKER_SCRIPT}"
    -static
    --emit-relocs
    -z max-page-size=0x1000
    $<TARGET_OBJECTS:os1_efi_preloader_objects>
    -o "${OS1_EFI_PRELOADER_ELF}"
  DEPENDS
    os1_efi_preloader_objects
    $<TARGET_OBJECTS:os1_efi_preloader_objects>
    "${OS1_EFI_PRELOADER_LINKER_SCRIPT}"
  COMMAND_EXPAND_LISTS
  VERBATIM
)
add_custom_command(
  OUTPUT "${OS1_EFI_PRELOADER}"
  COMMAND
    "${OS1_PYTHON_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/cmake/scripts/elf_to_pe.py"
    "${OS1_EFI_PRELOADER_ELF}" "${OS1_EFI_PRELOADER}"
  DEPENDS "${OS1_EFI_PRELOADER_ELF}" "${PROJECT_SOURCE_DIR}/cmake/scripts/elf_to_pe.py"
  VERBATIM
)
add_custom_target(os1_efi_preloader ALL DEPENDS "${OS1_EFI_PRELOADER}")

add_custom_target(kernel_elf DEPENDS "${OS1_KERNEL_ELF}")
add_custom_target(kernel_limine_elf DEPENDS "${OS1_KERNEL_LIMINE_ELF}")
add_custom_target(cpu_start_bin DEPENDS "${OS1_CPU_START_BIN}")
