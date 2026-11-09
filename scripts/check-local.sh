#!/usr/bin/env bash
# Run all local validation profiles. Test failures do not skip later profiles.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
logs="$root/build/local-validation"
mkdir -p "$logs"
failed=0

echo "Building and running all host unit tests"
cmake -S tests/host -B build-host-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DOS1_KMEM_DEBUG=ON
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure --no-tests=error \
  --output-log "$logs/host.log" || failed=1

echo "Building and running the complete normal-image suite"
cmake --preset default -DBUILD_TESTING=ON -DOS1_REQUIRE_UEFI_SMOKE=ON \
  -DOS1_KMEM_DEBUG=ON -DOS1_QEMU_DEBUGCON=ON \
  -DOS1_BOOT_DIAGNOSTICS=OFF -DOS1_USB_KEYBOARD_DIAGNOSTICS=OFF
cmake --build build
ctest --test-dir build --output-on-failure --no-tests=error \
  --output-log "$logs/full.log" || failed=1

echo "Building and running all keyboard diagnostic capture checks"
cmake --preset default -B build/usb-diagnostics -DBUILD_TESTING=ON \
  -DOS1_REQUIRE_UEFI_SMOKE=ON -DOS1_KMEM_DEBUG=ON \
  -DOS1_QEMU_DEBUGCON=OFF -DOS1_USB_KEYBOARD_DIAGNOSTICS=ON
cmake --build build/usb-diagnostics --target os1_usb_image
ctest --test-dir build/usb-diagnostics --output-on-failure --no-tests=error \
  -R '^os1_smoke_usb_keyboard_' --output-log "$logs/diagnostics.log" || failed=1

echo "Local validation finished with status $failed. Logs: $logs"
exit "$failed"
