cmake_minimum_required(VERSION 3.20)

set(OS1_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/cmake/toolchains/x86_64-elf.cmake")

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(OS1_CROSS_COMPILER_REQUESTED FALSE)

  foreach(OS1_COMPILER_VAR CMAKE_C_COMPILER CMAKE_CXX_COMPILER)
    if(DEFINED ${OS1_COMPILER_VAR} AND "${${OS1_COMPILER_VAR}}" MATCHES "(^|/)x86_64-elf-g(cc|\\+\\+)$")
      set(OS1_CROSS_COMPILER_REQUESTED TRUE)
    endif()
  endforeach()

  if(OS1_CROSS_COMPILER_REQUESTED)
    include("${OS1_TOOLCHAIN_FILE}")
  endif()
endif()

project(os1 LANGUAGES C CXX)

option(OS1_KMEM_DEBUG "Enable kernel small-object allocator debug instrumentation" ON)
option(OS1_BOOT_DIAGNOSTICS "Enable USB pre-loader and BIOS boot-log diagnostics" OFF)
option(OS1_QEMU_DEBUGCON "Mirror kernel output to QEMU port 0xE9 for no-UART tests" OFF)
set(OS1_BOOT_DIAGNOSTICS_ENABLED 0)
if(OS1_BOOT_DIAGNOSTICS)
  set(OS1_BOOT_DIAGNOSTICS_ENABLED 1)
endif()

if(NOT CMAKE_C_COMPILER MATCHES "(^|/)x86_64-elf-gcc$")
  message(FATAL_ERROR "os1 requires the x86_64-elf cross toolchain. Configure with -DCMAKE_TOOLCHAIN_FILE=${OS1_TOOLCHAIN_FILE} or use the provided CMake preset.")
endif()

if(NOT CMAKE_CXX_COMPILER MATCHES "(^|/)x86_64-elf-g\\+\\+$")
  message(FATAL_ERROR "os1 requires x86_64-elf-g++. Configure with -DCMAKE_TOOLCHAIN_FILE=${OS1_TOOLCHAIN_FILE} or use the provided CMake preset.")
endif()

foreach(OS1_LANG C CXX)
  foreach(OS1_CONFIG DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_${OS1_LANG}_FLAGS_${OS1_CONFIG} "" CACHE STRING "Disabled for os1 cross-builds" FORCE)
  endforeach()
endforeach()

function(os1_detect_existing_file out_var)
  foreach(candidate ${ARGN})
    if(candidate AND EXISTS "${candidate}")
      set(${out_var} "${candidate}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${out_var} "" PARENT_SCOPE)
endfunction()

set(OS1_ARTIFACT_DIR "${CMAKE_BINARY_DIR}/artifacts")
set(OS1_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(OS1_ISO_STAGE_DIR "${CMAKE_BINARY_DIR}/iso-root")
set(OS1_TARGET_MENU_MANIFEST "${OS1_GENERATED_DIR}/target_menu.txt")

function(os1_assert_menu_manifest_field field_name field_value)
  string(FIND "${field_value}" "|" field_separator_index)
  if(NOT field_separator_index EQUAL -1)
    message(FATAL_ERROR
      "Target menu ${field_name} must not contain '|': ${field_value}"
    )
  endif()

  string(FIND "${field_value}" "\n" field_newline_index)
  if(NOT field_newline_index EQUAL -1)
    message(FATAL_ERROR
      "Target menu ${field_name} must be single-line text: ${field_value}"
    )
  endif()
endfunction()

function(os1_register_menu_target target_name title description)
  if(NOT TARGET "${target_name}")
    message(FATAL_ERROR "Cannot register menu metadata for missing target: ${target_name}")
  endif()

  get_target_property(unavailable_reason "${target_name}" OS1_TARGET_UNAVAILABLE_REASON)
  if(unavailable_reason MATCHES "-NOTFOUND$")
    set(unavailable_reason "")
  endif()

  os1_assert_menu_manifest_field("target name" "${target_name}")
  os1_assert_menu_manifest_field("title" "${title}")
  os1_assert_menu_manifest_field("description" "${description}")
  os1_assert_menu_manifest_field("unavailable reason" "${unavailable_reason}")

  get_property(has_entries GLOBAL PROPERTY OS1_TARGET_MENU_ENTRIES SET)
  if(has_entries)
    get_property(entries GLOBAL PROPERTY OS1_TARGET_MENU_ENTRIES)
  else()
    set(entries "")
  endif()

  string(APPEND entries
    "${target_name}|${title}|${description}|${unavailable_reason}\n"
  )
  set_property(GLOBAL PROPERTY OS1_TARGET_MENU_ENTRIES "${entries}")
endfunction()

function(os1_write_target_menu_manifest)
  get_property(has_entries GLOBAL PROPERTY OS1_TARGET_MENU_ENTRIES SET)
  if(has_entries)
    get_property(entries GLOBAL PROPERTY OS1_TARGET_MENU_ENTRIES)
  else()
    set(entries "")
  endif()

  file(MAKE_DIRECTORY "${OS1_GENERATED_DIR}")
  file(WRITE "${OS1_TARGET_MENU_MANIFEST}" "${entries}")
endfunction()

set(OS1_BOOT_BIN "${OS1_ARTIFACT_DIR}/boot.bin")
set(OS1_KERNEL16_BIN "${OS1_ARTIFACT_DIR}/kernel16.bin")
set(OS1_CPU_START_BIN "${OS1_ARTIFACT_DIR}/cpu_start.bin")
set(OS1_KERNEL_ELF "${OS1_ARTIFACT_DIR}/kernel.elf")
set(OS1_KERNEL_BIOS_ELF "${OS1_ARTIFACT_DIR}/kernel_bios.elf")
set(OS1_KERNEL_LIMINE_ELF "${OS1_ARTIFACT_DIR}/kernel_limine.elf")
set(OS1_EFI_PRELOADER "${OS1_ARTIFACT_DIR}/OS1BOOTX64.EFI")
set(OS1_INITRD_CPIO "${OS1_ARTIFACT_DIR}/initrd.cpio")
set(OS1_RAW_IMAGE "${OS1_ARTIFACT_DIR}/os1.raw")
set(OS1_ISO_IMAGE "${OS1_ARTIFACT_DIR}/os1.iso")
set(OS1_VIRTIO_TEST_DISK "${OS1_ARTIFACT_DIR}/virtio-test-disk.raw")
set(OS1_SERIAL_LOG "${OS1_ARTIFACT_DIR}/os1.log")
set(OS1_BIOS_SERIAL_LOG "${OS1_ARTIFACT_DIR}/os1-bios.log")
set(OS1_OVMF_VARS_FILE "${OS1_ARTIFACT_DIR}/ovmf-vars.fd")
set(OS1_SMOKE_LOG "${OS1_ARTIFACT_DIR}/smoke.log")
set(OS1_SMOKE_BIOS_LOG "${OS1_ARTIFACT_DIR}/smoke-bios.log")
set(OS1_SMOKE_XHCI_LOG "${OS1_ARTIFACT_DIR}/smoke-xhci.log")
set(OS1_SMOKE_OBSERVE_LOG "${OS1_ARTIFACT_DIR}/smoke-observe.log")
set(OS1_SMOKE_OBSERVE_BIOS_LOG "${OS1_ARTIFACT_DIR}/smoke-observe-bios.log")
set(OS1_SMOKE_BALANCE_LOG "${OS1_ARTIFACT_DIR}/smoke-balance.log")
set(OS1_SMOKE_BALANCE_BIOS_LOG "${OS1_ARTIFACT_DIR}/smoke-balance-bios.log")
set(OS1_SMOKE_SPAWN_LOG "${OS1_ARTIFACT_DIR}/smoke-spawn.log")
set(OS1_SMOKE_SPAWN_BIOS_LOG "${OS1_ARTIFACT_DIR}/smoke-spawn-bios.log")
set(OS1_SMOKE_EXEC_LOG "${OS1_ARTIFACT_DIR}/smoke-exec.log")
set(OS1_SMOKE_EXEC_BIOS_LOG "${OS1_ARTIFACT_DIR}/smoke-exec-bios.log")
set(OS1_DUMP_ASM "${OS1_ARTIFACT_DIR}/dump.asm")
set(OS1_CPU_START_DUMP "${OS1_ARTIFACT_DIR}/cpu_start.asm")

set(OS1_LIMINE_DIR "${PROJECT_SOURCE_DIR}/third_party/limine/v11.4.0")
set(OS1_LIMINE_BOOTX64 "${OS1_LIMINE_DIR}/BOOTX64.EFI")
set(OS1_LIMINE_UEFI_CD "${OS1_LIMINE_DIR}/limine-uefi-cd.bin")
set(OS1_LIMINE_PROTOCOL_HEADER "${OS1_LIMINE_DIR}/limine.h")
set(OS1_LIMINE_CONFIG_SOURCE "${PROJECT_SOURCE_DIR}/src/boot/limine/limine.conf")
# The USB stick offers bring-up variants (boot options) in a Limine menu.
set(OS1_LIMINE_USB_CONFIG_SOURCE "${PROJECT_SOURCE_DIR}/src/boot/limine/limine-usb.conf")
set(OS1_USB_PRELOADER_ARGS)
set(OS1_USB_PRELOADER_DEPENDS)
if(OS1_BOOT_DIAGNOSTICS)
  set(OS1_LIMINE_USB_CONFIG_SOURCE "${PROJECT_SOURCE_DIR}/src/boot/limine/limine-usb-diagnostic.conf")
  set(OS1_USB_PRELOADER_ARGS --preloader "${OS1_EFI_PRELOADER}")
  set(OS1_USB_PRELOADER_DEPENDS "${OS1_EFI_PRELOADER}" os1_efi_preloader)
endif()

foreach(OS1_VENDOR_FILE
    "${OS1_LIMINE_BOOTX64}"
    "${OS1_LIMINE_UEFI_CD}"
    "${OS1_LIMINE_PROTOCOL_HEADER}"
    "${OS1_LIMINE_CONFIG_SOURCE}")
  if(NOT EXISTS "${OS1_VENDOR_FILE}")
    message(FATAL_ERROR "Missing required vendored Limine input: ${OS1_VENDOR_FILE}")
  endif()
endforeach()

set(OS1_KERNEL_IMAGE_LOAD_ADDRESS 0x10000)
set(OS1_INITRD_LOAD_ADDRESS 0x90000)
set(OS1_KERNEL_RESERVED_PHYSICAL_START 0x100000)
set(OS1_KERNEL_RESERVED_PHYSICAL_END 0x192000)
set(OS1_KERNEL_VIRTUAL_OFFSET 0xFFFFFFFF80000000)
set(OS1_KERNEL_SHIM_VIRTUAL_BASE 0xFFFFFFFF90000000)
set(OS1_DIRECT_MAP_BASE 0xFFFF800000000000)
math(EXPR OS1_KERNEL_RESERVED_PHYSICAL_BYTES "${OS1_KERNEL_RESERVED_PHYSICAL_END} - ${OS1_KERNEL_RESERVED_PHYSICAL_START}")
set(OS1_KERNEL_POST_IMAGE_RESERVE_BYTES 0x4000)
math(EXPR OS1_KERNEL_IMAGE_STAGING_BUFFER_BYTES "${OS1_INITRD_LOAD_ADDRESS} - ${OS1_KERNEL_IMAGE_LOAD_ADDRESS}")
math(EXPR OS1_KERNEL_RESERVED_PHYSICAL_BYTES_REMAINDER "${OS1_KERNEL_RESERVED_PHYSICAL_BYTES} % 512")
if(NOT OS1_KERNEL_RESERVED_PHYSICAL_BYTES_REMAINDER EQUAL 0)
  message(FATAL_ERROR "Kernel reserved physical window must be sector-aligned for the BIOS raw image slot")
endif()

set(OS1_LOADER16_IMAGE_START_LBA 1)
set(OS1_LOADER16_IMAGE_SECTOR_COUNT 64)
math(EXPR OS1_KERNEL_IMAGE_START_LBA "${OS1_LOADER16_IMAGE_START_LBA} + ${OS1_LOADER16_IMAGE_SECTOR_COUNT}")
# The BIOS raw-image kernel slot is constrained by the low-memory staging buffer
# between kKernelImageLoadAddress and kInitrdLoadAddress. The build separately
# asserts that the stripped BIOS ELF fits this slot and that the PT_LOAD range
# plus the post-image reserve fit inside the higher-half reserved kernel window.
math(EXPR OS1_KERNEL_IMAGE_SECTOR_COUNT "${OS1_KERNEL_IMAGE_STAGING_BUFFER_BYTES} / 512")
math(EXPR OS1_INITRD_IMAGE_START_LBA "${OS1_KERNEL_IMAGE_START_LBA} + ${OS1_KERNEL_IMAGE_SECTOR_COUNT}")
set(OS1_INITRD_IMAGE_SECTOR_COUNT 128)
# Real-hardware boot log area on the BIOS raw image. The loader saves the
# previous boot's RAM log here before booting again. Everything stays below LBA
# 2880 so the area remains reachable even when firmware emulates a USB stick as
# a 1.44 MB floppy. See doc/2026-11-02-real-hardware-bootlog.md.
set(OS1_BOOTLOG_JOURNAL_LBA 1280)
set(OS1_BOOTLOG_SLOT_BASE_LBA 1288)
set(OS1_BOOTLOG_SLOT_SECTORS 392)
set(OS1_BOOTLOG_SLOT_COUNT 4)
set(OS1_RAW_IMAGE_SECTOR_COUNT 2880)
math(EXPR OS1_INITRD_IMAGE_END_LBA "${OS1_INITRD_IMAGE_START_LBA} + ${OS1_INITRD_IMAGE_SECTOR_COUNT}")
math(EXPR OS1_BOOTLOG_AREA_END_LBA "${OS1_BOOTLOG_SLOT_BASE_LBA} + ${OS1_BOOTLOG_SLOT_COUNT} * ${OS1_BOOTLOG_SLOT_SECTORS}")
if(OS1_INITRD_IMAGE_END_LBA GREATER OS1_BOOTLOG_JOURNAL_LBA OR
   NOT OS1_BOOTLOG_SLOT_BASE_LBA GREATER OS1_BOOTLOG_JOURNAL_LBA OR
   OS1_BOOTLOG_AREA_END_LBA GREATER OS1_RAW_IMAGE_SECTOR_COUNT)
  message(FATAL_ERROR "BIOS boot log area overlaps the initrd slot or exceeds the raw image")
endif()
math(EXPR OS1_KERNEL_IMAGE_MAX_BYTES "${OS1_KERNEL_IMAGE_SECTOR_COUNT} * 512")
math(EXPR OS1_INITRD_IMAGE_MAX_BYTES "${OS1_INITRD_IMAGE_SECTOR_COUNT} * 512")

set(OS1_ASSERT_MAX_FILE_SIZE_SCRIPT "${PROJECT_SOURCE_DIR}/cmake/scripts/assert_max_file_size.cmake")
set(OS1_ASSERT_KERNEL_BOOT_CONTRACT_SCRIPT "${PROJECT_SOURCE_DIR}/cmake/scripts/assert_kernel_boot_contract.py")
set(OS1_ASSERT_LIMINE_SHIM_CONTRACT_SCRIPT "${PROJECT_SOURCE_DIR}/cmake/scripts/assert_limine_shim_contract.py")
set(OS1_BIOS_LAYOUT_CHECK_STAMP "${OS1_ARTIFACT_DIR}/bios-layout-check.stamp")
set(OS1_LIMINE_SHIM_LAYOUT_CHECK_STAMP "${OS1_ARTIFACT_DIR}/limine-shim-layout-check.stamp")

set(OS1_IMAGE_LAYOUT_INC "${OS1_GENERATED_DIR}/image_layout.inc")
set(OS1_KERNEL_LAYOUT_INC "${OS1_GENERATED_DIR}/kernel_layout.inc")
set(OS1_KERNEL_LAYOUT_HPP "${OS1_GENERATED_DIR}/kernel_layout.hpp")
set(OS1_INITRD_PAYLOAD_LAYOUT_INC "${OS1_GENERATED_DIR}/initrd_payload.inc")
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/templates/image_layout.inc.in"
  "${OS1_IMAGE_LAYOUT_INC}"
  @ONLY
)
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/templates/kernel_layout.inc.in"
  "${OS1_KERNEL_LAYOUT_INC}"
  @ONLY
)
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/templates/kernel_layout.hpp.in"
  "${OS1_KERNEL_LAYOUT_HPP}"
  @ONLY
)

