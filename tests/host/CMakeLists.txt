cmake_minimum_required(VERSION 3.20)

project(os1_host_tests LANGUAGES C CXX)

option(OS1_KMEM_DEBUG "Enable kernel small-object allocator debug instrumentation" ON)

if(APPLE)
  # ACPICA intentionally compiles some objects to no symbols with this config.
  # Apple ranlib warns for those, while libtool has a targeted suppression.
  find_program(OS1_DARWIN_LIBTOOL NAMES libtool REQUIRED)
  foreach(OS1_ARCHIVE_LANG C CXX)
    set(CMAKE_${OS1_ARCHIVE_LANG}_ARCHIVE_CREATE
      "<CMAKE_COMMAND> -E rm -f <TARGET> && \"${OS1_DARWIN_LIBTOOL}\" -static -no_warning_for_no_symbols -o <TARGET> <LINK_FLAGS> <OBJECTS>"
    )
    set(CMAKE_${OS1_ARCHIVE_LANG}_ARCHIVE_APPEND "")
    set(CMAKE_${OS1_ARCHIVE_LANG}_ARCHIVE_FINISH "")
  endforeach()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(OS1_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(OS1_HOST_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")

include("${OS1_REPO_ROOT}/cmake/acpica_sources.cmake")

include("${OS1_REPO_ROOT}/cmake/kernel_layout.cmake")

file(MAKE_DIRECTORY "${OS1_HOST_GENERATED_DIR}")
configure_file(
  "${OS1_REPO_ROOT}/cmake/templates/kernel_layout.hpp.in"
  "${OS1_HOST_GENERATED_DIR}/kernel_layout.hpp"
  @ONLY
)

enable_testing()

set(INSTALL_GTEST OFF CACHE BOOL "Do not install GoogleTest from os1 host tests" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
add_subdirectory("${OS1_REPO_ROOT}/third_party/googletest" "${CMAKE_BINARY_DIR}/googletest")
foreach(OS1_GTEST_TARGET gtest gtest_main gmock gmock_main)
  if(TARGET "${OS1_GTEST_TARGET}")
    # GoogleTest trips AppleClang's char8_t conversion warning under C++20.
    target_compile_options("${OS1_GTEST_TARGET}" PRIVATE
      $<$<CXX_COMPILER_ID:AppleClang,Clang>:-Wno-character-conversion>
    )
  endif()
endforeach()

add_library(os1_acpica STATIC ${OS1_ACPICA_SOURCES})
set(OS1_ACPICA_UTPRINT_SOURCE
  "${OS1_ACPICA_ROOT}/source/components/utilities/utprint.c"
)
set_source_files_properties(
  "${OS1_ACPICA_UTPRINT_SOURCE}"
  PROPERTIES
    COMPILE_DEFINITIONS "OS1_ACPICA_RENAME_UTPRINT=1"
)
target_compile_definitions(os1_acpica PRIVATE OS1_HOST_TEST=1)
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
target_compile_options(os1_acpica PRIVATE -O2 -w)

add_library(os1_host_support
  "${OS1_REPO_ROOT}/src/kernel/core/wall_clock.cpp"
  "${OS1_REPO_ROOT}/src/kernel/core/trap_dispatch.cpp"
  "${OS1_REPO_ROOT}/src/kernel/buffer/buffer.cpp"
  "${OS1_REPO_ROOT}/src/kernel/ipc/channel.cpp"
  "${OS1_REPO_ROOT}/src/kernel/ipc/kernel_provider.cpp"
  "${OS1_REPO_ROOT}/src/kernel/drivers/input/service.cpp"
  "${OS1_REPO_ROOT}/src/kernel/drivers/input/ps2_console.cpp"
  "${OS1_REPO_ROOT}/src/kernel/platform/system_control.cpp"
  "${OS1_REPO_ROOT}/src/kernel/platform/system_power.cpp"
  "${OS1_REPO_ROOT}/src/kernel/platform/acpica_ec.cpp"
  support/cpu_stubs.cpp
  support/console_input_stub.cpp
  "${OS1_REPO_ROOT}/src/kernel/vfs/initrd.cpp"
  "${OS1_REPO_ROOT}/src/kernel/vfs/tmpfs.cpp"
  "${OS1_REPO_ROOT}/src/kernel/vfs/ext2.cpp"
  "${OS1_REPO_ROOT}/src/kernel/fs/ext2.cpp"
  "${OS1_REPO_ROOT}/src/kernel/storage/block_device.cpp"
  support/block_device_stub.cpp
  "${OS1_REPO_ROOT}/src/kernel/syscall/vfs.cpp"
  "${OS1_REPO_ROOT}/src/kernel/syscall/observe.cpp"
  "${OS1_REPO_ROOT}/src/kernel/platform/hardware.cpp"
  "${OS1_REPO_ROOT}/src/kernel/proc/startup.cpp"
  "${OS1_REPO_ROOT}/src/kernel/console/console_input.cpp"
  "${OS1_REPO_ROOT}/src/kernel/resource/descriptor.cpp"
  "${OS1_REPO_ROOT}/src/kernel/syscall/resource.cpp"
  "${OS1_REPO_ROOT}/src/kernel/syscall/console_read.cpp"
  "${OS1_REPO_ROOT}/src/kernel/syscall/process.cpp"
  "${OS1_REPO_ROOT}/src/kernel/proc/user_program.cpp"
  "${OS1_REPO_ROOT}/src/kernel/fs/initrd.cpp"
  support/debug_stub.cpp
  support/kernel_state_stub.cpp
  support/lapic_stub.cpp
  support/memory_stubs.cpp
  support/physical_memory.cpp
  "${OS1_REPO_ROOT}/src/kernel/arch/x86_64/apic/ipi.cpp"
  "${OS1_REPO_ROOT}/src/kernel/arch/x86_64/interrupt/vector_allocator.cpp"
  "${OS1_REPO_ROOT}/src/kernel/core/timer_source.cpp"
  "${OS1_REPO_ROOT}/src/kernel/debug/event_ring.cpp"
  "${OS1_REPO_ROOT}/src/kernel/debug/log.cpp"
  "${OS1_REPO_ROOT}/src/kernel/debug/log_export.cpp"
  "${OS1_REPO_ROOT}/src/kernel/drivers/bus/device.cpp"
  "${OS1_REPO_ROOT}/src/kernel/drivers/bus/driver_registry.cpp"
  "${OS1_REPO_ROOT}/src/kernel/drivers/bus/pci_bus.cpp"
  "${OS1_REPO_ROOT}/src/kernel/drivers/virtio/pci_transport.cpp"
  "${OS1_REPO_ROOT}/src/kernel/drivers/virtio/virtqueue.cpp"
  "${OS1_REPO_ROOT}/src/kernel/drivers/module/virtio_host.cpp"
  "${OS1_REPO_ROOT}/src/kernel/drivers/net/arp_cache.cpp"
  "${OS1_REPO_ROOT}/src/kernel/drivers/usb/diagnostics.cpp"
  "${OS1_REPO_ROOT}/src/kernel/drivers/usb/stall_trace.cpp"
  "${OS1_REPO_ROOT}/src/kernel/drivers/usb/ehci_controller.cpp"
  "${OS1_REPO_ROOT}/src/kernel/drivers/usb/hub.cpp"
  "${OS1_REPO_ROOT}/src/kernel/drivers/usb/mass_storage.cpp"
  "${OS1_REPO_ROOT}/src/kernel/drivers/usb/hid_keyboard.cpp"
  "${OS1_REPO_ROOT}/src/kernel/drivers/usb/xhci_controller.cpp"
  "${OS1_REPO_ROOT}/src/kernel/fs/cpio_newc.cpp"
  "${OS1_REPO_ROOT}/src/kernel/handoff/boot_info.cpp"
  "${OS1_REPO_ROOT}/src/kernel/mm/boot_mapping.cpp"
  "${OS1_REPO_ROOT}/src/kernel/drivers/bus/resource.cpp"
  "${OS1_REPO_ROOT}/src/kernel/mm/kmem.cpp"
  "${OS1_REPO_ROOT}/src/kernel/mm/page_frame.cpp"
  "${OS1_REPO_ROOT}/src/kernel/mm/dma.cpp"
  "${OS1_REPO_ROOT}/src/kernel/mm/user_copy.cpp"
  "${OS1_REPO_ROOT}/src/kernel/mm/virtual_memory.cpp"
  "${OS1_REPO_ROOT}/src/kernel/platform/acpi.cpp"
  "${OS1_REPO_ROOT}/src/kernel/platform/acpi_aml.cpp"
  "${OS1_REPO_ROOT}/src/kernel/platform/acpica_integration.cpp"
  "${OS1_REPO_ROOT}/src/kernel/platform/acpica_osl.cpp"
  "${OS1_REPO_ROOT}/src/kernel/platform/hpet.cpp"
  "${OS1_REPO_ROOT}/src/kernel/platform/irq_registry.cpp"
  "${OS1_REPO_ROOT}/src/kernel/platform/power.cpp"
  "${OS1_REPO_ROOT}/src/kernel/platform/pci_capability.cpp"
  "${OS1_REPO_ROOT}/src/kernel/platform/pci.cpp"
  "${OS1_REPO_ROOT}/src/kernel/platform/pci_config.cpp"
  "${OS1_REPO_ROOT}/src/kernel/platform/pci_msi.cpp"
  "${OS1_REPO_ROOT}/src/kernel/platform/state.cpp"
  "${OS1_REPO_ROOT}/src/kernel/proc/process.cpp"
  "${OS1_REPO_ROOT}/src/kernel/proc/reaper.cpp"
  "${OS1_REPO_ROOT}/src/kernel/proc/kernel_stack.cpp"
  "${OS1_REPO_ROOT}/src/kernel/proc/thread.cpp"
  "${OS1_REPO_ROOT}/src/kernel/proc/user_elf.cpp"
  "${OS1_REPO_ROOT}/src/kernel/sched/scheduler.cpp"
  "${OS1_REPO_ROOT}/src/kernel/sched/thread_queue.cpp"
  "${OS1_REPO_ROOT}/src/kernel/sync/wait_queue.cpp"
  "${OS1_REPO_ROOT}/src/kernel/syscall/wait.cpp"
)
target_compile_definitions(os1_host_support PUBLIC OS1_HOST_TEST=1)
if(OS1_KMEM_DEBUG)
  target_compile_definitions(os1_host_support PUBLIC OS1_KMEM_DEBUG=1)
endif()
target_include_directories(os1_host_support PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}"
  "${OS1_ACPICA_CUSTOM_INCLUDE_DIR}"
  "${OS1_REPO_ROOT}/src/common"
  "${OS1_REPO_ROOT}/src/kernel"
  "${OS1_REPO_ROOT}/src/uapi"
  "${OS1_HOST_GENERATED_DIR}"
)
target_include_directories(os1_host_support SYSTEM PUBLIC
  "${OS1_ACPICA_INCLUDE_DIR}"
)

add_executable(os1_host_tests
  common/context_tests.cpp
  common/discovery_tests.cpp
  common/hardware_tests.cpp
  common/driver_tests.cpp
  common/presentation_tests.cpp
  common/guided_tests.cpp
  common/shell_tests.cpp
  common/pager_tests.cpp
  common/text_tools_tests.cpp
  common/interface_tests.cpp
  common/parameter_runtime_tests.cpp
  common/elf64_tests.cpp
  common/freestanding_string_tests.cpp
  fs/cpio_newc_tests.cpp
  handoff/boot_info_tests.cpp
  kernel/atomic_tests.cpp
  kernel/acpi_hpet_tests.cpp
  kernel/acpi_aml_tests.cpp
  kernel/acpica_integration_tests.cpp
  kernel/cpu_record_tests.cpp
  kernel/utility_tests.cpp
  kernel/event_ring_tests.cpp
  kernel/log_ring_tests.cpp
  kernel/log_export_tests.cpp
  kernel/irq_route_registry_tests.cpp
  kernel/irq_vector_allocator_tests.cpp
  kernel/load_balancer_tests.cpp
  kernel/runqueue_tests.cpp
  kernel/timer_source_tests.cpp
  kernel/measurement_clock_tests.cpp
  kernel/wall_clock_tests.cpp
  kernel/pci_bus_tests.cpp
  kernel/pci_capability_tests.cpp
  kernel/pci_msi_tests.cpp
  kernel/pci_resource_tests.cpp
  kernel/virtio_pci_transport_tests.cpp
  kernel/trap_dispatch_tests.cpp
  kernel/wait_queue_tests.cpp
  kernel/arp_cache_tests.cpp
  kernel/ehci_tests.cpp
  kernel/gm206_tests.cpp
  kernel/display_draw_tests.cpp
  kernel/stream_ring_tests.cpp
  ${PROJECT_SOURCE_DIR}/../../src/kernel/drivers/display/reflection.cpp
  ${PROJECT_SOURCE_DIR}/../../src/drivers/gm206/reflection.cpp
  ${PROJECT_SOURCE_DIR}/../../src/drivers/gm206/display.cpp
  ${PROJECT_SOURCE_DIR}/../../src/drivers/gm206/driver.cpp
  kernel/usb_storage_tests.cpp
  kernel/hid_keyboard_tests.cpp
  kernel/input_service_tests.cpp
  kernel/ehci_stall_trace_tests.cpp
  kernel/ipi_tests.cpp
  kernel/ext2_tests.cpp
  kernel/xhci_controller_tests.cpp
  mm/kmem_tests.cpp
  mm/page_frame_tests.cpp
  mm/dma_tests.cpp
  mm/tlb_retirement_tests.cpp
  mm/buffer_tests.cpp
  mm/user_address_tests.cpp
  mm/virtual_memory_tests.cpp
  proc/task_registry_tests.cpp
  proc/user_elf_tests.cpp
  syscall/ipc_tests.cpp
  syscall/vfs_tests.cpp
  syscall/resource_tests.cpp
  syscall/observe_abi_tests.cpp
)
target_link_libraries(os1_host_tests PRIVATE os1_host_support os1_acpica GTest::gtest_main)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(OS1_EXT2_FIXTURE "${CMAKE_BINARY_DIR}/ext2-fixture.raw")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${OS1_REPO_ROOT}/cmake/scripts/create_ext2_image.py"
  --output "${OS1_EXT2_FIXTURE}" --utility "${CMAKE_CURRENT_SOURCE_DIR}/support/block_device_stub.cpp"
  --size-mib 4 --reset COMMAND_ERROR_IS_FATAL ANY)
target_compile_definitions(os1_host_tests PRIVATE OS1_EXT2_FIXTURE="${OS1_EXT2_FIXTURE}")

set(OS1_LOG_EXT2_FIXTURE "${CMAKE_BINARY_DIR}/log-ext2-fixture.raw")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${OS1_REPO_ROOT}/cmake/scripts/create_ext2_image.py"
  --output "${OS1_LOG_EXT2_FIXTURE}" --utility "${CMAKE_CURRENT_SOURCE_DIR}/support/block_device_stub.cpp"
  --size-mib 16 --reset COMMAND_ERROR_IS_FATAL ANY)
target_compile_definitions(os1_host_tests PRIVATE OS1_LOG_EXT2_FIXTURE="${OS1_LOG_EXT2_FIXTURE}")

include(GoogleTest)
gtest_add_tests(TARGET os1_host_tests TEST_LIST os1_host_tests_TESTS)
set_tests_properties(${os1_host_tests_TESTS} PROPERTIES LABELS "host;unit")

add_test(NAME HardwareReferenceComparison
  COMMAND "${Python3_EXECUTABLE}" -E -B "${OS1_REPO_ROOT}/tests/scripts/test_hardware_compare.py")
set_tests_properties(HardwareReferenceComparison PROPERTIES LABELS "host;unit")

add_test(NAME UapiHeaderCompatibility
  COMMAND "${Python3_EXECUTABLE}" -E -B "${OS1_REPO_ROOT}/tests/scripts/test_uapi_headers.py"
    --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}")
set_tests_properties(UapiHeaderCompatibility PROPERTIES LABELS "host;unit")
