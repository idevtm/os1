if(NOT DEFINED STAGE_DIR)
  message(FATAL_ERROR "STAGE_DIR must be set")
endif()

if(NOT DEFINED OUTPUT_FILE)
  message(FATAL_ERROR "OUTPUT_FILE must be set")
endif()

if(NOT DEFINED CPIO_EXECUTABLE)
  message(FATAL_ERROR "CPIO_EXECUTABLE must be set")
endif()

# Keep full ELF artifacts for debugging, but omit symbol tables from the fixed
# BIOS initrd slot. Allocated interface metadata and section headers survive.
if(NOT DEFINED STRIP_EXECUTABLE)
  message(FATAL_ERROR "STRIP_EXECUTABLE must be set")
endif()
file(GLOB initrd_programs "${STAGE_DIR}/bin/*")
foreach(program IN LISTS initrd_programs)
  execute_process(COMMAND "${STRIP_EXECUTABLE}" --strip-all --remove-section=.comment "${program}"
    RESULT_VARIABLE strip_result)
  if(NOT strip_result EQUAL 0)
    message(FATAL_ERROR "Failed to strip initrd program ${program}")
  endif()
endforeach()

execute_process(
  COMMAND /bin/sh -c "find . -print | \"${CPIO_EXECUTABLE}\" -o -H newc > \"${OUTPUT_FILE}\""
  WORKING_DIRECTORY "${STAGE_DIR}"
  RESULT_VARIABLE initrd_result
)

if(NOT initrd_result EQUAL 0)
  message(FATAL_ERROR "Failed to create initrd archive at ${OUTPUT_FILE}")
endif()