add_custom_command(
  OUTPUT "${OS1_INITRD_PAYLOAD_LAYOUT_INC}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_GENERATED_DIR}"
  COMMAND
    "${CMAKE_COMMAND}"
    "-DINPUT_FILE=${OS1_INITRD_CPIO}"
    "-DOUTPUT_FILE=${OS1_INITRD_PAYLOAD_LAYOUT_INC}"
    -P
    "${PROJECT_SOURCE_DIR}/cmake/scripts/write_initrd_payload_layout.cmake"
  DEPENDS
    "${OS1_INITRD_CPIO}"
    "${PROJECT_SOURCE_DIR}/cmake/scripts/write_initrd_payload_layout.cmake"
  VERBATIM
)

add_custom_target(os1_initrd_payload_layout DEPENDS "${OS1_INITRD_PAYLOAD_LAYOUT_INC}")

if(OS1_KERNEL_IMAGE_MAX_BYTES GREATER OS1_KERNEL_IMAGE_STAGING_BUFFER_BYTES)
  message(FATAL_ERROR
    "Kernel BIOS slot exceeds the low-memory staging buffer between kernel_image and initrd_image"
  )
endif()

find_program(OS1_NASM_EXECUTABLE NAMES nasm REQUIRED)
find_program(OS1_DD_EXECUTABLE NAMES dd REQUIRED)
find_program(OS1_CPIO_EXECUTABLE NAMES cpio REQUIRED)
find_program(OS1_XORRISO_EXECUTABLE NAMES xorriso REQUIRED)
find_program(OS1_QEMU_EXECUTABLE NAMES qemu-system-x86_64)
find_program(OS1_OBJDUMP_EXECUTABLE NAMES x86_64-elf-objdump objdump)
find_program(OS1_STRIP_EXECUTABLE NAMES x86_64-elf-strip strip REQUIRED)
find_program(OS1_PYTHON_EXECUTABLE NAMES python3 python REQUIRED)

# Wrapped developer tools such as Snap can inject incompatible runtime linker
# and GTK plugin paths into child processes. Scrub those variables before
# launching host-side QEMU so run targets behave consistently.
set(OS1_QEMU_HOST_COMMAND
  "${CMAKE_COMMAND}"
  -E
  env
  --unset=LD_LIBRARY_PATH
  --unset=LD_PRELOAD
  --unset=GIO_MODULE_DIR
  --unset=GTK_EXE_PREFIX
  --unset=GTK_IM_MODULE_FILE
  --unset=GTK_MODULES
  --unset=GTK_PATH
  --unset=XDG_DATA_DIRS
  --unset=XDG_DATA_HOME
)

set(OS1_SMOKE_RUNNER "${PROJECT_SOURCE_DIR}/cmake/scripts/run_smoke.py")
set(OS1_VIRTIO_TEST_DISK_SCRIPT "${PROJECT_SOURCE_DIR}/cmake/scripts/create_virtio_test_disk.cmake")

# OVMF/edk2 firmware discovery is centralized here so the kernel build is the
# single source of truth. CI workflows and developer scripts must not re-implement
# this candidate list — read the resulting OS1_OVMF_* cache entries instead.
# Vendor layout notes:
#   - Ubuntu 'ovmf' apt package: /usr/share/OVMF/OVMF_{CODE,VARS}[_4M].fd
#   - Fedora 'edk2-ovmf' rpm:    /usr/share/edk2/ovmf/OVMF_{CODE,VARS}[_4M].fd
#   - Arch 'edk2-ovmf' pkg:      /usr/share/OVMF/x64/OVMF_{CODE,VARS}.4m.fd (not yet listed)
#   - Homebrew qemu (9+):        $PREFIX/share/qemu/edk2-x86_64-code.fd
#                                $PREFIX/share/qemu/edk2-i386-vars.fd  (x86_64 build shares i386 VARS)
set(OS1_OVMF_CODE "${OS1_OVMF_CODE}" CACHE FILEPATH "Path to the OVMF/edk2 code firmware image used for UEFI run and smoke targets")
set(OS1_OVMF_VARS_TEMPLATE "${OS1_OVMF_VARS_TEMPLATE}" CACHE FILEPATH "Path to the OVMF/edk2 vars template used for UEFI run and smoke targets")

