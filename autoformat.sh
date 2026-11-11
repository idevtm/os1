#!/usr/bin/env bash
set -euo pipefail
unset CDPATH

root=$(cd -P -- "$(dirname -- "$0")" && pwd)
cd -- "$root"

if (($# == 0)); then
    echo "Usage: ./autoformat.sh src/path.cpp [tests/path.hpp ...]" >&2
    exit 2
fi

reject() {
    printf 'Cannot format %s. Select a tracked project C or C++ file.\n' "$1" >&2
    exit 2
}

for path in "$@"; do
    case "$path" in
        */../*|*/./*|*//*|*/third_party/*|*/generated/*|*/build/*) reject "$path" ;;
        src/*|tests/*) ;;
        *) reject "$path" ;;
    esac
    case "$path" in
        *.c|*.cpp|*.h|*.hpp) ;;
        *) reject "$path" ;;
    esac
    [[ -f "$path" && ! -L "$path" ]] || reject "$path"
    parent=$(cd -P -- "${path%/*}" && pwd)
    [[ "$parent" == "$root/${path%/*}" ]] || reject "$path"
    git --literal-pathspecs ls-files --error-unmatch -- "$path" >/dev/null 2>&1 || reject "$path"
done

clang-format -i -- "$@"
