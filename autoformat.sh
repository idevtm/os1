#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)

printf 'Formatting project-owned C/C++ files ...\n'
find "$SCRIPT_DIR/src" "$SCRIPT_DIR/tests/host" -type f \
  \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' \
  -o -name '*.h' -o -name '*.hh' -o -name '*.hpp' \) -print0 \
  | xargs -0 clang-format -i