if(OS1_QEMU_EXECUTABLE)
  get_filename_component(OS1_QEMU_BIN_DIR "${OS1_QEMU_EXECUTABLE}" DIRECTORY)
  get_filename_component(OS1_QEMU_PREFIX "${OS1_QEMU_BIN_DIR}/.." ABSOLUTE)

  if(NOT OS1_OVMF_CODE)
    os1_detect_existing_file(OS1_OVMF_CODE_DETECTED
      "${OS1_QEMU_PREFIX}/share/qemu/edk2-x86_64-code.fd"
      "${OS1_QEMU_PREFIX}/share/qemu/OVMF_CODE.fd"
      "/usr/share/OVMF/OVMF_CODE.fd"
      "/usr/share/OVMF/OVMF_CODE_4M.fd"
      "/usr/share/edk2/ovmf/OVMF_CODE.fd"
      "/usr/share/edk2/ovmf/OVMF_CODE_4M.fd"
      "/usr/share/qemu/OVMF_CODE.fd"
      "/usr/share/qemu/edk2-x86_64-code.fd"
    )
    if(OS1_OVMF_CODE_DETECTED)
      set(OS1_OVMF_CODE "${OS1_OVMF_CODE_DETECTED}" CACHE FILEPATH "Path to the OVMF/edk2 code firmware image used for UEFI run and smoke targets" FORCE)
    endif()
  endif()

  if(NOT OS1_OVMF_VARS_TEMPLATE)
    os1_detect_existing_file(OS1_OVMF_VARS_TEMPLATE_DETECTED
      "${OS1_QEMU_PREFIX}/share/qemu/edk2-x86_64-vars.fd"
      "${OS1_QEMU_PREFIX}/share/qemu/edk2-i386-vars.fd"
      "${OS1_QEMU_PREFIX}/share/qemu/OVMF_VARS.fd"
      "/usr/share/OVMF/OVMF_VARS.fd"
      "/usr/share/OVMF/OVMF_VARS_4M.fd"
      "/usr/share/edk2/ovmf/OVMF_VARS.fd"
      "/usr/share/edk2/ovmf/OVMF_VARS_4M.fd"
      "/usr/share/qemu/OVMF_VARS.fd"
      "/usr/share/qemu/edk2-x86_64-vars.fd"
      "/usr/share/qemu/edk2-i386-vars.fd"
    )
    if(OS1_OVMF_VARS_TEMPLATE_DETECTED)
      set(OS1_OVMF_VARS_TEMPLATE "${OS1_OVMF_VARS_TEMPLATE_DETECTED}" CACHE FILEPATH "Path to the OVMF/edk2 vars template used for UEFI run and smoke targets" FORCE)
    endif()
  endif()
endif()

if(OS1_OVMF_CODE)
  message(STATUS "OVMF code firmware:  ${OS1_OVMF_CODE}")
else()
  message(STATUS "OVMF code firmware:  not found (set -DOS1_OVMF_CODE=<path> to override)")
endif()
if(OS1_OVMF_VARS_TEMPLATE)
  message(STATUS "OVMF vars firmware:  ${OS1_OVMF_VARS_TEMPLATE}")
else()
  message(STATUS "OVMF vars firmware:  not found (set -DOS1_OVMF_VARS_TEMPLATE=<path> to override)")
endif()

# Loud, deterministic signal when the default (UEFI) smoke path would silently
# be skipped. Keeping this as WARNING instead of FATAL_ERROR lets a developer
# without OVMF still build and exercise the BIOS compatibility path, but CI
# logs always surface the gap clearly. CI then fails on the missing test count
# below, not here, so the error stays actionable.
if(OS1_QEMU_EXECUTABLE AND (NOT OS1_OVMF_CODE OR NOT OS1_OVMF_VARS_TEMPLATE))
  message(WARNING
    "UEFI smoke path will be SKIPPED: OVMF firmware not detected. "
    "Install edk2/ovmf (Homebrew 'qemu', apt 'ovmf', dnf 'edk2-ovmf') "
    "or set -DOS1_OVMF_CODE=... and -DOS1_OVMF_VARS_TEMPLATE=... "
    "Only the BIOS compatibility smoke will run; CI will fail on missing UEFI coverage."
  )
endif()

add_subdirectory(src/boot)
add_subdirectory(src/kernel)
add_subdirectory(src/user)

add_custom_command(
  OUTPUT "${OS1_VIRTIO_TEST_DISK}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ARTIFACT_DIR}"
  COMMAND
    "${CMAKE_COMMAND}"
    "-DOUTPUT_FILE=${OS1_VIRTIO_TEST_DISK}"
    -P
    "${OS1_VIRTIO_TEST_DISK_SCRIPT}"
  DEPENDS "${OS1_VIRTIO_TEST_DISK_SCRIPT}"
  VERBATIM
)

add_custom_target(os1_virtio_test_disk DEPENDS "${OS1_VIRTIO_TEST_DISK}")

add_custom_command(
  OUTPUT "${OS1_BIOS_LAYOUT_CHECK_STAMP}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ARTIFACT_DIR}"
  COMMAND
    "${OS1_PYTHON_EXECUTABLE}"
    "${OS1_ASSERT_KERNEL_BOOT_CONTRACT_SCRIPT}"
    --elf
    "${OS1_KERNEL_BIOS_ELF}"
    --disk-slot-bytes
    "${OS1_KERNEL_IMAGE_MAX_BYTES}"
    --staging-buffer-bytes
    "${OS1_KERNEL_IMAGE_STAGING_BUFFER_BYTES}"
    --reserved-start
    "${OS1_KERNEL_RESERVED_PHYSICAL_START}"
    --reserved-end
    "${OS1_KERNEL_RESERVED_PHYSICAL_END}"
    --post-image-reserve-bytes
    "${OS1_KERNEL_POST_IMAGE_RESERVE_BYTES}"
    --kernel-virtual-offset
    "${OS1_KERNEL_VIRTUAL_OFFSET}"
  COMMAND
    "${CMAKE_COMMAND}"
    "-DINPUT_FILE=${OS1_INITRD_CPIO}"
    "-DMAX_BYTES=${OS1_INITRD_IMAGE_MAX_BYTES}"
    "-DSECTOR_COUNT=${OS1_INITRD_IMAGE_SECTOR_COUNT}"
    "-DLABEL=initrd.cpio BIOS slot"
    "-DVARIABLE_NAME=OS1_INITRD_IMAGE_SECTOR_COUNT"
    "-DCONFIG_FILE=${PROJECT_SOURCE_DIR}/CMakeLists.txt"
    -P
    "${OS1_ASSERT_MAX_FILE_SIZE_SCRIPT}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${OS1_BIOS_LAYOUT_CHECK_STAMP}"
  DEPENDS
    "${OS1_KERNEL_BIOS_ELF}"
    "${OS1_INITRD_CPIO}"
    "${OS1_ASSERT_KERNEL_BOOT_CONTRACT_SCRIPT}"
    "${OS1_ASSERT_MAX_FILE_SIZE_SCRIPT}"
  VERBATIM
)

add_custom_target(os1_bios_layout_check DEPENDS "${OS1_BIOS_LAYOUT_CHECK_STAMP}")

add_custom_command(
  OUTPUT "${OS1_KERNEL_BIOS_ELF}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ARTIFACT_DIR}"
  COMMAND "${OS1_STRIP_EXECUTABLE}" -o "${OS1_KERNEL_BIOS_ELF}" --strip-unneeded "${OS1_KERNEL_ELF}"
  DEPENDS
    "${OS1_KERNEL_ELF}"
  VERBATIM
)

add_custom_command(
  OUTPUT "${OS1_LIMINE_SHIM_LAYOUT_CHECK_STAMP}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ARTIFACT_DIR}"
  COMMAND
    "${OS1_PYTHON_EXECUTABLE}"
    "${OS1_ASSERT_LIMINE_SHIM_CONTRACT_SCRIPT}"
    --elf
    "${OS1_KERNEL_LIMINE_ELF}"
    --expected-base
    "${OS1_KERNEL_SHIM_VIRTUAL_BASE}"
    --kernel-virtual-offset
    "${OS1_KERNEL_VIRTUAL_OFFSET}"
    --kernel-reserved-start
    "${OS1_KERNEL_RESERVED_PHYSICAL_START}"
    --kernel-reserved-end
    "${OS1_KERNEL_RESERVED_PHYSICAL_END}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${OS1_LIMINE_SHIM_LAYOUT_CHECK_STAMP}"
  DEPENDS
    "${OS1_KERNEL_LIMINE_ELF}"
    "${OS1_ASSERT_LIMINE_SHIM_CONTRACT_SCRIPT}"
  VERBATIM
)

add_custom_target(os1_limine_shim_layout_check DEPENDS "${OS1_LIMINE_SHIM_LAYOUT_CHECK_STAMP}")

add_custom_command(
  OUTPUT "${OS1_RAW_IMAGE}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ARTIFACT_DIR}"
  COMMAND "${OS1_DD_EXECUTABLE}" if=/dev/zero "of=${OS1_RAW_IMAGE}" bs=512 "count=${OS1_RAW_IMAGE_SECTOR_COUNT}"
  COMMAND "${OS1_DD_EXECUTABLE}" "if=${OS1_BOOT_BIN}" "of=${OS1_RAW_IMAGE}" bs=512 count=1 conv=notrunc,sync
  COMMAND
    "${OS1_DD_EXECUTABLE}"
    "if=${OS1_KERNEL16_BIN}"
    "of=${OS1_RAW_IMAGE}"
    "seek=${OS1_LOADER16_IMAGE_START_LBA}"
    "count=${OS1_LOADER16_IMAGE_SECTOR_COUNT}"
    conv=notrunc,sync
  COMMAND
    "${OS1_DD_EXECUTABLE}"
    "if=${OS1_KERNEL_BIOS_ELF}"
    "of=${OS1_RAW_IMAGE}"
    bs=512
    "seek=${OS1_KERNEL_IMAGE_START_LBA}"
    "count=${OS1_KERNEL_IMAGE_SECTOR_COUNT}"
    conv=notrunc,sync
  COMMAND
    "${OS1_DD_EXECUTABLE}"
    "if=${OS1_INITRD_CPIO}"
    "of=${OS1_RAW_IMAGE}"
    bs=512
    "seek=${OS1_INITRD_IMAGE_START_LBA}"
    "count=${OS1_INITRD_IMAGE_SECTOR_COUNT}"
    conv=notrunc,sync
  DEPENDS
    "${OS1_BOOT_BIN}"
    "${OS1_KERNEL16_BIN}"
    "${OS1_KERNEL_BIOS_ELF}"
    "${OS1_INITRD_CPIO}"
    "${OS1_BIOS_LAYOUT_CHECK_STAMP}"
  VERBATIM
)

