# Major Dependency Contract

> Status: active
> Owner: build and subsystem maintainers
> Last verified: 2026-08-19 at `d255142`

This document records the major vendored dependencies that materially shape the
kernel, boot path, or test harness. Source adapters and executable validation
remain authoritative.

[`tools/dependency-lock.json`](../tools/dependency-lock.json) is the executable
identity contract. [`tools/check_dependencies.py`](../tools/check_dependencies.py)
strictly validates its schema, parent gitlinks, initialized/clean submodule
checkouts, the complete Limine file set, sizes/checksums, and synchronization of
the pins below. It runs in `tools/verify.sh fast`.

## Pin Summary

| Dependency | Exact pin | Form | Purpose |
| --- | --- | --- | --- |
| ACPICA | commit `232ff3f8ae1a4da11c709f61d9154482cfe8e6df`, upstream tag `20260408` | Git submodule `third_party/acpica` | ACPI table manager, namespace, resources, methods, and OSL-backed platform integration |
| Limine | upstream release `v11.4.0`; repository files checksummed below | Vendored release artifacts under `third_party/limine/v11.4.0` | Default UEFI bootloader/protocol and ISO boot image |
| GoogleTest | commit `52eb8108c5bdec04579160ae17225d66034bd723`, upstream tag `v1.17.0` | Git submodule `third_party/googletest` | Host-only unit-test framework |

Initialize the submodules after cloning:

```sh
git submodule update --init --recursive
```

## ACPICA

### Boundary and local policy

- Upstream C sources and headers remain unmodified under `third_party/acpica`.
- [`cmake/acpica_sources.cmake`](../cmake/acpica_sources.cmake) selects the
  dispatcher, events, executer, hardware, namespace, parser, resources, tables,
  and utilities components while excluding upstream dump-only sources.
- OS policy and adaptation live in
  [`src/kernel/platform/acpica/`](../src/kernel/platform/acpica),
  [`acpica_integration.cpp`](../src/kernel/platform/acpica_integration.cpp), and
  [`acpica_osl.cpp`](../src/kernel/platform/acpica_osl.cpp).
- Third-party warnings are isolated from first-party warning policy. Local fixes
  belong in the adapter unless an upstream patch is deliberately carried and
  documented here with a removal condition.

### Footprint reference

At the Phase B baseline using the configured `x86_64-elf` toolchain:

- `libos1_acpica.a`: 512,534-byte archive; `size -t` totals 137,940 text,
  1,571 data, and 1,663 BSS bytes (141,174 total before final linking).
- `kernel.elf`: 619,712-byte file; 360,448 text, 40,960 data, and 102,400 BSS
  bytes (503,808 total).

The kernel number is an aggregate, not an ACPICA-only delta. Record both before
and after every ACPICA update; [TD-003](TECH_DEBT.md#td-003--enforce-acpica-upgrade-and-footprint-budgets)
tracks promotion to an automated budget.

### Upgrade procedure

1. Read the upstream release notes and confirm the target tag/commit.
2. Update only the ACPICA gitlink, verify `ACPI_CA_VERSION` in `acpixf.h`, and
   inspect component additions/removals against `cmake/acpica_sources.cmake`.
3. Keep `git diff --submodule=log` in the review evidence and update the pin and
   footprint reference in this document plus `tools/dependency-lock.json`.
4. Run targeted host tests, then the complete contract:

   ```sh
   ctest --test-dir build-host-tests --output-on-failure -R 'Acpi|Acpica'
   tools/verify.sh full
   ```

5. Review both UEFI and BIOS ACPICA markers, OSL failure paths, kernel size, and
   BIOS/boot-layout assertions before accepting the new gitlink.

Removing ACPICA requires a replacement for the live table/namespace/resource
contract; the older narrow parser is not a supported equivalent fallback.

## Limine

### Boundary and exact artifacts

Limine is not a submodule. The release directory contains the upstream protocol
header, UEFI executable, ISO boot image, and licenses. The first-party boundary
is [`src/boot/limine/`](../src/boot/limine), which translates Limine-owned state
into the shared physical-address `BootInfo` contract.

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `BOOTX64.EFI` | 319,488 | `d9f86e107067eb683c9498844c72c69e51869ffa01f0297a8f11f49e0c3ceb71` |
| `limine-uefi-cd.bin` | 2,949,120 | `88407b143f4bbbd74b253be7f8df41a684d4bf9276f80ebc5ad6fcb60b58cb36` |
| `limine.h` | 16,718 | `bf0f2aff9395fe158c7268498dff8841d700c5544ee987a2b80294adae4deeec` |
| `LICENSE` | 1,297 | `f174b8ff78f6f8982a1fc992668f8486eb8cf598f6938923c22e9bb50f8338a8` |
| `LICENSE.protocol` | 659 | `e2b1c35814afb22acbddf7ff567d2b05a467e503628ffea62752bc1f3fa2595c` |

Do not patch these files in place. Adapter changes belong in first-party boot
code; a dependency update adds a new versioned directory and switches
`OS1_LIMINE_DIR` in [CMake](../CMakeLists.txt) so the old/new snapshot is clear
in review.

### Upgrade procedure

1. Obtain the complete upstream release bundle and verify its published source
   and licenses outside the repository.
2. Add a new `third_party/limine/vX.Y.Z/` directory, refresh the size/checksum
   table and dependency lock, then update `OS1_LIMINE_DIR` once—never mix
   artifacts across releases.
3. Review protocol-structure changes against every file in `src/boot/limine/`.
4. Run `tools/verify.sh full` and inspect all UEFI smokes, the Limine shim
   contract assertions, ISO contents, and the BIOS matrix for shared-kernel
   regressions.

The legacy BIOS frontend is a diagnostic fallback, not a replacement for the
default UEFI artifact. Removing Limine therefore requires another UEFI loader
that produces the same reviewed `BootInfo` and image/layout guarantees.

## GoogleTest

### Boundary and local policy

GoogleTest is compiled only by [`tests/host/CMakeLists.txt`](../tests/host/CMakeLists.txt).
Production kernel code cannot include it or depend on `tests/host/support/`.
There are no local submodule edits; compiler-specific warning suppression and
test integration stay in the host CMake project.

GoogleTest has no freestanding or image footprint. Its relevant cost is host
build time and compatibility with the supported C++20 host compilers.

### Upgrade procedure

1. Review upstream release notes, update only the GoogleTest gitlink, and keep
   `git diff --submodule=log` in the review evidence. Update the dependency lock
   and this pin in the same change.
2. Confirm the target resolves to the pin recorded above and that no local
   submodule changes exist.
3. Configure a clean host-test build and run `tools/verify.sh fast` on the
   supported local compiler plus the Linux CI compiler.
4. Inspect test discovery/count changes; a changed count needs a source-backed
   explanation and an update to [QUALITY.md](QUALITY.md), not a relaxed check.

Removing GoogleTest requires migrating all host tests and discovery semantics;
it must not reduce the host evidence classes in `QUALITY.md`.

## Pin Verification

```sh
git submodule status -- third_party/acpica third_party/googletest
git -C third_party/acpica describe --tags --exact-match
git -C third_party/googletest describe --tags --exact-match
shasum -a 256 third_party/limine/v11.4.0/*
```

Dependency changes always require explicit review because ACPICA and Limine
cross kernel/boot trust boundaries and GoogleTest defines the host evidence
harness. `tools/verify.sh full` is necessary but does not replace release-note,
license, footprint, or adapter-boundary review.
