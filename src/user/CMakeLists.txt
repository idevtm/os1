set(USER_STAGE_DIR "${CMAKE_BINARY_DIR}/user-root")
set(USER_BIN_DIR "${OS1_ARTIFACT_DIR}/user")
set(USER_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/linker.ld")

set(USER_COMMON_CXX_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/crt0.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/syscall.cpp"
)

function(os1_add_user_cxx_program program_name source_file output_var)
  set(output_path "${USER_BIN_DIR}/${program_name}.elf")
  add_custom_command(
    OUTPUT "${output_path}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${USER_BIN_DIR}"
    COMMAND
      "${CMAKE_CXX_COMPILER}"
      -std=gnu++20
      -ffreestanding
      -fno-stack-protector
      -fno-asynchronous-unwind-tables
      -fno-unwind-tables
      -fno-exceptions
      -fno-rtti
      -fno-threadsafe-statics
      -fno-use-cxa-atexit
      -fno-pic
      -nostdinc++
      -march=x86-64
      -mtune=generic
      -mgeneral-regs-only
      -mcmodel=large
      -O1
      -Wall
      -Wextra
      "-I${CMAKE_CURRENT_SOURCE_DIR}/include"
      "-I${PROJECT_SOURCE_DIR}/src/uapi"
      -nostdlib
      -nostartfiles
      -nodefaultlibs
      "-Wl,-T,${USER_LINKER_SCRIPT}"
      -Wl,-static
      -Wl,-n
      -Wl,--build-id=none
      -o "${output_path}"
      ${USER_COMMON_CXX_SOURCES}
      "${source_file}"
    DEPENDS
      ${USER_COMMON_CXX_SOURCES}
      "${source_file}"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/os1/syscall.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/os1/syscall.hpp"
      "${PROJECT_SOURCE_DIR}/src/uapi/os1/resource.h"
      "${PROJECT_SOURCE_DIR}/src/uapi/os1/vfs.h"
      "${PROJECT_SOURCE_DIR}/src/uapi/os1/startup.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/os1/vfs.hpp"
      "${PROJECT_SOURCE_DIR}/src/uapi/os1/syscall_numbers.h"
      "${USER_LINKER_SCRIPT}"
    VERBATIM
  )
  set("${output_var}" "${output_path}" PARENT_SCOPE)
endfunction()

os1_add_user_cxx_program(sh "${CMAKE_CURRENT_SOURCE_DIR}/programs/sh.cpp" OS1_USER_SH_ELF)
os1_add_user_cxx_program(init "${CMAKE_CURRENT_SOURCE_DIR}/programs/init.cpp" OS1_USER_INIT_ELF)
os1_add_user_cxx_program(yield "${CMAKE_CURRENT_SOURCE_DIR}/programs/yield.cpp" OS1_USER_YIELD_ELF)
os1_add_user_cxx_program(balanceworker "${CMAKE_CURRENT_SOURCE_DIR}/programs/balanceworker.cpp" OS1_USER_BALANCEWORKER_ELF)
os1_add_user_cxx_program(busyyield "${CMAKE_CURRENT_SOURCE_DIR}/programs/busyyield.cpp" OS1_USER_BUSYYIELD_ELF)
os1_add_user_cxx_program(balancecheck "${CMAKE_CURRENT_SOURCE_DIR}/programs/balancecheck.cpp" OS1_USER_BALANCECHECK_ELF)
os1_add_user_cxx_program(fault "${CMAKE_CURRENT_SOURCE_DIR}/programs/fault.cpp" OS1_USER_FAULT_ELF)
os1_add_user_cxx_program(copycheck "${CMAKE_CURRENT_SOURCE_DIR}/programs/copycheck.cpp" OS1_USER_COPYCHECK_ELF)
os1_add_user_cxx_program(smpcheck "${CMAKE_CURRENT_SOURCE_DIR}/programs/smpcheck.cpp" OS1_USER_SMPCHECK_ELF)
os1_add_user_cxx_program(acpiprobe "${CMAKE_CURRENT_SOURCE_DIR}/programs/acpiprobe.cpp" OS1_USER_ACPIPROBE_ELF)
os1_add_user_cxx_program(ascii "${CMAKE_CURRENT_SOURCE_DIR}/programs/ascii.cpp" OS1_USER_ASCII_ELF)

os1_add_user_cxx_program(fdcheck "${CMAKE_CURRENT_SOURCE_DIR}/programs/fdcheck.cpp" OS1_USER_FDCHECK_ELF)

os1_add_user_cxx_program(fdexec "${CMAKE_CURRENT_SOURCE_DIR}/programs/fdexec.cpp" OS1_USER_FDEXEC_ELF)

os1_add_user_cxx_program(ls "${CMAKE_CURRENT_SOURCE_DIR}/programs/ls.cpp" OS1_USER_LS_ELF)

os1_add_user_cxx_program(cat "${CMAKE_CURRENT_SOURCE_DIR}/programs/cat.cpp" OS1_USER_CAT_ELF)

os1_add_user_cxx_program(vfscheck "${CMAKE_CURRENT_SOURCE_DIR}/programs/vfscheck.cpp" OS1_USER_VFSCHECK_ELF)

add_custom_command(
  OUTPUT "${OS1_INITRD_CPIO}"
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${USER_STAGE_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${USER_STAGE_DIR}/bin" "${USER_STAGE_DIR}/etc"
  COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/data/motd" "${USER_STAGE_DIR}/etc/motd"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_USER_FDCHECK_ELF}" "${USER_STAGE_DIR}/bin/fdcheck"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_USER_FDEXEC_ELF}" "${USER_STAGE_DIR}/bin/fdexec"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_USER_LS_ELF}" "${USER_STAGE_DIR}/bin/ls"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_USER_VFSCHECK_ELF}" "${USER_STAGE_DIR}/bin/vfscheck"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_USER_CAT_ELF}" "${USER_STAGE_DIR}/bin/cat"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_USER_INIT_ELF}" "${USER_STAGE_DIR}/bin/init"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_USER_SH_ELF}" "${USER_STAGE_DIR}/bin/sh"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_USER_YIELD_ELF}" "${USER_STAGE_DIR}/bin/yield"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_USER_BALANCEWORKER_ELF}" "${USER_STAGE_DIR}/bin/balanceworker"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_USER_BUSYYIELD_ELF}" "${USER_STAGE_DIR}/bin/busyyield"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_USER_BALANCECHECK_ELF}" "${USER_STAGE_DIR}/bin/balancecheck"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_USER_FAULT_ELF}" "${USER_STAGE_DIR}/bin/fault"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_USER_COPYCHECK_ELF}" "${USER_STAGE_DIR}/bin/copycheck"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_USER_SMPCHECK_ELF}" "${USER_STAGE_DIR}/bin/smpcheck"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_USER_ACPIPROBE_ELF}" "${USER_STAGE_DIR}/bin/acpiprobe"
  COMMAND "${CMAKE_COMMAND}" -E copy "${OS1_USER_ASCII_ELF}" "${USER_STAGE_DIR}/bin/ascii"
  COMMAND
    "${CMAKE_COMMAND}"
    "-DSTAGE_DIR=${USER_STAGE_DIR}"
    "-DOUTPUT_FILE=${OS1_INITRD_CPIO}"
    "-DCPIO_EXECUTABLE=${OS1_CPIO_EXECUTABLE}"
    -P
    "${PROJECT_SOURCE_DIR}/cmake/scripts/create_initrd.cmake"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/data/motd"
    "${OS1_USER_FDCHECK_ELF}"
    "${OS1_USER_FDEXEC_ELF}"
    "${OS1_USER_LS_ELF}"
    "${OS1_USER_CAT_ELF}"
    "${OS1_USER_VFSCHECK_ELF}"
    "${OS1_USER_INIT_ELF}"
    "${OS1_USER_SH_ELF}"
    "${OS1_USER_YIELD_ELF}"
    "${OS1_USER_BALANCEWORKER_ELF}"
    "${OS1_USER_BUSYYIELD_ELF}"
    "${OS1_USER_BALANCECHECK_ELF}"
    "${OS1_USER_FAULT_ELF}"
    "${OS1_USER_COPYCHECK_ELF}"
    "${OS1_USER_SMPCHECK_ELF}"
    "${OS1_USER_ACPIPROBE_ELF}"
    "${OS1_USER_ASCII_ELF}"
  VERBATIM
)

add_custom_target(user_programs DEPENDS
  "${OS1_USER_VFSCHECK_ELF}"
  "${OS1_USER_CAT_ELF}"
  "${OS1_USER_LS_ELF}"
  "${OS1_USER_FDEXEC_ELF}"
  "${OS1_USER_FDCHECK_ELF}"
  "${OS1_USER_INIT_ELF}"
  "${OS1_USER_SH_ELF}"
  "${OS1_USER_YIELD_ELF}"
  "${OS1_USER_BALANCEWORKER_ELF}"
  "${OS1_USER_BUSYYIELD_ELF}"
  "${OS1_USER_BALANCECHECK_ELF}"
  "${OS1_USER_FAULT_ELF}"
  "${OS1_USER_COPYCHECK_ELF}"
  "${OS1_USER_SMPCHECK_ELF}"
  "${OS1_USER_ACPIPROBE_ELF}"
  "${OS1_USER_ASCII_ELF}"
)

add_custom_target(initrd_cpio DEPENDS "${OS1_INITRD_CPIO}")