add_custom_command(
  OUTPUT "${OS1_ISO_IMAGE}"
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${OS1_ISO_STAGE_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ISO_STAGE_DIR}/EFI/BOOT"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ISO_STAGE_DIR}/boot/limine"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ARTIFACT_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_LIMINE_BOOTX64}" "${OS1_ISO_STAGE_DIR}/EFI/BOOT/BOOTX64.EFI"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_LIMINE_CONFIG_SOURCE}" "${OS1_ISO_STAGE_DIR}/limine.conf"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_LIMINE_CONFIG_SOURCE}" "${OS1_ISO_STAGE_DIR}/EFI/BOOT/limine.conf"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_LIMINE_UEFI_CD}" "${OS1_ISO_STAGE_DIR}/boot/limine/limine-uefi-cd.bin"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_KERNEL_ELF}" "${OS1_ISO_STAGE_DIR}/kernel.elf"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_KERNEL_LIMINE_ELF}" "${OS1_ISO_STAGE_DIR}/kernel_limine.elf"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_INITRD_CPIO}" "${OS1_ISO_STAGE_DIR}/initrd.cpio"
  COMMAND
    "${OS1_XORRISO_EXECUTABLE}"
    -as
    mkisofs
    -R
    -r
    --efi-boot
    boot/limine/limine-uefi-cd.bin
    -efi-boot-part
    --efi-boot-image
    --protective-msdos-label
    -no-emul-boot
    -o
    "${OS1_ISO_IMAGE}"
    "${OS1_ISO_STAGE_DIR}"
  DEPENDS
    "${OS1_LIMINE_BOOTX64}"
    "${OS1_LIMINE_UEFI_CD}"
    "${OS1_LIMINE_CONFIG_SOURCE}"
    "${OS1_KERNEL_ELF}"
    "${OS1_KERNEL_LIMINE_ELF}"
    "${OS1_INITRD_CPIO}"
    "${OS1_LIMINE_SHIM_LAYOUT_CHECK_STAMP}"
  VERBATIM
)

add_custom_target(os1_image ALL DEPENDS "${OS1_ISO_IMAGE}" os1_virtio_test_disk os1_bios_layout_check)
add_custom_target(os1_bios_image ALL DEPENDS "${OS1_RAW_IMAGE}" os1_virtio_test_disk)

# Hybrid USB stick image: the BIOS raw image (legacy boot + boot log area) plus
# a FAT16 EFI System Partition with Limine, so one stick boots on legacy BIOS
# and UEFI-only machines alike.
set(OS1_USB_IMAGE "${OS1_ARTIFACT_DIR}/os1-usb.img")
set(OS1_USB_ESP_START_LBA 4096)
set(OS1_USB_ESP_SECTORS 65536)
add_custom_command(
  OUTPUT "${OS1_USB_IMAGE}"
  COMMAND
    "${OS1_PYTHON_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/cmake/scripts/create_usb_image.py"
    --raw-image "${OS1_RAW_IMAGE}"
    ${OS1_USB_PRELOADER_ARGS}
    --limine-efi "${OS1_LIMINE_BOOTX64}"
    --limine-conf "${OS1_LIMINE_USB_CONFIG_SOURCE}"
    --kernel-shim "${OS1_KERNEL_LIMINE_ELF}"
    --kernel "${OS1_KERNEL_ELF}"
    --initrd "${OS1_INITRD_CPIO}"
    --esp-start-lba "${OS1_USB_ESP_START_LBA}"
    --esp-sectors "${OS1_USB_ESP_SECTORS}"
    --clear-lba-start "${OS1_BOOTLOG_JOURNAL_LBA}"
    --clear-lba-end "${OS1_RAW_IMAGE_SECTOR_COUNT}"
    --output "${OS1_USB_IMAGE}"
  DEPENDS
    "${OS1_RAW_IMAGE}"
    ${OS1_USB_PRELOADER_DEPENDS}
    "${OS1_LIMINE_BOOTX64}"
    "${OS1_LIMINE_USB_CONFIG_SOURCE}"
    "${OS1_KERNEL_LIMINE_ELF}"
    "${OS1_KERNEL_ELF}"
    "${OS1_INITRD_CPIO}"
    "${OS1_LIMINE_SHIM_LAYOUT_CHECK_STAMP}"
    "${PROJECT_SOURCE_DIR}/cmake/scripts/create_usb_image.py"
  VERBATIM
)
add_custom_target(os1_usb_image ALL DEPENDS "${OS1_USB_IMAGE}")

function(os1_add_unavailable_target target_name reason)
  add_custom_target(
    "${target_name}"
    COMMAND "${CMAKE_COMMAND}" -E echo "${reason}"
    COMMAND "${CMAKE_COMMAND}" -E false
    VERBATIM
  )
  set_property(TARGET "${target_name}" PROPERTY OS1_TARGET_UNAVAILABLE_REASON "${reason}")
endfunction()

if(OS1_QEMU_EXECUTABLE AND OS1_OVMF_CODE AND OS1_OVMF_VARS_TEMPLATE)
  add_custom_target(
    run
    BYPRODUCTS "${OS1_SERIAL_LOG}" "${OS1_OVMF_VARS_FILE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ARTIFACT_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_OVMF_VARS_TEMPLATE}" "${OS1_OVMF_VARS_FILE}"
    COMMAND
      ${OS1_QEMU_HOST_COMMAND}
      "${OS1_QEMU_EXECUTABLE}"
      -machine
      q35
      -smp
      4
      -drive
      "if=pflash,format=raw,readonly=on,file=${OS1_OVMF_CODE}"
      -drive
      "if=pflash,format=raw,file=${OS1_OVMF_VARS_FILE}"
      -cdrom
      "${OS1_ISO_IMAGE}"
      -netdev
      "user,id=os1net"
      -device
      "virtio-net-pci,netdev=os1net,disable-legacy=on"
      -drive
      "if=none,id=virtio_test,format=raw,file=${OS1_VIRTIO_TEST_DISK}"
      -device
      "virtio-blk-pci,drive=virtio_test,disable-legacy=on"
      -serial
      "file:${OS1_SERIAL_LOG}"
      -no-reboot
      -d
      cpu_reset
      -d
      int
    DEPENDS os1_image "${OS1_VIRTIO_TEST_DISK}"
    USES_TERMINAL
    VERBATIM
  )

  add_custom_target(
    run_serial
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ARTIFACT_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_OVMF_VARS_TEMPLATE}" "${OS1_OVMF_VARS_FILE}"
    COMMAND
      ${OS1_QEMU_HOST_COMMAND}
      "${OS1_QEMU_EXECUTABLE}"
      -machine
      q35
      -smp
      4
      -drive
      "if=pflash,format=raw,readonly=on,file=${OS1_OVMF_CODE}"
      -drive
      "if=pflash,format=raw,file=${OS1_OVMF_VARS_FILE}"
      -cdrom
      "${OS1_ISO_IMAGE}"
      -netdev
      "user,id=os1net"
      -device
      "virtio-net-pci,netdev=os1net,disable-legacy=on"
      -drive
      "if=none,id=virtio_test,format=raw,file=${OS1_VIRTIO_TEST_DISK}"
      -device
      "virtio-blk-pci,drive=virtio_test,disable-legacy=on"
      -serial
      stdio
      -display
      none
      -no-reboot
      -no-shutdown
    DEPENDS os1_image "${OS1_VIRTIO_TEST_DISK}"
    USES_TERMINAL
    VERBATIM
  )
else()
  os1_add_unavailable_target(run "run target unavailable: qemu-system-x86_64 or OVMF firmware was not found")
  os1_add_unavailable_target(run_serial "run_serial target unavailable: qemu-system-x86_64 or OVMF firmware was not found")
endif()

os1_register_menu_target(
  run
  "Default modern UEFI path"
  "Boot OVMF on q35 with the generated virtio-blk test disk used by the modern-platform smoke."
)
os1_register_menu_target(
  run_serial
  "Serial-first UEFI shell session"
  "Boot the same OVMF + ISO path, route the guest serial console to this terminal, and disable graphics."
)

if(OS1_QEMU_EXECUTABLE)
  add_custom_target(
    run_bios
    BYPRODUCTS "${OS1_BIOS_SERIAL_LOG}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ARTIFACT_DIR}"
    COMMAND
      ${OS1_QEMU_HOST_COMMAND}
      "${OS1_QEMU_EXECUTABLE}"
      -machine
      q35
      -smp
      4
      -drive
      "format=raw,file=${OS1_RAW_IMAGE}"
      -netdev
      "user,id=os1net"
      -device
      "virtio-net-pci,netdev=os1net,disable-legacy=on"
      -drive
      "if=none,id=virtio_test,format=raw,file=${OS1_VIRTIO_TEST_DISK}"
      -device
      "virtio-blk-pci,drive=virtio_test,disable-legacy=on"
      -serial
      "file:${OS1_BIOS_SERIAL_LOG}"
      -no-reboot
      -d
      cpu_reset
      -d
      int
    DEPENDS os1_bios_image "${OS1_VIRTIO_TEST_DISK}"
    USES_TERMINAL
    VERBATIM
  )

  add_custom_target(
    run_bios_serial
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${OS1_ARTIFACT_DIR}"
    COMMAND
      ${OS1_QEMU_HOST_COMMAND}
      "${OS1_QEMU_EXECUTABLE}"
      -machine
      q35
      -smp
      4
      -drive
      "format=raw,file=${OS1_RAW_IMAGE}"
      -netdev
      "user,id=os1net"
      -device
      "virtio-net-pci,netdev=os1net,disable-legacy=on"
      -drive
      "if=none,id=virtio_test,format=raw,file=${OS1_VIRTIO_TEST_DISK}"
      -device
      "virtio-blk-pci,drive=virtio_test,disable-legacy=on"
      -serial
      stdio
      -display
      none
      -no-reboot
      -no-shutdown
    DEPENDS os1_bios_image "${OS1_VIRTIO_TEST_DISK}"
    USES_TERMINAL
    VERBATIM
  )
else()
  os1_add_unavailable_target(run_bios "run_bios target unavailable: qemu-system-x86_64 was not found")
  os1_add_unavailable_target(run_bios_serial "run_bios_serial target unavailable: qemu-system-x86_64 was not found")
endif()

os1_register_menu_target(
  run_bios
  "Legacy BIOS compatibility path"
  "Boot the raw BIOS image on q35 with the same secondary virtio-blk test disk attached."
)
os1_register_menu_target(
  run_bios_serial
  "Serial-first BIOS shell session"
  "Keep the BIOS boot path but route the shell through serial stdio instead of the display-first run target."
)

include(CTest)

