if(NOT DEFINED OS1_REPO_ROOT)
  set(OS1_REPO_ROOT "${PROJECT_SOURCE_DIR}")
endif()

set(OS1_ACPICA_ROOT "${OS1_REPO_ROOT}/third_party/acpica")
set(OS1_ACPICA_CUSTOM_INCLUDE_DIR "${OS1_REPO_ROOT}/src/kernel/platform/acpica/include")
set(OS1_ACPICA_INCLUDE_DIR "${OS1_ACPICA_ROOT}/source/include")

if(NOT EXISTS "${OS1_ACPICA_ROOT}/source/include/acpi.h")
  message(FATAL_ERROR "Missing ACPICA sources under ${OS1_ACPICA_ROOT}; run git submodule update --init --recursive")
endif()

set(OS1_ACPICA_COMPONENT_DIRS
  dispatcher
  events
  executer
  hardware
  namespace
  parser
  resources
  tables
  utilities
)

set(OS1_ACPICA_SOURCES)
foreach(component_dir IN LISTS OS1_ACPICA_COMPONENT_DIRS)
  file(GLOB component_sources CONFIGURE_DEPENDS
    "${OS1_ACPICA_ROOT}/source/components/${component_dir}/*.c"
  )
  list(APPEND OS1_ACPICA_SOURCES ${component_sources})
endforeach()
list(FILTER OS1_ACPICA_SOURCES EXCLUDE REGEX ".*/rsdump(info)?\\.c$")
list(SORT OS1_ACPICA_SOURCES)

# Keep detailed region and method failures without exceeding the BIOS kernel slot.
# Both host and kernel builds use this profile; upstream sources remain unchanged.
foreach(diagnostic_source
    events/evregion.c
    parser/psparse.c
    utilities/uterror.c
    utilities/utxferror.c)
  set_property(SOURCE "${OS1_ACPICA_ROOT}/source/components/${diagnostic_source}"
    APPEND PROPERTY COMPILE_DEFINITIONS OS1_ACPICA_ERROR_DIAGNOSTICS=1)
endforeach()
