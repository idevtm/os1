# Strategic CI and mandatory local validation

This change moves complete validation to the developer host and keeps representative checks on GitHub.
It preserves all existing tests and adds one full local command.

```text
full local acceptance -> independent review -> push -> strategic hosted checks
```

## Requirements and impact

Baseline: `c8dc8dccbeac6d2ed06ad8a90a8cb39436802b2b`, after PR #46 merged into PR #45.

| Area | Assessment |
| --- | --- |
| Scope | GOALS: Testability and observability. Keep a simple local entrypoint and fast hosted feedback. |
| Boot and platform | Architecture: Boot profile selection and Build Outputs And Boot Artifacts. Retain BIOS, UEFI, and diagnostic checks locally. |
| Memory and isolation | No guest allocation or permission changes. Tests retain private fixtures and allocator checks. |
| Concurrency | Keep serial execution and existing shared-artifact locks. Test selection does not change SMP behavior. |
| Interfaces | No syscall, native ABI, image-format, or program changes. Native API profiles retain their current authority. |
| Runtime resources | Reduce hosted test executions and remove its second image build. Full local validation still consumes complete-suite resources. |
| Validation and documents | Require the full local command before push or completion. Update AGENTS, README, GOALS, and the testing guide. |

Guest architecture and ABI contracts are unchanged.
The architecture's CI section now describes the changed validation responsibilities.
No C or C++ files change.

## Plan

1. Label six representative QEMU checks and require their registration in CI configuration.
2. Keep host tests on GitHub and move diagnostic-image checks into the complete local command.
3. Require final-tree local validation and preserve explicit failure evidence.
4. Run the local command and the exact hosted selection.
5. Compare elapsed time and obtain an independent review.

## Known baseline failures

Hosted run 42675847104 passed 590 host tests and 77 of 78 normal-image checks.
The matrix took 957.54 seconds.
The UEFI log-export test could not find `/tmp/log` after rescue export.
That test remains in the strategic selection.
The earlier recursive-copy `OS1_ERR_CHANGED` failure also remains unresolved, despite a later passing run.
This task does not repair either guest behavior.

## Validation evidence

The final local command returned status zero.
All 590 host tests, 79 normal-image checks, and three diagnostic checks passed.
The exact GitHub QEMU selection also passed all six checks.

| Selection | Checks | Elapsed time |
| --- | ---: | ---: |
| Full host suite | 590 | 19.68 seconds |
| Full normal-image suite | 79 | 616.92 seconds |
| Diagnostic capture suite | 3 | 10.42 seconds |
| Strategic normal-image subset | 6 | 86.66 seconds |

The strategic subset reduces normal-image test time by 530.26 seconds, or 85.95 percent, in this local comparison.
Each row records one completed run on the final implementation.
The full and strategic runs use identical normal images and execute sequentially.
The comparison changes workload coverage; it does not establish faster individual tests.
Hosted setup, compilation, and total workflow duration remain unmeasured for this change.
The new workflow has not been pushed or executed on GitHub.
Physical-device behavior is unchanged and was not tested for this host-tooling task.

The local runner adds one contract to the previous 78-check normal-image suite.
The UEFI log-export and recursive-copy checks passed locally.
These passes do not resolve their earlier intermittent hosted failures.

The first independent review found two P2 defects.
The host configure command could retain cached `OS1_KMEM_DEBUG=OFF`.
The architecture also described obsolete CI coverage.
Both defects are corrected, and the correction review found no further implementation defects.

Three runner contract tests check required flags, complete profile execution, test failure propagation, and build failure termination.
The allocator-flag contract failed before the correction and passed afterward.
An isolated CMake configuration checks all six labels and rejects a missing required test.
These command contracts do not replace the real local suite.

The preliminary run was stopped before the review corrections.
A later sandboxed run failed because QEMU could not bind local monitor sockets.
The error was `Operation not permitted`; it did not establish a guest regression.
The restricted run was stopped, and its evidence remains in `build/strategic-ci-sandbox-evidence/`.
The final local command runs with permission to create those sockets.

Measurement host: Apple M4 Max, sixteen physical cores, 64 GiB RAM, macOS 26.6.2.
Tools: CMake 4.4.5, Python 3.15.2, QEMU 11.1.3, and GCC 11.2.0.
Guest tests retain their existing q35, four-CPU configurations under TCG.
Python guest helpers specify 128 MiB RAM.
The CMake smoke runner uses QEMU's default RAM size.
The installed QEMU reports 134,217,728 bytes through `query-memory-size-summary`, also 128 MiB.
The query result is retained in `build/local-validation/qemu-memory.jsonl`.
Both normal and diagnostic builds enable allocator checks.
Normal tests enable debugcon; diagnostic capture disables it.
Normal UEFI tests use `/opt/homebrew/share/qemu/edk2-x86_64-code.fd` and `edk2-i386-vars.fd`.
Their SHA-256 values are `33090cc07675baa5190d9f1e84bf5176b33bcbfa9bacac522961150cdb6dbb2a` and `5d2ac383371b408398accee7ec27c8c09ea5b74a0de0ceea6513388b15be5d1e`.
BIOS tests use the bundled QEMU firmware.

| Normal artifact | Bytes | SHA-256 before and after both selections |
| --- | ---: | --- |
| `os1.iso` | 4,311,040 | `c129548537fbbe55bdadbaec353c0cd2f6e2a147d32437ec26ced4db008aba03` |
| `os1.raw` | 1,474,560 | `7d5fd79d8ea19b66485a465df1f51d0976e0454f2cc2cbb22c0e4667e40e08ca` |
| `program-test-disk.raw` | 34,603,008 | `95ab831dbc0134e4c03180f3cd651b065155dd9579f36f9559132bf78a21204c` |

The final executable-source digest is `657ad52c9504fb8f8697d699a45031a5823c1cc2e06537accd8db998c53deb41`.
It covers `.github/workflows/ci.yml`, `CMakeLists.txt`, `scripts/check-local.sh`, and `tests/scripts/test_check_local.py`.
Hash sorted relative paths, a NUL byte, complete file bytes, and another NUL byte for each file.
Documentation-only evidence updates do not change this digest.

Commands:

```sh
./scripts/check-local.sh
ctest --test-dir build --output-on-failure --no-tests=error -L '^ci$' --output-log build/local-validation/strategic.log
python3 -I -B tests/scripts/test_check_local.py
bash -n scripts/check-local.sh
git diff --check
```

CTest evidence is in `build/local-validation/{host,full,diagnostics,strategic}.log`.
The full build transcript is `build/strategic-ci-local-run.log`.
The subset transcript is `build/strategic-ci-subset-run.log`.
The allocator negative control is `build/strategic-ci-negative.log`.
Selection-guard evidence is in `build/strategic-selection-contract/`.
Shell syntax, YAML syntax, whitespace, and local document link checks pass.
No formatter applies because the task changes no owned C or C++ source.

The complete ASD-STE100 Issue 9 dictionary is unavailable; full language compliance is not established.