# The smoke runner terminates QEMU as soon as every expected marker has been
# seen, so these values are safety ceilings for the slowest realistic CI host
# rather than fixed runtimes. A healthy local run exits in a few seconds. The
# UEFI ceiling is intentionally larger because OVMF + ISO + Limine do far more
# I/O than the direct BIOS path, which matters on shared runners without KVM
# acceleration (GitHub Actions ubuntu-hosted, in particular).
set(OS1_UEFI_SMOKE_TIMEOUT_SECONDS 30)
set(OS1_BIOS_SMOKE_TIMEOUT_SECONDS 30)
set(OS1_SMOKE_SETTLE_AFTER_MARKERS_SECONDS 1)
set(OS1_SMOKE_SEND_DELAY_SECONDS 0.2)
set(OS1_SMOKE_REJECT_MARKERS
  "#GP general protection"
  "#UD invalid opcode"
  "[user/balancecheck] fail"
  "[user/smpcheck] fail"
  "[user/acpiprobe] fail"
)

# The shell prompt does not terminate with a newline, so the line-buffered
# smoke runner observes it when the following command output flushes.
set(OS1_SHELL_INPUT_EVENTS
  "shell prompt ready::help\\n"
  "help echo pid sys ps cpu pci initrd devices irqs resources kmem events exec exit::echo hi\\n"
  "echo: hi::pid\\n"
)

set(OS1_SHELL_OBSERVE_INPUT_EVENTS
  "shell prompt ready::sys\\n"
  "sys boot=::ps\\n"
  "pid tid pstate tstate mode cr3 name::cpu\\n"
  "cpu slot apic bsp booted pid tid::events\\n"
  "events complete::pci\\n"
  "pci 00:03.0 1af4:1042::initrd\\n"
  "initrd complete::devices\\n"
  "devices complete::irqs\\n"
  "irqs complete::resources\\n"
  "resources complete::kmem\\n"
  "kmem complete::acpiprobe\\n"
  "shell spawn acpiprobe ok pid=::smpcheck\\n"
)

set(OS1_SHELL_SPAWN_INPUT_EVENTS
  "shell prompt ready::yield\\n"
  "shell spawn yield ok pid=::fault\\n"
  "shell spawn fault ok pid=::nope\\n"
  "unknown command: nope::copycheck\\n"
)

set(OS1_SHELL_EXEC_INPUT_EVENTS
  "shell prompt ready::exec yield\\n"
  "[user/yield] tick 2::echo after-exec\\n"
)

set(OS1_SHELL_BALANCE_INPUT_EVENTS
  "shell prompt ready::balancecheck\\n"
)

set(OS1_EXEC_SMOKE_REJECT_MARKERS
  ${OS1_SMOKE_REJECT_MARKERS}
  "shell exec returned"
  "echo: after-exec"
)

# Preventive: if CI (or any caller) sets OS1_REQUIRE_UEFI_SMOKE=ON, a missing
# UEFI smoke becomes a configure-time hard error rather than a silent skip.
# GitHub Actions + act both set this via the CI workflow, so a missing OVMF
# firmware cannot regress coverage unnoticed.
option(OS1_REQUIRE_UEFI_SMOKE "Fail configure if the UEFI smoke test cannot be registered" OFF)
if(OS1_REQUIRE_UEFI_SMOKE)
  set(OS1_REQUIRE_UEFI_REASON "")
  if(NOT OS1_QEMU_EXECUTABLE)
    set(OS1_REQUIRE_UEFI_REASON "qemu-system-x86_64 not found")
  elseif(NOT OS1_OVMF_CODE OR NOT EXISTS "${OS1_OVMF_CODE}")
    set(OS1_REQUIRE_UEFI_REASON "OVMF code firmware missing or unreadable: '${OS1_OVMF_CODE}'")
  elseif(NOT OS1_OVMF_VARS_TEMPLATE OR NOT EXISTS "${OS1_OVMF_VARS_TEMPLATE}")
    set(OS1_REQUIRE_UEFI_REASON "OVMF vars template missing or unreadable: '${OS1_OVMF_VARS_TEMPLATE}'")
  endif()
  if(OS1_REQUIRE_UEFI_REASON)
    message(FATAL_ERROR
      "OS1_REQUIRE_UEFI_SMOKE=ON but the UEFI smoke path is unavailable.\n"
      "Reason: ${OS1_REQUIRE_UEFI_REASON}\n"
      "Install edk2/ovmf (Homebrew 'qemu', apt 'ovmf', dnf 'edk2-ovmf') "
      "or override OS1_OVMF_CODE / OS1_OVMF_VARS_TEMPLATE."
    )
  endif()
endif()

set(OS1_QEMU_SMOKE_TESTS)

set(OS1_ACPICA_SMOKE_MARKERS
  "acpica: tables ready"
  "acpica: namespace ready"
  "acpica: methods ready"
)

