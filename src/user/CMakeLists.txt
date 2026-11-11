set(USER_STAGE_DIR "${CMAKE_BINARY_DIR}/user-root")
set(USER_BIN_DIR "${OS1_ARTIFACT_DIR}/user")
set(USER_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/linker.ld")

set(USER_COMMON_CXX_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/crt0.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/syscall.cpp"
)

function(os1_add_user_cxx_program program_name source_file output_var)
  set(output_path "${USER_BIN_DIR}/${program_name}.elf")
  set(compile_options
    -std=gnu++20
    ${ARGN}
    -ffreestanding
    -fno-stack-protector
    -fno-asynchronous-unwind-tables
    -fno-unwind-tables
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -fno-use-cxa-atexit
    # Fixed high-address executables use RIP-relative code and a static GOT.
    # Disable relaxation: absolute 32-bit addresses cannot encode this window.
    -fpie
    -fno-plt
    -no-pie
    -nostdinc++
    -march=x86-64
    -mtune=generic
    -mgeneral-regs-only
    -mcmodel=small
    # Keep bundled programs within the bounded BIOS initrd load window.
    -Os
    -flto
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
    "-I${CMAKE_CURRENT_SOURCE_DIR}/include"
    "-I${PROJECT_SOURCE_DIR}/src/uapi"
    "-I${PROJECT_SOURCE_DIR}/src/common"
  )
  set(object_dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/os1_user_${program_name}.dir")
  set(objects)
  foreach(source IN ITEMS ${USER_COMMON_CXX_SOURCES} "${source_file}")
    get_filename_component(source_name "${source}" NAME)
    set(object "${object_dir}/${source_name}.o")
    add_custom_command(
      OUTPUT "${object}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${object_dir}"
      COMMAND "${CMAKE_CXX_COMPILER}" ${compile_options}
        -MD -MF "${object}.d" -MQ "${object}" -c "${source}" -o "${object}"
      DEPENDS "${source}"
      DEPFILE "${object}.d"
      VERBATIM
    )
    list(APPEND objects "${object}")
  endforeach()

  add_custom_command(
    OUTPUT "${output_path}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${USER_BIN_DIR}"
    COMMAND "${CMAKE_CXX_COMPILER}" ${compile_options}
      -nostdlib
      -nostartfiles
      -nodefaultlibs
      "-Wl,-T,${USER_LINKER_SCRIPT}"
      -Wl,-static
      -Wl,--no-relax
      -Wl,-n
      -Wl,--gc-sections
      -Wl,--build-id=none
      -o "${output_path}"
      ${objects}
    DEPENDS ${objects} "${USER_LINKER_SCRIPT}"
    VERBATIM
  )
  set("${output_var}" "${output_path}" PARENT_SCOPE)
  set(OS1_USER_PROGRAM_FILES ${OS1_USER_PROGRAM_FILES} "${output_path}" PARENT_SCOPE)
endfunction()

os1_add_user_cxx_program(sh "${CMAKE_CURRENT_SOURCE_DIR}/programs/sh.cpp" OS1_USER_SH_ELF)
os1_add_user_cxx_program(gfxdemo "${CMAKE_CURRENT_SOURCE_DIR}/programs/gfxdemo.cpp" OS1_USER_GFXDEMO_ELF)
os1_add_user_cxx_program(run-tests "${CMAKE_CURRENT_SOURCE_DIR}/programs/run_tests.cpp" OS1_USER_RUN_TESTS_ELF)
os1_add_user_cxx_program(init "${CMAKE_CURRENT_SOURCE_DIR}/programs/init.cpp" OS1_USER_INIT_ELF)
os1_add_user_cxx_program(rescue "${CMAKE_CURRENT_SOURCE_DIR}/programs/rescue.cpp" OS1_USER_RESCUE_ELF)
os1_add_user_cxx_program(yield "${CMAKE_CURRENT_SOURCE_DIR}/programs/yield.cpp" OS1_USER_YIELD_ELF)
os1_add_user_cxx_program(balanceworker "${CMAKE_CURRENT_SOURCE_DIR}/programs/balanceworker.cpp" OS1_USER_BALANCEWORKER_ELF)
os1_add_user_cxx_program(busyyield "${CMAKE_CURRENT_SOURCE_DIR}/programs/busyyield.cpp" OS1_USER_BUSYYIELD_ELF)
os1_add_user_cxx_program(balancecheck "${CMAKE_CURRENT_SOURCE_DIR}/programs/balancecheck.cpp" OS1_USER_BALANCECHECK_ELF)
os1_add_user_cxx_program(fault "${CMAKE_CURRENT_SOURCE_DIR}/programs/fault.cpp" OS1_USER_FAULT_ELF)
os1_add_user_cxx_program(copycheck "${CMAKE_CURRENT_SOURCE_DIR}/programs/copycheck.cpp" OS1_USER_COPYCHECK_ELF)
os1_add_user_cxx_program(smpcheck "${CMAKE_CURRENT_SOURCE_DIR}/programs/smpcheck.cpp" OS1_USER_SMPCHECK_ELF)
os1_add_user_cxx_program(acpiprobe "${CMAKE_CURRENT_SOURCE_DIR}/programs/acpiprobe.cpp" OS1_USER_ACPIPROBE_ELF)
os1_add_user_cxx_program(ascii "${CMAKE_CURRENT_SOURCE_DIR}/programs/ascii.cpp" OS1_USER_ASCII_ELF)

os1_add_user_cxx_program(textcheck "${CMAKE_CURRENT_SOURCE_DIR}/programs/textcheck.cpp" OS1_USER_TEXTCHECK_ELF)

os1_add_user_cxx_program(fdcheck "${CMAKE_CURRENT_SOURCE_DIR}/programs/fdcheck.cpp" OS1_USER_FDCHECK_ELF)

os1_add_user_cxx_program(fdexec "${CMAKE_CURRENT_SOURCE_DIR}/programs/fdexec.cpp" OS1_USER_FDEXEC_ELF)

os1_add_user_cxx_program(ls "${CMAKE_CURRENT_SOURCE_DIR}/programs/ls.cpp" OS1_USER_LS_ELF)

os1_add_user_cxx_program(cat "${CMAKE_CURRENT_SOURCE_DIR}/programs/cat.cpp" OS1_USER_CAT_ELF)

os1_add_user_cxx_program(vfscheck "${CMAKE_CURRENT_SOURCE_DIR}/programs/vfscheck.cpp" OS1_USER_VFSCHECK_ELF)

os1_add_user_cxx_program(textprocessor "${CMAKE_CURRENT_SOURCE_DIR}/programs/textprocessor.cpp" OS1_USER_TEXTPROCESSOR_ELF)
os1_add_user_cxx_program(buffercheck "${CMAKE_CURRENT_SOURCE_DIR}/programs/buffercheck.cpp" OS1_USER_BUFFERCHECK_ELF)
os1_add_user_cxx_program(logcheck "${CMAKE_CURRENT_SOURCE_DIR}/programs/logcheck.cpp" OS1_USER_LOGCHECK_ELF)
os1_add_user_cxx_program(ipccheck "${CMAKE_CURRENT_SOURCE_DIR}/programs/ipccheck.cpp" OS1_USER_IPCCHECK_ELF)
os1_add_user_cxx_program(paramdemo "${CMAKE_CURRENT_SOURCE_DIR}/programs/paramdemo.cpp" OS1_USER_PARAMDEMO_ELF)
os1_add_user_cxx_program(paramcheck "${CMAKE_CURRENT_SOURCE_DIR}/programs/paramcheck.cpp" OS1_USER_PARAMCHECK_ELF)
os1_add_user_cxx_program(intf "${CMAKE_CURRENT_SOURCE_DIR}/programs/intf.cpp" OS1_USER_INTF_ELF)

os1_add_user_cxx_program(counter "${CMAKE_CURRENT_SOURCE_DIR}/programs/counter.cpp" OS1_USER_COUNTER_ELF)
os1_add_user_cxx_program(countercheck "${CMAKE_CURRENT_SOURCE_DIR}/programs/countercheck.cpp" OS1_USER_COUNTERCHECK_ELF)

os1_add_user_cxx_program(activation "${CMAKE_CURRENT_SOURCE_DIR}/programs/activation.cpp" OS1_USER_ACTIVATION_ELF)
os1_add_user_cxx_program(activationcheck "${CMAKE_CURRENT_SOURCE_DIR}/programs/activationcheck.cpp" OS1_USER_ACTIVATIONCHECK_ELF)

os1_add_user_cxx_program(diskutil "${CMAKE_CURRENT_SOURCE_DIR}/programs/diskutil.cpp" OS1_USER_DISKUTIL_ELF)
add_custom_target(os1_diskutil ALL DEPENDS "${OS1_USER_DISKUTIL_ELF}")
set(OS1_USER_DISKUTIL_ELF "${OS1_USER_DISKUTIL_ELF}" PARENT_SCOPE)

foreach(name IN ITEMS lshw lspci lsusb lsblk)
  os1_add_user_cxx_program(${name} "${CMAKE_CURRENT_SOURCE_DIR}/programs/${name}.cpp" utility_elf)
endforeach()

# Keep the recovery set explicit. All other programs are deployed onto ext2.
set(recovery_programs init rescue)
set(recovery_commands)
set(recovery_files)
set(program_manifest "{\"version\":2,\"programs\":[")
set(separator "")
foreach(program IN LISTS OS1_USER_PROGRAM_FILES)
  get_filename_component(name "${program}" NAME_WE)
  if(name IN_LIST recovery_programs)
    list(APPEND recovery_commands COMMAND "${CMAKE_COMMAND}" -E copy "${program}" "${USER_STAGE_DIR}/bin/${name}")
    list(APPEND recovery_files "${program}")
  else()
    string(APPEND program_manifest "${separator}{\"source\":\"${program}\",\"destination\":\"/disk/bin/${name}\"}")
    set(separator ",")
  endif()
endforeach()
# Retain the published path used by older images and deployment workflows.
string(APPEND program_manifest ",{\"source\":\"${OS1_USER_DISKUTIL_ELF}\",\"destination\":\"/disk/diskutil\"}],\"resources\":[{\"source\":\"${CMAKE_CURRENT_SOURCE_DIR}/data/motd\",\"destination\":\"/disk/etc/motd\"}]}\n")
file(GENERATE OUTPUT "${OS1_GENERATED_DIR}/storage-programs-base.json" CONTENT "${program_manifest}")
add_custom_command(OUTPUT "${OS1_GENERATED_DIR}/storage-programs.json"
  COMMAND "${OS1_PYTHON_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/cmake/scripts/driver_manifest.py"
    --base "${OS1_GENERATED_DIR}/storage-programs-base.json"
    --metadata "${OS1_DRIVER_METADATA}" --image "${OS1_DRIVER_ELF}"
    --metadata "${OS1_DISPLAY_DRIVER_METADATA}" --image "${OS1_DISPLAY_DRIVER_ELF}"
    --output "${OS1_GENERATED_DIR}/storage-programs.json"
  DEPENDS "${OS1_GENERATED_DIR}/storage-programs-base.json" "${OS1_DRIVER_METADATA}"
    "${OS1_DRIVER_ELF}" "${OS1_DISPLAY_DRIVER_ELF}" "${OS1_DISPLAY_DRIVER_METADATA}"
    "${PROJECT_SOURCE_DIR}/cmake/scripts/driver_manifest.py"
    "${PROJECT_SOURCE_DIR}/cmake/scripts/driver_package.py"
  VERBATIM)
add_custom_target(os1_storage_manifest ALL DEPENDS "${OS1_GENERATED_DIR}/storage-programs.json")
list(APPEND OS1_USER_PROGRAM_FILES "${OS1_DRIVER_ELF}" "${OS1_DISPLAY_DRIVER_ELF}" "${OS1_GENERATED_DIR}/storage-programs.json")
set(OS1_USER_PROGRAM_FILES "${OS1_USER_PROGRAM_FILES}" PARENT_SCOPE)

add_custom_command(
  OUTPUT "${OS1_INITRD_CPIO}"
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${USER_STAGE_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${USER_STAGE_DIR}/bin"
  ${recovery_commands}
  COMMAND "${CMAKE_COMMAND}"
    "-DSTAGE_DIR=${USER_STAGE_DIR}" "-DOUTPUT_FILE=${OS1_INITRD_CPIO}"
    "-DCPIO_EXECUTABLE=${OS1_CPIO_EXECUTABLE}" "-DSTRIP_EXECUTABLE=${CMAKE_STRIP}"
    "-DPYTHON_EXECUTABLE=${OS1_PYTHON_EXECUTABLE}"
    -P "${PROJECT_SOURCE_DIR}/cmake/scripts/create_initrd.cmake"
  DEPENDS ${recovery_files} "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/motd"
    "${PROJECT_SOURCE_DIR}/cmake/scripts/compact_user_elf.py"
    "${PROJECT_SOURCE_DIR}/cmake/scripts/create_initrd.cmake"
  VERBATIM
)
add_custom_target(user_programs DEPENDS ${OS1_USER_PROGRAM_FILES})
add_custom_target(initrd_cpio DEPENDS "${OS1_INITRD_CPIO}")

# Disk-only deployment fixtures. Never stage these programs into the initrd.
if(BUILD_TESTING)
  os1_add_user_cxx_program(entry_probe
    "${PROJECT_SOURCE_DIR}/tests/fixtures/entry_probe.cpp" entry_probe_elf)
  add_custom_target(os1_entry_probe DEPENDS "${entry_probe_elf}")
  set(OS1_ENTRY_PROBE_ELF "${entry_probe_elf}" PARENT_SCOPE)
  foreach(version IN ITEMS 1 2)
    os1_add_user_cxx_program(deploy_probe_${version}
      "${PROJECT_SOURCE_DIR}/tests/fixtures/deploy_probe.cpp" probe_elf "-DDEPLOY_VERSION=\"${version}\"")
    add_custom_target(os1_deploy_probe_${version} DEPENDS "${probe_elf}")
    set(OS1_DEPLOY_PROBE_${version} "${probe_elf}" PARENT_SCOPE)
  endforeach()
endif()
