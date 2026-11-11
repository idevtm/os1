#!/usr/bin/env bash
set -euo pipefail

target=x86_64-elf
gcc_version=11.2.0
binutils_version=2.37
gcc_sha256=d08edc536b54c372a1010ff6619dd274c0f1603aa49212ba20f7aa2cda36fa8b
binutils_sha256=820d9724f020a3e69cb337893a0b63c2db161dadcb0e06fc11dc29eb1e84a32c

check_version() {
    local tool=$1 expected=$2 actual=$3
    printf '%s: %s (expected %s)\n' "$tool" "$actual" "$expected"
    [[ "$actual" == "$expected" ]]
}

check() {
    local bin_dir=${1:+$1/bin/}
    local tool name version machine
    for tool in gcc g++; do
        name="$bin_dir$target-$tool"
        version=$("$name" -dumpfullversion)
        machine=$("$name" -dumpmachine)
        check_version "$name version" "$gcc_version" "$version"
        check_version "$name target" "$target" "$machine"
    done
    for tool in as ld; do
        name="$bin_dir$target-$tool"
        version=$("$name" --version)
        version=${version%%$'\n'*}
        check_version "$name version" "$binutils_version" "${version##* }"
    done
}

download() {
    local url=$1 digest=$2 archive=${1##*/}
    curl --fail --location --retry 3 --output "$archive" "$url"
    printf '%s  %s\n' "$digest" "$archive" | sha256sum --check
    tar -xf "$archive"
}

install() (
    local prefix=$1 jobs=$2
    mkdir -p "$prefix"
    prefix=$(cd "$prefix" && pwd -P)
    scratch=$(mktemp -d)
    trap 'rm -rf -- "$scratch"' EXIT
    cd "$scratch"

    download "https://ftp.gnu.org/gnu/binutils/binutils-$binutils_version.tar.xz" "$binutils_sha256"
    download "https://ftp.gnu.org/gnu/gcc/gcc-$gcc_version/gcc-$gcc_version.tar.xz" "$gcc_sha256"

    mkdir build-binutils build-gcc
    cd build-binutils
    "../binutils-$binutils_version/configure" --target="$target" --prefix="$prefix" \
        --disable-nls --disable-werror --disable-gdb --disable-sim
    make -j "$jobs" all
    make install

    export PATH="$prefix/bin:$PATH"
    cd ../build-gcc
    "../gcc-$gcc_version/configure" --target="$target" --prefix="$prefix" \
        --disable-nls --without-isl --without-headers --enable-languages=c,c++ \
        --with-as="$prefix/bin/$target-as" --with-ld="$prefix/bin/$target-ld"
    make -j "$jobs" all-gcc
    make install-gcc
    make -j "$jobs" all-target-libgcc
    make install-target-libgcc
    check "$prefix"
)

case "${1:-}" in
    check) check "${2:-}" ;;
    install) install "${2:?Specify an installation directory}" "${3:?Specify the job count}" ;;
    *) printf 'Usage: %s check [DIRECTORY] | install DIRECTORY JOBS\n' "$0" >&2; exit 2 ;;
esac