if(BUILD_TESTING AND OS1_QEMU_EXECUTABLE AND OS1_OVMF_CODE AND OS1_OVMF_VARS_TEMPLATE)
  set(OS1_UEFI_XHCI_SMOKE_MARKERS
    "[kernel64] hello!"
    "boot source: limine"
    ${OS1_ACPICA_SMOKE_MARKERS}
    "virtio-blk smoke ok"
    "virtio-blk threaded smoke ok"
    "virtio-net smoke ok"
    "shell prompt ready"
    "xhci: HID keyboard ready"
  )
  set(OS1_XHCI_QEMU_ARGS
    -device
    qemu-xhci,id=xhci
    -device
    usb-kbd,bus=xhci.0
  )

  add_test(
    NAME os1_smoke_xhci
    COMMAND
      "${CMAKE_COMMAND}"
      "-DQEMU_EXECUTABLE=${OS1_QEMU_EXECUTABLE}"
      "-DISO_IMAGE=${OS1_ISO_IMAGE}"
      "-DOVMF_CODE=${OS1_OVMF_CODE}"
      "-DOVMF_VARS_TEMPLATE=${OS1_OVMF_VARS_TEMPLATE}"
      "-DLOG_FILE=${OS1_SMOKE_XHCI_LOG}"
      "-DSMOKE_TIMEOUT_SECONDS=${OS1_UEFI_SMOKE_TIMEOUT_SECONDS}"
      "-DSMOKE_SETTLE_AFTER_MARKERS_SECONDS=${OS1_SMOKE_SETTLE_AFTER_MARKERS_SECONDS}"
      "-DEXPECTED_MARKERS=${OS1_UEFI_XHCI_SMOKE_MARKERS}"
      "-DREJECTED_MARKERS=${OS1_SMOKE_REJECT_MARKERS}"
      "-DEXTRA_QEMU_ARGS=${OS1_XHCI_QEMU_ARGS}"
      "-DVIRTIO_TEST_DISK=${OS1_VIRTIO_TEST_DISK}"
      "-DPYTHON_EXECUTABLE=${OS1_PYTHON_EXECUTABLE}"
      "-DSMOKE_RUNNER=${OS1_SMOKE_RUNNER}"
      -P
      "${PROJECT_SOURCE_DIR}/cmake/scripts/run_smoke_test.cmake"
  )
  list(APPEND OS1_QEMU_SMOKE_TESTS os1_smoke_xhci)

  add_custom_target(
    smoke_xhci
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -R "^os1_smoke_xhci$"
    DEPENDS os1_image "${OS1_VIRTIO_TEST_DISK}"
    USES_TERMINAL
    VERBATIM
  )
  os1_register_menu_target(
    smoke_xhci
    "Modern UEFI xHCI + USB-keyboard smoke"
    "Run the UEFI-only xHCI and USB-keyboard smoke."
  )

  set(OS1_UEFI_SMOKE_MARKERS
    "[kernel64] hello!"
    "boot source: limine"
    ${OS1_ACPICA_SMOKE_MARKERS}
    "boot rsdp physical=0x"
    "acpi: MADT ready"
    "acpi: MCFG ready"
    "pci: enumerated devices="
    "virtio-blk smoke ok"
    "virtio-blk threaded smoke ok"
    "virtio-net smoke ok"
    "ncpu: 4"
    "framebuffer console active"
    "initrd module discovered"
    "starting first user process"
    "shell prompt ready"
    "help echo pid sys ps cpu pci initrd devices irqs resources kmem events exec exit"
    "echo: hi"
    "pid: "
    "idle thread online"
  )

  add_test(
    NAME os1_smoke
    COMMAND
      "${CMAKE_COMMAND}"
      "-DQEMU_EXECUTABLE=${OS1_QEMU_EXECUTABLE}"
      "-DISO_IMAGE=${OS1_ISO_IMAGE}"
      "-DOVMF_CODE=${OS1_OVMF_CODE}"
      "-DOVMF_VARS_TEMPLATE=${OS1_OVMF_VARS_TEMPLATE}"
      "-DLOG_FILE=${OS1_SMOKE_LOG}"
      "-DSMOKE_TIMEOUT_SECONDS=${OS1_UEFI_SMOKE_TIMEOUT_SECONDS}"
      "-DSMOKE_SETTLE_AFTER_MARKERS_SECONDS=${OS1_SMOKE_SETTLE_AFTER_MARKERS_SECONDS}"
      "-DSMOKE_SEND_DELAY_SECONDS=${OS1_SMOKE_SEND_DELAY_SECONDS}"
      "-DEXPECTED_MARKERS=${OS1_UEFI_SMOKE_MARKERS}"
      "-DREJECTED_MARKERS=${OS1_SMOKE_REJECT_MARKERS}"
      "-DSERIAL_INPUT_EVENTS=${OS1_SHELL_INPUT_EVENTS}"
      "-DVIRTIO_TEST_DISK=${OS1_VIRTIO_TEST_DISK}"
      "-DPYTHON_EXECUTABLE=${OS1_PYTHON_EXECUTABLE}"
      "-DSMOKE_RUNNER=${OS1_SMOKE_RUNNER}"
      -P
      "${PROJECT_SOURCE_DIR}/cmake/scripts/run_smoke_test.cmake"
  )
  list(APPEND OS1_QEMU_SMOKE_TESTS os1_smoke)

  add_custom_target(
    smoke
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -R "^os1_smoke$"
    DEPENDS os1_image "${OS1_VIRTIO_TEST_DISK}"
    USES_TERMINAL
    VERBATIM
  )

  set(OS1_UEFI_OBSERVE_SMOKE_MARKERS
    "[kernel64] hello!"
    "boot source: limine"
    ${OS1_ACPICA_SMOKE_MARKERS}
    "virtio-blk smoke ok"
    "virtio-blk threaded smoke ok"
    "virtio-net smoke ok"
    "framebuffer console active"
    "starting first user process"
    "shell prompt ready"
    "sys boot=limine console=framebuffer"
    "pid tid pstate tstate mode cr3 name"
    "/bin/init"
    "cpu slot apic bsp booted pid tid"
    "cpu 3 03 no yes"
    "pci bdf vendor:device class irq bars"
    "pci 00:03.0 1af4:1042 class=01:00"
    "pci 00:02.0 1af4:1041 class=02:00"
    "initrd path size"
    "initrd /bin/sh size="
    "initrd /bin/yield size="
    "initrd /bin/fault size="
    "initrd /bin/copycheck size="
    "device bus id state pci driver"
    "device pci 3 started 3 virtio-blk"
    "device pci 2 started 2 virtio-net"
    "irq vector kind owner source gsi flags"
    "irq 33 legacy-isa platform:1 irq=1 id=0 gsi=1 flags=0x0"
    "resource kind owner ref detail base size extra"
    "resource bar pci:3 pci=3 bar="
    "resource dma pci:3 dir=bidirectional pages=3 coherent=yes"
    "resources complete"
    "kmem global caches="
    "kmem cache kmalloc-16 obj=16"
    "kmem complete"
    "[user/acpiprobe] route ok"
    "[user/acpiprobe] power "
    "[user/smpcheck] observed pids "
    "event type seq tick cpu pid tid flags arg0 arg1 arg2 arg3"
    "event net-rx seq="
    "event ap-online seq="
    "event ap-tick seq="
    "event kernel-thread-ping seq="
    "event timer-source-lapic seq="
    "event smoke-marker seq="
    "idle thread online"
  )

  add_test(
    NAME os1_smoke_observe
    COMMAND
      "${CMAKE_COMMAND}"
      "-DQEMU_EXECUTABLE=${OS1_QEMU_EXECUTABLE}"
      "-DISO_IMAGE=${OS1_ISO_IMAGE}"
      "-DOVMF_CODE=${OS1_OVMF_CODE}"
      "-DOVMF_VARS_TEMPLATE=${OS1_OVMF_VARS_TEMPLATE}"
      "-DLOG_FILE=${OS1_SMOKE_OBSERVE_LOG}"
      "-DSMOKE_TIMEOUT_SECONDS=${OS1_UEFI_SMOKE_TIMEOUT_SECONDS}"
      "-DSMOKE_SETTLE_AFTER_MARKERS_SECONDS=${OS1_SMOKE_SETTLE_AFTER_MARKERS_SECONDS}"
      "-DSMOKE_SEND_DELAY_SECONDS=${OS1_SMOKE_SEND_DELAY_SECONDS}"
      "-DEXPECTED_MARKERS=${OS1_UEFI_OBSERVE_SMOKE_MARKERS}"
      "-DREJECTED_MARKERS=${OS1_SMOKE_REJECT_MARKERS}"
      "-DSERIAL_INPUT_EVENTS=${OS1_SHELL_OBSERVE_INPUT_EVENTS}"
      "-DVIRTIO_TEST_DISK=${OS1_VIRTIO_TEST_DISK}"
      "-DPYTHON_EXECUTABLE=${OS1_PYTHON_EXECUTABLE}"
      "-DSMOKE_RUNNER=${OS1_SMOKE_RUNNER}"
      -P
      "${PROJECT_SOURCE_DIR}/cmake/scripts/run_smoke_test.cmake"
  )
  list(APPEND OS1_QEMU_SMOKE_TESTS os1_smoke_observe)

  add_custom_target(
    smoke_observe
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -R "^os1_smoke_observe$"
    DEPENDS os1_image "${OS1_VIRTIO_TEST_DISK}"
    USES_TERMINAL
    VERBATIM
  )

  set(OS1_UEFI_BALANCE_SMOKE_MARKERS
    "[kernel64] hello!"
    "boot source: limine"
    ${OS1_ACPICA_SMOKE_MARKERS}
    "virtio-blk smoke ok"
    "virtio-blk threaded smoke ok"
    "virtio-net smoke ok"
    "starting first user process"
    "shell prompt ready"
    "[user/balancecheck] runq delta="
    "idle thread online"
  )

  add_test(
    NAME os1_smoke_balance
    COMMAND
      "${CMAKE_COMMAND}"
      "-DQEMU_EXECUTABLE=${OS1_QEMU_EXECUTABLE}"
      "-DISO_IMAGE=${OS1_ISO_IMAGE}"
      "-DOVMF_CODE=${OS1_OVMF_CODE}"
      "-DOVMF_VARS_TEMPLATE=${OS1_OVMF_VARS_TEMPLATE}"
      "-DLOG_FILE=${OS1_SMOKE_BALANCE_LOG}"
      "-DSMOKE_TIMEOUT_SECONDS=${OS1_UEFI_SMOKE_TIMEOUT_SECONDS}"
      "-DSMOKE_SETTLE_AFTER_MARKERS_SECONDS=${OS1_SMOKE_SETTLE_AFTER_MARKERS_SECONDS}"
      "-DSMOKE_SEND_DELAY_SECONDS=${OS1_SMOKE_SEND_DELAY_SECONDS}"
      "-DEXPECTED_MARKERS=${OS1_UEFI_BALANCE_SMOKE_MARKERS}"
      "-DREJECTED_MARKERS=${OS1_SMOKE_REJECT_MARKERS}"
      "-DSERIAL_INPUT_EVENTS=${OS1_SHELL_BALANCE_INPUT_EVENTS}"
      "-DVIRTIO_TEST_DISK=${OS1_VIRTIO_TEST_DISK}"
      "-DPYTHON_EXECUTABLE=${OS1_PYTHON_EXECUTABLE}"
      "-DSMOKE_RUNNER=${OS1_SMOKE_RUNNER}"
      -P
      "${PROJECT_SOURCE_DIR}/cmake/scripts/run_smoke_test.cmake"
  )
  list(APPEND OS1_QEMU_SMOKE_TESTS os1_smoke_balance)

  add_custom_target(
    smoke_balance
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -R "^os1_smoke_balance$"
    DEPENDS os1_image "${OS1_VIRTIO_TEST_DISK}"
    USES_TERMINAL
    VERBATIM
  )

  set(OS1_UEFI_SPAWN_SMOKE_MARKERS
    "[kernel64] hello!"
    "boot source: limine"
    ${OS1_ACPICA_SMOKE_MARKERS}
    "virtio-blk smoke ok"
    "virtio-blk threaded smoke ok"
    "virtio-net smoke ok"
    "starting first user process"
    "shell prompt ready"
    "[user/yield] tick 2"
    "shell spawn yield ok pid="
    "shell prompt resumed"
    "[user/fault] boom"
    "shell spawn fault ok pid="
    "unknown command: nope"
    "[user/copycheck] ok"
    "shell spawn copycheck ok pid="
    "idle thread online"
  )

  add_test(
    NAME os1_smoke_spawn
    COMMAND
      "${CMAKE_COMMAND}"
      "-DQEMU_EXECUTABLE=${OS1_QEMU_EXECUTABLE}"
      "-DISO_IMAGE=${OS1_ISO_IMAGE}"
      "-DOVMF_CODE=${OS1_OVMF_CODE}"
      "-DOVMF_VARS_TEMPLATE=${OS1_OVMF_VARS_TEMPLATE}"
      "-DLOG_FILE=${OS1_SMOKE_SPAWN_LOG}"
      "-DSMOKE_TIMEOUT_SECONDS=${OS1_UEFI_SMOKE_TIMEOUT_SECONDS}"
      "-DSMOKE_SETTLE_AFTER_MARKERS_SECONDS=${OS1_SMOKE_SETTLE_AFTER_MARKERS_SECONDS}"
      "-DSMOKE_SEND_DELAY_SECONDS=${OS1_SMOKE_SEND_DELAY_SECONDS}"
      "-DEXPECTED_MARKERS=${OS1_UEFI_SPAWN_SMOKE_MARKERS}"
      "-DREJECTED_MARKERS=${OS1_SMOKE_REJECT_MARKERS}"
      "-DSERIAL_INPUT_EVENTS=${OS1_SHELL_SPAWN_INPUT_EVENTS}"
      "-DVIRTIO_TEST_DISK=${OS1_VIRTIO_TEST_DISK}"
      "-DPYTHON_EXECUTABLE=${OS1_PYTHON_EXECUTABLE}"
      "-DSMOKE_RUNNER=${OS1_SMOKE_RUNNER}"
      -P
      "${PROJECT_SOURCE_DIR}/cmake/scripts/run_smoke_test.cmake"
  )
  list(APPEND OS1_QEMU_SMOKE_TESTS os1_smoke_spawn)

  add_custom_target(
    smoke_spawn
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -R "^os1_smoke_spawn$"
    DEPENDS os1_image "${OS1_VIRTIO_TEST_DISK}"
    USES_TERMINAL
    VERBATIM
  )

  set(OS1_UEFI_EXEC_SMOKE_MARKERS
    "[kernel64] hello!"
    "boot source: limine"
    ${OS1_ACPICA_SMOKE_MARKERS}
    "virtio-blk smoke ok"
    "virtio-blk threaded smoke ok"
    "virtio-net smoke ok"
    "starting first user process"
    "shell prompt ready"
    "shell exec start /bin/yield"
    "[user/yield] tick 2"
    "idle thread online"
  )

  add_test(
    NAME os1_smoke_exec
    COMMAND
      "${CMAKE_COMMAND}"
      "-DQEMU_EXECUTABLE=${OS1_QEMU_EXECUTABLE}"
      "-DISO_IMAGE=${OS1_ISO_IMAGE}"
      "-DOVMF_CODE=${OS1_OVMF_CODE}"
      "-DOVMF_VARS_TEMPLATE=${OS1_OVMF_VARS_TEMPLATE}"
      "-DLOG_FILE=${OS1_SMOKE_EXEC_LOG}"
      "-DSMOKE_TIMEOUT_SECONDS=${OS1_UEFI_SMOKE_TIMEOUT_SECONDS}"
      "-DSMOKE_SETTLE_AFTER_MARKERS_SECONDS=${OS1_SMOKE_SETTLE_AFTER_MARKERS_SECONDS}"
      "-DSMOKE_SEND_DELAY_SECONDS=${OS1_SMOKE_SEND_DELAY_SECONDS}"
      "-DEXPECTED_MARKERS=${OS1_UEFI_EXEC_SMOKE_MARKERS}"
      "-DREJECTED_MARKERS=${OS1_EXEC_SMOKE_REJECT_MARKERS}"
      "-DSERIAL_INPUT_EVENTS=${OS1_SHELL_EXEC_INPUT_EVENTS}"
      "-DVIRTIO_TEST_DISK=${OS1_VIRTIO_TEST_DISK}"
      "-DPYTHON_EXECUTABLE=${OS1_PYTHON_EXECUTABLE}"
      "-DSMOKE_RUNNER=${OS1_SMOKE_RUNNER}"
      -P
      "${PROJECT_SOURCE_DIR}/cmake/scripts/run_smoke_test.cmake"
  )
  list(APPEND OS1_QEMU_SMOKE_TESTS os1_smoke_exec)

  add_custom_target(
    smoke_exec
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -R "^os1_smoke_exec$"
    DEPENDS os1_image "${OS1_VIRTIO_TEST_DISK}"
    USES_TERMINAL
    VERBATIM
  )
