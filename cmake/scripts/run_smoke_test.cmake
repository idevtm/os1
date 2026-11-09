if(NOT DEFINED QEMU_EXECUTABLE)
  message(FATAL_ERROR "QEMU_EXECUTABLE must be set")
endif()

if(NOT DEFINED LOG_FILE)
  message(FATAL_ERROR "LOG_FILE must be set")
endif()

if(NOT DEFINED SMOKE_TIMEOUT_SECONDS)
  set(SMOKE_TIMEOUT_SECONDS 8)
endif()

if(NOT DEFINED SMOKE_SETTLE_AFTER_MARKERS_SECONDS)
  set(SMOKE_SETTLE_AFTER_MARKERS_SECONDS 0)
endif()

if(NOT DEFINED SMOKE_SEND_DELAY_SECONDS)
  set(SMOKE_SEND_DELAY_SECONDS 0)
endif()

if(NOT DEFINED EXPECTED_MARKERS)
  set(EXPECTED_MARKERS "")
endif()

if(NOT DEFINED PYTHON_EXECUTABLE)
  message(FATAL_ERROR "PYTHON_EXECUTABLE must be set")
endif()

if(NOT DEFINED SMOKE_RUNNER)
  message(FATAL_ERROR "SMOKE_RUNNER must be set to the path of run_smoke.py")
endif()

set(qemu_command
  "${QEMU_EXECUTABLE}"
  -machine
  q35
  -smp
  4
  -display
  none
  -no-reboot
  -no-shutdown
)

if(NO_UART)
  if(SERIAL_INPUT_EVENTS)
    message(FATAL_ERROR "NO_UART smoke input must use the keyboard monitor, not serial")
  endif()
  # OS1_QEMU_DEBUGCON supplies output independently of the absent COM1 device.
  list(APPEND qemu_command
    -serial none
    -debugcon stdio
    -global isa-debugcon.iobase=0xe9
  )
else()
  list(APPEND qemu_command -serial stdio)
endif()

if(DEFINED MONITOR_SOCKET)
  get_filename_component(monitor_dir "${MONITOR_SOCKET}" DIRECTORY)
  file(MAKE_DIRECTORY "${monitor_dir}")
  file(REMOVE "${MONITOR_SOCKET}")
  list(APPEND qemu_command
    -monitor
    "unix:${MONITOR_SOCKET},server,nowait"
  )
endif()

if(DEFINED ISO_IMAGE)
  if(NOT DEFINED OVMF_CODE)
    message(FATAL_ERROR "OVMF_CODE must be set for ISO-based smoke tests")
  endif()
  if(NOT DEFINED OVMF_VARS_TEMPLATE)
    message(FATAL_ERROR "OVMF_VARS_TEMPLATE must be set for ISO-based smoke tests")
  endif()

  get_filename_component(log_dir "${LOG_FILE}" DIRECTORY)
  get_filename_component(log_name "${LOG_FILE}" NAME_WE)
  file(MAKE_DIRECTORY "${log_dir}")
  set(ovmf_vars_copy "${log_dir}/${log_name}-ovmf-vars.fd")
  file(COPY_FILE "${OVMF_VARS_TEMPLATE}" "${ovmf_vars_copy}" ONLY_IF_DIFFERENT)

  list(APPEND qemu_command
    -drive
    "if=pflash,format=raw,readonly=on,file=${OVMF_CODE}"
    -drive
    "if=pflash,format=raw,file=${ovmf_vars_copy}"
    -cdrom
    "${ISO_IMAGE}"
  )
elseif(DEFINED RAW_IMAGE)
  list(APPEND qemu_command
    -drive
    "format=raw,file=${RAW_IMAGE}"
  )
else()
  message(FATAL_ERROR "Either ISO_IMAGE or RAW_IMAGE must be set")
endif()

list(APPEND qemu_command
  -netdev
  "user,id=os1net"
  -device
  "virtio-net-pci,netdev=os1net,disable-legacy=on"
)

if(DEFINED VIRTIO_TEST_DISK)
  if(NOT EXISTS "${VIRTIO_TEST_DISK}")
    message(FATAL_ERROR "VIRTIO_TEST_DISK was set but does not exist: ${VIRTIO_TEST_DISK}")
  endif()
  list(APPEND qemu_command
    -drive
    "if=none,id=virtio_test,format=raw,file=${VIRTIO_TEST_DISK}"
    -device
    "virtio-blk-pci,drive=virtio_test,disable-legacy=on"
  )
endif()

# Private QEMU overlay: verification programs never modify the shared fixture.
if(DEFINED PROGRAM_DISK)
  list(APPEND qemu_command
    -drive "if=none,id=program_data,format=raw,file=${PROGRAM_DISK},snapshot=on"
    -device "usb-ehci,id=program_ehci"
    -device "usb-storage,bus=program_ehci.0,drive=program_data")
endif()

if(DEFINED EXTRA_QEMU_ARGS)
  list(APPEND qemu_command ${EXTRA_QEMU_ARGS})
endif()

# Build the runner argv. The runner streams QEMU stdout/stderr, matches markers
# on the fly, terminates QEMU as soon as every marker has appeared, and fails
# the test with a log dump if the wall-clock timeout is reached first.
set(runner_command
  "${PYTHON_EXECUTABLE}"
  "${SMOKE_RUNNER}"
  --log
  "${LOG_FILE}"
  --timeout
  "${SMOKE_TIMEOUT_SECONDS}"
  --settle-after-markers
  "${SMOKE_SETTLE_AFTER_MARKERS_SECONDS}"
  --send-delay
  "${SMOKE_SEND_DELAY_SECONDS}"
)
if(DEFINED ISO_IMAGE AND NOT NO_UART)
  # The guarded selector stops at kernel startup and does not delay shell input.
  list(APPEND runner_command --boot-menu-entry 1)
endif()
foreach(marker IN LISTS EXPECTED_MARKERS)
  list(APPEND runner_command --marker "${marker}")
endforeach()
if(DEFINED REJECTED_MARKERS)
  foreach(marker IN LISTS REJECTED_MARKERS)
    list(APPEND runner_command --reject-marker "${marker}")
  endforeach()
endif()
if(DEFINED SERIAL_INPUT_EVENTS)
  foreach(event IN LISTS SERIAL_INPUT_EVENTS)
    list(APPEND runner_command --send-after "${event}")
  endforeach()
endif()
if(DEFINED MONITOR_SOCKET)
  list(APPEND runner_command --monitor-socket "${MONITOR_SOCKET}")
endif()
if(DEFINED MONITOR_SEND_EVENTS)
  foreach(event IN LISTS MONITOR_SEND_EVENTS)
    list(APPEND runner_command --monitor-send-after "${event}")
  endforeach()
endif()
list(APPEND runner_command --)
list(APPEND runner_command ${qemu_command})

execute_process(
  COMMAND ${runner_command}
  RESULT_VARIABLE runner_result
  OUTPUT_VARIABLE runner_stdout
  ERROR_VARIABLE runner_stderr
)

if(runner_stdout)
  message(STATUS "${runner_stdout}")
endif()

if(NOT runner_result EQUAL 0)
  message(FATAL_ERROR
    "Smoke runner failed (exit=${runner_result}). See ${LOG_FILE} for the captured serial log.\n"
    "${runner_stderr}"
  )
endif()
