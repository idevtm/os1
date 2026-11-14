#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/build}"
TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-$SCRIPT_DIR/cmake/toolchains/x86_64-elf.cmake}"
TARGET_MENU_MANIFEST="$BUILD_DIR/generated/target_menu.txt"

configure_workspace() {
	cmake --preset default -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
}

if [ "$(id -u)" -eq 0 ]; then
	printf 'Run ./os1.sh without sudo; the USB writer requests it only when needed.\n' >&2
	exit 1
fi

configure_workspace

if [ "$#" -gt 0 ]; then
	TARGET=$1
elif [ -t 0 ] && [ -t 1 ]; then
	TARGET=$(python3 -I "$SCRIPT_DIR/scripts/target_menu.py" "$TARGET_MENU_MANIFEST")
	printf 'Selected target: %s\n' "$TARGET"
else
	TARGET=run
	printf 'No target specified and no interactive terminal is available; defaulting to %s.\n' "$TARGET" >&2
fi

if [ "$TARGET" = usb ]; then
	exec python3 -I "$SCRIPT_DIR/scripts/write_usb.py" \
		--build-dir "${USB_BUILD_DIR:-$SCRIPT_DIR/build/usb-write}" --toolchain "$TOOLCHAIN_FILE"
fi

if [ "$TARGET" = usb_capture ]; then
	exec python3 -I "$SCRIPT_DIR/scripts/write_usb.py" --capture
fi

cmake --build "$BUILD_DIR"
cmake --build "$BUILD_DIR" --target "$TARGET"