else()
  os1_add_unavailable_target(smoke "smoke target unavailable: qemu-system-x86_64 or OVMF firmware was not found")
  os1_add_unavailable_target(smoke_observe "smoke_observe target unavailable: qemu-system-x86_64 or OVMF firmware was not found")
  os1_add_unavailable_target(smoke_balance "smoke_balance target unavailable: qemu-system-x86_64 or OVMF firmware was not found")
  os1_add_unavailable_target(smoke_spawn "smoke_spawn target unavailable: qemu-system-x86_64 or OVMF firmware was not found")
  os1_add_unavailable_target(smoke_exec "smoke_exec target unavailable: qemu-system-x86_64 or OVMF firmware was not found")
endif()

os1_register_menu_target(
  smoke
  "Modern UEFI shell baseline smoke"
  "Run the baseline UEFI shell smoke test."
)
os1_register_menu_target(
  smoke_observe
  "Modern UEFI observability smoke"
  "Run the UEFI observability smoke that exercises shell inspection commands."
)
os1_register_menu_target(
  smoke_balance
  "Modern UEFI SMP balance smoke"
  "Run the UEFI SMP balance smoke."
)
os1_register_menu_target(
  smoke_spawn
  "Modern UEFI child-launch smoke"
  "Run the UEFI child-process spawn smoke."
)
os1_register_menu_target(
  smoke_exec
  "Modern UEFI exec smoke"
  "Run the UEFI exec smoke."
)

if(BUILD_TESTING AND OS1_QEMU_EXECUTABLE)
  set(OS1_BIOS_SMOKE_MARKERS
    "[kernel64] hello!"
    "boot source: bios"
    ${OS1_ACPICA_SMOKE_MARKERS}
    "boot rsdp physical=0x"
    "acpi: MADT ready"
    "acpi: MCFG ready"
    "pci: enumerated devices="
    "virtio-blk smoke ok"
    "virtio-blk threaded smoke ok"
    "virtio-net smoke ok"
    "ncpu: 4"
    "Interrupts initialization successful"
    "initrd module discovered"
    "starting first user process"
    "shell prompt ready"
    "help echo pid sys ps cpu pci initrd devices irqs resources kmem events exec exit"
    "echo: hi"
    "pid: "
    "idle thread online"
  )

  add_test(
    NAME os1_smoke_bios
    COMMAND
      "${CMAKE_COMMAND}"
      "-DQEMU_EXECUTABLE=${OS1_QEMU_EXECUTABLE}"
      "-DRAW_IMAGE=${OS1_RAW_IMAGE}"
      "-DLOG_FILE=${OS1_SMOKE_BIOS_LOG}"
      "-DSMOKE_TIMEOUT_SECONDS=${OS1_BIOS_SMOKE_TIMEOUT_SECONDS}"
      "-DSMOKE_SETTLE_AFTER_MARKERS_SECONDS=${OS1_SMOKE_SETTLE_AFTER_MARKERS_SECONDS}"
      "-DSMOKE_SEND_DELAY_SECONDS=${OS1_SMOKE_SEND_DELAY_SECONDS}"
      "-DEXPECTED_MARKERS=${OS1_BIOS_SMOKE_MARKERS}"
      "-DREJECTED_MARKERS=${OS1_SMOKE_REJECT_MARKERS}"
      "-DSERIAL_INPUT_EVENTS=${OS1_SHELL_INPUT_EVENTS}"
      "-DVIRTIO_TEST_DISK=${OS1_VIRTIO_TEST_DISK}"
      "-DPYTHON_EXECUTABLE=${OS1_PYTHON_EXECUTABLE}"
      "-DSMOKE_RUNNER=${OS1_SMOKE_RUNNER}"
      -P
      "${PROJECT_SOURCE_DIR}/cmake/scripts/run_smoke_test.cmake"
  )
  list(APPEND OS1_QEMU_SMOKE_TESTS os1_smoke_bios)

  add_custom_target(
    smoke_bios
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -R "^os1_smoke_bios$"
    DEPENDS os1_bios_image "${OS1_VIRTIO_TEST_DISK}"
    USES_TERMINAL
    VERBATIM
  )

  set(OS1_BIOS_OBSERVE_SMOKE_MARKERS
    "[kernel64] hello!"
    "boot source: bios"
    ${OS1_ACPICA_SMOKE_MARKERS}
    "virtio-blk smoke ok"
    "virtio-blk threaded smoke ok"
    "virtio-net smoke ok"
    "Interrupts initialization successful"
    "starting first user process"
    "shell prompt ready"
    "sys boot=bios console=vga"
    "pid tid pstate tstate mode cr3 name"
    "/bin/init"
    "cpu slot apic bsp booted pid tid"
    "cpu 3 03 no yes"
    "pci bdf vendor:device class irq bars"
    "pci 00:03.0 1af4:1042 class=01:00"
    "pci 00:02.0 1af4:1041 class=02:00"
    "initrd path size"
    "initrd /bin/sh size="
    "initrd /bin/yield size="
    "initrd /bin/fault size="
    "initrd /bin/copycheck size="
    "device bus id state pci driver"
    "device pci 3 started 3 virtio-blk"
    "device pci 2 started 2 virtio-net"
    "irq vector kind owner source gsi flags"
    "irq 33 legacy-isa platform:1 irq=1 id=0 gsi=1 flags=0x0"
    "resource kind owner ref detail base size extra"
    "resource bar pci:3 pci=3 bar="
    "resource dma pci:3 dir=bidirectional pages=3 coherent=yes"
    "resources complete"
    "kmem global caches="
    "kmem cache kmalloc-16 obj=16"
    "kmem complete"
    "[user/acpiprobe] route ok"
    "[user/acpiprobe] power "
    "[user/smpcheck] observed pids "
    "event type seq tick cpu pid tid flags arg0 arg1 arg2 arg3"
    "event net-rx seq="
    "event ap-online seq="
    "event kernel-thread-ping seq="
    "event timer-source-pit seq="
    "event smoke-marker seq="
    "idle thread online"
  )

  add_test(
    NAME os1_smoke_observe_bios
    COMMAND
      "${CMAKE_COMMAND}"
      "-DQEMU_EXECUTABLE=${OS1_QEMU_EXECUTABLE}"
      "-DRAW_IMAGE=${OS1_RAW_IMAGE}"
      "-DLOG_FILE=${OS1_SMOKE_OBSERVE_BIOS_LOG}"
      "-DSMOKE_TIMEOUT_SECONDS=${OS1_BIOS_SMOKE_TIMEOUT_SECONDS}"
      "-DSMOKE_SETTLE_AFTER_MARKERS_SECONDS=${OS1_SMOKE_SETTLE_AFTER_MARKERS_SECONDS}"
      "-DSMOKE_SEND_DELAY_SECONDS=${OS1_SMOKE_SEND_DELAY_SECONDS}"
      "-DEXPECTED_MARKERS=${OS1_BIOS_OBSERVE_SMOKE_MARKERS}"
      "-DREJECTED_MARKERS=${OS1_SMOKE_REJECT_MARKERS}"
      "-DSERIAL_INPUT_EVENTS=${OS1_SHELL_OBSERVE_INPUT_EVENTS}"
      "-DVIRTIO_TEST_DISK=${OS1_VIRTIO_TEST_DISK}"
      "-DPYTHON_EXECUTABLE=${OS1_PYTHON_EXECUTABLE}"
      "-DSMOKE_RUNNER=${OS1_SMOKE_RUNNER}"
      -P
      "${PROJECT_SOURCE_DIR}/cmake/scripts/run_smoke_test.cmake"
  )
  list(APPEND OS1_QEMU_SMOKE_TESTS os1_smoke_observe_bios)

  add_custom_target(
    smoke_observe_bios
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -R "^os1_smoke_observe_bios$"
    DEPENDS os1_bios_image "${OS1_VIRTIO_TEST_DISK}"
    USES_TERMINAL
    VERBATIM
  )

  set(OS1_BIOS_BALANCE_SMOKE_MARKERS
    "[kernel64] hello!"
    "boot source: bios"
    ${OS1_ACPICA_SMOKE_MARKERS}
    "virtio-blk smoke ok"
    "virtio-blk threaded smoke ok"
    "virtio-net smoke ok"
    "starting first user process"
    "shell prompt ready"
    "[user/balancecheck] runq delta="
    "idle thread online"
  )

  add_test(
    NAME os1_smoke_balance_bios
    COMMAND
      "${CMAKE_COMMAND}"
      "-DQEMU_EXECUTABLE=${OS1_QEMU_EXECUTABLE}"
      "-DRAW_IMAGE=${OS1_RAW_IMAGE}"
      "-DLOG_FILE=${OS1_SMOKE_BALANCE_BIOS_LOG}"
      "-DSMOKE_TIMEOUT_SECONDS=${OS1_BIOS_SMOKE_TIMEOUT_SECONDS}"
      "-DSMOKE_SETTLE_AFTER_MARKERS_SECONDS=${OS1_SMOKE_SETTLE_AFTER_MARKERS_SECONDS}"
      "-DSMOKE_SEND_DELAY_SECONDS=${OS1_SMOKE_SEND_DELAY_SECONDS}"
      "-DEXPECTED_MARKERS=${OS1_BIOS_BALANCE_SMOKE_MARKERS}"
      "-DREJECTED_MARKERS=${OS1_SMOKE_REJECT_MARKERS}"
      "-DSERIAL_INPUT_EVENTS=${OS1_SHELL_BALANCE_INPUT_EVENTS}"
      "-DVIRTIO_TEST_DISK=${OS1_VIRTIO_TEST_DISK}"
      "-DPYTHON_EXECUTABLE=${OS1_PYTHON_EXECUTABLE}"
      "-DSMOKE_RUNNER=${OS1_SMOKE_RUNNER}"
      -P
      "${PROJECT_SOURCE_DIR}/cmake/scripts/run_smoke_test.cmake"
  )
  list(APPEND OS1_QEMU_SMOKE_TESTS os1_smoke_balance_bios)

  add_custom_target(
    smoke_balance_bios
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -R "^os1_smoke_balance_bios$"
    DEPENDS os1_bios_image "${OS1_VIRTIO_TEST_DISK}"
    USES_TERMINAL
    VERBATIM
  )

  set(OS1_BIOS_SPAWN_SMOKE_MARKERS
    "[kernel64] hello!"
    "boot source: bios"
    ${OS1_ACPICA_SMOKE_MARKERS}
    "virtio-blk smoke ok"
    "virtio-blk threaded smoke ok"
    "virtio-net smoke ok"
    "starting first user process"
    "shell prompt ready"
    "[user/yield] tick 2"
    "shell spawn yield ok pid="
    "shell prompt resumed"
    "[user/fault] boom"
    "shell spawn fault ok pid="
    "unknown command: nope"
    "[user/copycheck] ok"
    "shell spawn copycheck ok pid="
    "idle thread online"
  )

  add_test(
    NAME os1_smoke_spawn_bios
    COMMAND
      "${CMAKE_COMMAND}"
      "-DQEMU_EXECUTABLE=${OS1_QEMU_EXECUTABLE}"
      "-DRAW_IMAGE=${OS1_RAW_IMAGE}"
      "-DLOG_FILE=${OS1_SMOKE_SPAWN_BIOS_LOG}"
      "-DSMOKE_TIMEOUT_SECONDS=${OS1_BIOS_SMOKE_TIMEOUT_SECONDS}"
      "-DSMOKE_SETTLE_AFTER_MARKERS_SECONDS=${OS1_SMOKE_SETTLE_AFTER_MARKERS_SECONDS}"
      "-DSMOKE_SEND_DELAY_SECONDS=${OS1_SMOKE_SEND_DELAY_SECONDS}"
      "-DEXPECTED_MARKERS=${OS1_BIOS_SPAWN_SMOKE_MARKERS}"
      "-DREJECTED_MARKERS=${OS1_SMOKE_REJECT_MARKERS}"
      "-DSERIAL_INPUT_EVENTS=${OS1_SHELL_SPAWN_INPUT_EVENTS}"
      "-DVIRTIO_TEST_DISK=${OS1_VIRTIO_TEST_DISK}"
      "-DPYTHON_EXECUTABLE=${OS1_PYTHON_EXECUTABLE}"
      "-DSMOKE_RUNNER=${OS1_SMOKE_RUNNER}"
      -P
      "${PROJECT_SOURCE_DIR}/cmake/scripts/run_smoke_test.cmake"
  )
  list(APPEND OS1_QEMU_SMOKE_TESTS os1_smoke_spawn_bios)

  add_custom_target(
    smoke_spawn_bios
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -R "^os1_smoke_spawn_bios$"
    DEPENDS os1_bios_image "${OS1_VIRTIO_TEST_DISK}"
    USES_TERMINAL
    VERBATIM
  )

  set(OS1_BIOS_EXEC_SMOKE_MARKERS
    "[kernel64] hello!"
    "boot source: bios"
    ${OS1_ACPICA_SMOKE_MARKERS}
    "virtio-blk smoke ok"
    "virtio-blk threaded smoke ok"
    "virtio-net smoke ok"
    "starting first user process"
    "shell prompt ready"
    "shell exec start /bin/yield"
    "[user/yield] tick 2"
    "idle thread online"
  )

  add_test(
    NAME os1_smoke_exec_bios
    COMMAND
      "${CMAKE_COMMAND}"
      "-DQEMU_EXECUTABLE=${OS1_QEMU_EXECUTABLE}"
      "-DRAW_IMAGE=${OS1_RAW_IMAGE}"
      "-DLOG_FILE=${OS1_SMOKE_EXEC_BIOS_LOG}"
      "-DSMOKE_TIMEOUT_SECONDS=${OS1_BIOS_SMOKE_TIMEOUT_SECONDS}"
      "-DSMOKE_SETTLE_AFTER_MARKERS_SECONDS=${OS1_SMOKE_SETTLE_AFTER_MARKERS_SECONDS}"
      "-DSMOKE_SEND_DELAY_SECONDS=${OS1_SMOKE_SEND_DELAY_SECONDS}"
      "-DEXPECTED_MARKERS=${OS1_BIOS_EXEC_SMOKE_MARKERS}"
      "-DREJECTED_MARKERS=${OS1_EXEC_SMOKE_REJECT_MARKERS}"
      "-DSERIAL_INPUT_EVENTS=${OS1_SHELL_EXEC_INPUT_EVENTS}"
      "-DVIRTIO_TEST_DISK=${OS1_VIRTIO_TEST_DISK}"
      "-DPYTHON_EXECUTABLE=${OS1_PYTHON_EXECUTABLE}"
      "-DSMOKE_RUNNER=${OS1_SMOKE_RUNNER}"
      -P
      "${PROJECT_SOURCE_DIR}/cmake/scripts/run_smoke_test.cmake"
  )
  list(APPEND OS1_QEMU_SMOKE_TESTS os1_smoke_exec_bios)

  add_custom_target(
    smoke_exec_bios
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -R "^os1_smoke_exec_bios$"
    DEPENDS os1_bios_image "${OS1_VIRTIO_TEST_DISK}"
    USES_TERMINAL
    VERBATIM
  )
