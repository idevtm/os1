#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/build}"
TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-$SCRIPT_DIR/cmake/toolchains/x86_64-elf.cmake}"
TARGET_MENU_MANIFEST="$BUILD_DIR/generated/target_menu.txt"

TARGET_MENU_ITEMS=
TARGET_MENU_COUNT=0

configure_workspace() {
	cmake --preset default -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
}

load_target_menu_items() {
	if [ ! -s "$TARGET_MENU_MANIFEST" ]; then
		printf 'Target menu manifest not found or empty: %s\n' "$TARGET_MENU_MANIFEST" >&2
		exit 1
	fi

	TARGET_MENU_ITEMS=$(cat "$TARGET_MENU_MANIFEST")
	TARGET_MENU_COUNT=$(wc -l < "$TARGET_MENU_MANIFEST" | tr -d '[:space:]')
}

menu_cleanup() {
	if [ -n "${MENU_STTY-}" ]; then
		stty "$MENU_STTY"
		MENU_STTY=
	fi
	printf '\033[0m\033[?1049l\033[?25h' >&2
}

menu_record() {
	printf '%s\n' "$TARGET_MENU_ITEMS" | sed -n "${1}p"
}

parse_menu_record() {
	menu_record_value=$1
	MENU_TARGET_NAME=${menu_record_value%%|*}
	menu_record_rest=${menu_record_value#*|}
	MENU_TARGET_LABEL=${menu_record_rest%%|*}
	menu_record_rest=${menu_record_rest#*|}
	MENU_TARGET_DESCRIPTION=${menu_record_rest%%|*}
	MENU_TARGET_UNAVAILABLE_REASON=${menu_record_rest#*|}
}

draw_target_menu() {
	selection=$1
	selected_record=$(menu_record "$selection")
	parse_menu_record "$selected_record"
	selected_label=$MENU_TARGET_LABEL
	selected_description=$MENU_TARGET_DESCRIPTION
	selected_unavailable_reason=$MENU_TARGET_UNAVAILABLE_REASON

	printf '\033[2J\033[H' >&2
	printf 'Select an os1 target\n' >&2
	printf 'Use Up/Down to move, Enter to run, q to cancel.\n\n' >&2

	i=1
	while [ "$i" -le "$TARGET_MENU_COUNT" ]; do
		record=$(menu_record "$i")
		parse_menu_record "$record"
		target_status=
		if [ -n "$MENU_TARGET_UNAVAILABLE_REASON" ]; then
			target_status=' [unavailable]'
		fi
		if [ "$i" -eq "$selection" ]; then
			printf '\033[7m> %-20s %s%s\033[0m\n' "$MENU_TARGET_NAME" "$MENU_TARGET_LABEL" "$target_status" >&2
		else
			printf '  %-20s %s%s\n' "$MENU_TARGET_NAME" "$MENU_TARGET_LABEL" "$target_status" >&2
		fi
		i=$((i + 1))
	done

	printf '\nDescription\n' >&2
	printf '  %s\n' "$selected_label" >&2
	printf '  %s\n' "$selected_description" >&2
	if [ -n "$selected_unavailable_reason" ]; then
		printf '  Unavailable: %s\n' "$selected_unavailable_reason" >&2
	fi
}

read_menu_key() {
	key=$(dd bs=1 count=1 2>/dev/null || true)
	if [ "$key" = "$(printf '\033')" ]; then
		key="$key$(dd bs=1 count=1 2>/dev/null || true)$(dd bs=1 count=1 2>/dev/null || true)"
	fi
	printf '%s' "$key"
}

choose_target() {
	selection=1
	MENU_STTY=$(stty -g)
	trap 'menu_cleanup; printf "\n"; exit 130' INT TERM HUP

	stty -echo -icanon min 1 time 0
	printf '\033[?1049h\033[?25l' >&2

	while :; do
		draw_target_menu "$selection"
		key=$(read_menu_key)
		case "$key" in
			"$(printf '\033[A')")
				selection=$((selection - 1))
				if [ "$selection" -lt 1 ]; then
					selection=$TARGET_MENU_COUNT
				fi
				;;
			"$(printf '\033[B')")
				selection=$((selection + 1))
				if [ "$selection" -gt "$TARGET_MENU_COUNT" ]; then
					selection=1
				fi
				;;
			"$(printf '\n')"|"$(printf '\r')")
				break
				;;
			q|Q)
				menu_cleanup
				trap - INT TERM HUP
				printf '\n' >&2
				exit 130
				;;
		esac
	done

	menu_cleanup
	trap - INT TERM HUP
	record=$(menu_record "$selection")
	printf '%s\n' "${record%%|*}"
}

if [ "$(id -u)" -eq 0 ]; then
	printf 'Run ./os1.sh without sudo; the USB writer requests it only when needed.\n' >&2
	exit 1
fi

configure_workspace

if [ "$#" -gt 0 ]; then
	TARGET=$1
elif [ -t 0 ] && [ -t 1 ]; then
	load_target_menu_items
	TARGET=$(choose_target)
	printf 'Selected target: %s\n' "$TARGET"
else
	TARGET=run
	printf 'No target specified and no interactive terminal is available; defaulting to %s.\n' "$TARGET" >&2
fi

if [ "$TARGET" = usb ]; then
	exec python3 -I "$SCRIPT_DIR/scripts/write_usb.py" \
		--build-dir "${USB_BUILD_DIR:-$SCRIPT_DIR/build/usb-write}" --toolchain "$TOOLCHAIN_FILE"
fi

cmake --build "$BUILD_DIR"
cmake --build "$BUILD_DIR" --target "$TARGET"
