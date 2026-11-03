#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
export PYTHONDONTWRITEBYTECODE=1

usage() {
    printf 'Usage: %s fast|full\n' "$0"
    printf '  fast  tooling, format/docs/architecture checks, and host tests\n'
    printf '  full  fast checks plus cross build and all registered QEMU smokes\n'
}

if [ "$#" -ne 1 ]; then
    usage >&2
    exit 2
fi

case "$1" in
    fast|full)
        MODE=$1
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        printf 'Unknown verification mode: %s\n' "$1" >&2
        usage >&2
        exit 2
        ;;
esac

OS1_BUILD_DIR=${OS1_BUILD_DIR:-$REPO_ROOT/build}
OS1_HOST_BUILD_DIR=${OS1_HOST_BUILD_DIR:-$REPO_ROOT/build-host-tests}

validate_build_dir() {
    label=$1
    value=$2
    if [ -z "$value" ] || [ "${#value}" -gt 4096 ]; then
        printf '%s must be a non-empty path no longer than 4096 characters.\n' "$label" >&2
        exit 2
    fi
    case "$value" in
        *"
"*|*""*)
            printf '%s must not contain a newline.\n' "$label" >&2
            exit 2
            ;;
    esac
}

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf 'Required command not found: %s\n' "$1" >&2
        exit 2
    fi
}

stage() {
    printf '\n==> %s\n' "$1"
}

require_command python3
validate_build_dir OS1_BUILD_DIR "$OS1_BUILD_DIR"
validate_build_dir OS1_HOST_BUILD_DIR "$OS1_HOST_BUILD_DIR"

OS1_BUILD_DIR=$(python3 -c 'import os, sys; print(os.path.realpath(os.path.abspath(sys.argv[1])))' "$OS1_BUILD_DIR")
OS1_HOST_BUILD_DIR=$(python3 -c 'import os, sys; print(os.path.realpath(os.path.abspath(sys.argv[1])))' "$OS1_HOST_BUILD_DIR")

validate_build_location() {
    label=$1
    value=$2
    case "$value" in
        "$REPO_ROOT/build"|"$REPO_ROOT/build-"*)
            ;;
        "$REPO_ROOT"|"$REPO_ROOT/"*)
            printf '%s must be outside the repository or an ignored top-level build/build-* directory: %s\n' "$label" "$value" >&2
            exit 2
            ;;
    esac
}

validate_build_location OS1_BUILD_DIR "$OS1_BUILD_DIR"
validate_build_location OS1_HOST_BUILD_DIR "$OS1_HOST_BUILD_DIR"
if [ "$OS1_BUILD_DIR" = "$OS1_HOST_BUILD_DIR" ]; then
    printf 'OS1_BUILD_DIR and OS1_HOST_BUILD_DIR must be different directories.\n' >&2
    exit 2
fi

for command in clang-format cmake ctest git ninja; do
    require_command "$command"
done

if [ "$MODE" = full ]; then
    for command in x86_64-elf-gcc x86_64-elf-g++ x86_64-elf-ld nasm cpio xorriso qemu-system-x86_64; do
        require_command "$command"
    done
fi

cd "$REPO_ROOT"

stage "Repository tooling tests"
python3 -m unittest discover -s tools/tests -p 'test_*.py'

stage "C/C++ formatting ratchet"
python3 tools/check_format.py

stage "Documentation index and links"
python3 tools/check_docs.py

stage "Source architecture boundaries"
python3 tools/check_architecture.py

stage "Dependency integrity"
python3 tools/check_dependencies.py

stage "Repository hygiene"
python3 tools/check_hygiene.py

stage "Configure host tests"
cmake -S tests/host -B "$OS1_HOST_BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOS1_KMEM_DEBUG=ON

stage "Build host tests"
cmake --build "$OS1_HOST_BUILD_DIR"

stage "Run host tests"
ctest --test-dir "$OS1_HOST_BUILD_DIR" --output-on-failure --no-tests=error

if [ "$MODE" = full ]; then
    stage "Configure freestanding build"
    cmake --preset default -B "$OS1_BUILD_DIR" -DOS1_REQUIRE_UEFI_SMOKE=ON -DOS1_KMEM_DEBUG=ON -DOS1_QEMU_DEBUGCON=ON

    stage "Build UEFI and BIOS images"
    cmake --build "$OS1_BUILD_DIR" --target os1_image os1_bios_image

    stage "Run registered QEMU smoke matrix"
    ctest --test-dir "$OS1_BUILD_DIR" --output-on-failure --no-tests=error
fi

printf '\nVerification (%s) passed.\n' "$MODE"