else()
  os1_add_unavailable_target(smoke_bios "smoke_bios target unavailable: qemu-system-x86_64 was not found")
  os1_add_unavailable_target(smoke_observe_bios "smoke_observe_bios target unavailable: qemu-system-x86_64 was not found")
  os1_add_unavailable_target(smoke_balance_bios "smoke_balance_bios target unavailable: qemu-system-x86_64 was not found")
  os1_add_unavailable_target(smoke_spawn_bios "smoke_spawn_bios target unavailable: qemu-system-x86_64 was not found")
  os1_add_unavailable_target(smoke_exec_bios "smoke_exec_bios target unavailable: qemu-system-x86_64 was not found")
endif()

os1_register_menu_target(
  smoke_bios
  "Legacy BIOS shell baseline smoke"
  "Run the baseline BIOS shell smoke test."
)
os1_register_menu_target(
  smoke_observe_bios
  "Legacy BIOS observability smoke"
  "Run the BIOS observability smoke that exercises shell inspection commands."
)
os1_register_menu_target(
  smoke_balance_bios
  "Legacy BIOS SMP balance smoke"
  "Run the BIOS SMP balance smoke."
)
os1_register_menu_target(
  smoke_spawn_bios
  "Legacy BIOS child-launch smoke"
  "Run the BIOS child-process spawn smoke."
)
os1_register_menu_target(
  smoke_exec_bios
  "Legacy BIOS exec smoke"
  "Run the BIOS exec smoke."
)

if(BUILD_TESTING AND OS1_QEMU_EXECUTABLE AND OS1_OVMF_CODE AND OS1_OVMF_VARS_TEMPLATE AND OS1_QEMU_DEBUGCON)
  # COM1 is absent; output uses debugcon and input uses the actual PS/2 path.
  set(OS1_NO_UART_INPUT_EVENTS
    "shell prompt ready::sendkey e 1\\nsendkey c 1\\nsendkey h 1\\nsendkey o 1\\nsendkey spc 1\\nsendkey n 1\\nsendkey o 1\\nsendkey minus 1\\nsendkey u 1\\nsendkey a 1\\nsendkey r 1\\nsendkey t 1\\nsendkey ret 1"
  )
  add_test(
    NAME os1_smoke_no_uart
    COMMAND "${CMAKE_COMMAND}"
      "-DQEMU_EXECUTABLE=${OS1_QEMU_EXECUTABLE}"
      "-DISO_IMAGE=${OS1_ISO_IMAGE}"
      "-DOVMF_CODE=${OS1_OVMF_CODE}"
      "-DOVMF_VARS_TEMPLATE=${OS1_OVMF_VARS_TEMPLATE}"
      "-DLOG_FILE=${OS1_ARTIFACT_DIR}/smoke-no-uart.log"
      "-DSMOKE_TIMEOUT_SECONDS=${OS1_UEFI_SMOKE_TIMEOUT_SECONDS}"
      "-DSMOKE_SETTLE_AFTER_MARKERS_SECONDS=${OS1_SMOKE_SETTLE_AFTER_MARKERS_SECONDS}"
      "-DEXPECTED_MARKERS=shell prompt ready;echo: no-uart"
      "-DREJECTED_MARKERS=${OS1_SMOKE_REJECT_MARKERS}"
      "-DMONITOR_SOCKET=${OS1_ARTIFACT_DIR}/smoke-no-uart.sock"
      "-DMONITOR_SEND_EVENTS=${OS1_NO_UART_INPUT_EVENTS}"
      "-DNO_UART=ON"
      "-DVIRTIO_TEST_DISK=${OS1_VIRTIO_TEST_DISK}"
      "-DPYTHON_EXECUTABLE=${OS1_PYTHON_EXECUTABLE}"
      "-DSMOKE_RUNNER=${OS1_SMOKE_RUNNER}"
      -P "${PROJECT_SOURCE_DIR}/cmake/scripts/run_smoke_test.cmake"
  )
  list(APPEND OS1_QEMU_SMOKE_TESTS os1_smoke_no_uart)
  add_custom_target(smoke_no_uart
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -R "^os1_smoke_no_uart$"
    DEPENDS os1_image "${OS1_VIRTIO_TEST_DISK}"
    USES_TERMINAL VERBATIM
  )
else()
  os1_add_unavailable_target(smoke_no_uart "smoke_no_uart requires OS1_QEMU_DEBUGCON=ON, BUILD_TESTING, QEMU and OVMF")
endif()
os1_register_menu_target(smoke_no_uart
  "UEFI boot and PS/2 input without a UART"
  "Remove COM1 and verify a shell command through debugcon and PS/2."
)

if(OS1_QEMU_SMOKE_TESTS)
  set_tests_properties(
    ${OS1_QEMU_SMOKE_TESTS}
    PROPERTIES
      RUN_SERIAL TRUE
      RESOURCE_LOCK os1_qemu_smoke_artifacts
  )
endif()

if(BUILD_TESTING AND OS1_QEMU_EXECUTABLE AND OS1_OVMF_CODE AND OS1_OVMF_VARS_TEMPLATE)
  add_custom_target(
    smoke_all
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -R "^os1_smoke(_observe|_balance|_spawn|_exec|_xhci|_no_uart)?(_bios)?$"
    DEPENDS os1_image os1_bios_image "${OS1_VIRTIO_TEST_DISK}"
    USES_TERMINAL
    VERBATIM
  )
else()
  os1_add_unavailable_target(smoke_all "smoke_all target unavailable: qemu-system-x86_64 or OVMF firmware was not found")
endif()

os1_register_menu_target(
  smoke_all
  "Full smoke matrix"
  "Run the full smoke matrix across the configured boot paths."
)

os1_write_target_menu_manifest()

message(STATUS "Artifacts will be written to ${OS1_ARTIFACT_DIR}")
message(STATUS "Default image: ${OS1_ISO_IMAGE}")
message(STATUS "Legacy BIOS image: ${OS1_RAW_IMAGE}")
