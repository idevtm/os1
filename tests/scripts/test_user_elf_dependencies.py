#!/usr/bin/env python3
"""Check user ELF rebuilds in an isolated source tree."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def run(*command):
    subprocess.run(command, check=True)


def check(args, source, build):
    repo = Path(__file__).resolve().parents[2]
    for directory in ("src/user", "src/uapi", "src/common"):
        shutil.copytree(repo / directory, source / directory)
    (source / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.20)\n'
        'project(UserElfDependencies LANGUAGES CXX)\n'
        'set(OS1_ARTIFACT_DIR "${CMAKE_BINARY_DIR}/artifacts")\n'
        'set(OS1_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")\n'
        'set(OS1_INITRD_CPIO "${CMAKE_BINARY_DIR}/initrd.cpio")\n'
        'add_subdirectory(src/user)\n'
    )
    # Expose the production rules without pulling in boot, driver, or disk artifacts.
    with (source / "src/user/CMakeLists.txt").open("a") as cmake:
        cmake.write('\nadd_custom_target(dependency_probe DEPENDS '
                    '"${OS1_USER_SH_ELF}" "${OS1_USER_RESCUE_ELF}" "${OS1_USER_INIT_ELF}")\n')

    runtime = source / "src/user/lib"
    with (runtime / "syscall.cpp").open("a") as cpp:
        cpp.write('\n#include "dependency_probe.hpp"\n')
    (runtime / "dependency_probe.hpp").write_text('#include "dependency_leaf.hpp"\n')
    leaf = runtime / "dependency_leaf.hpp"
    leaf.write_text("static_assert(true);\n")

    run(args.cmake, "-S", str(source), "-B", str(build), "-G", args.generator,
        f"-DCMAKE_MAKE_PROGRAM={args.make}", f"-DCMAKE_CXX_COMPILER={args.compiler}",
        "-DCMAKE_SYSTEM_NAME=Generic", "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY")

    names = ("sh", "rescue", "init")
    elfs = {name: build / "artifacts/user" / f"{name}.elf" for name in names}

    def build_elfs():
        run(args.cmake, "--build", str(build), "--target", "dependency_probe", "--parallel", "3")
        return {name: path.stat().st_mtime_ns for name, path in elfs.items()}

    def require_rebuilds(before, expected):
        after = build_elfs()
        actual = {name for name in names if before[name] != after[name]}
        if actual != expected:
            raise RuntimeError(f"expected rebuilds {sorted(expected)}, got {sorted(actual)}")
        return after

    def edit_header(path, text, mode):
        # Older Make versions compare timestamps with one-second resolution.
        if "Makefiles" in args.generator:
            time.sleep(1)
        with path.open(mode) as header:
            header.write(text)

    initial = require_rebuilds(build_elfs(), set())
    edit_header(source / "src/uapi/os1/observe.h", "\n/* Dependency test. */\n", "a")
    direct = require_rebuilds(initial, {"sh", "rescue"})
    edit_header(leaf, "static_assert(1 + 1 == 2);\n", "w")
    transitive = require_rebuilds(direct, set(names))
    require_rebuilds(transitive, set())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("cmake", "compiler", "generator", "make"):
        parser.add_argument(f"--{option}", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="os1-user-elf-") as directory:
        root = Path(directory)
        check(args, root / "source tree", root / "build tree")


if __name__ == "__main__":
    main()
