# System lifecycle and shell durability

This change adds controlled shutdown, reboot, and shell supervision. Completed shell commands synchronize persistent writes before the next prompt.

```text
shell command -> native SystemControl.sync -> ext2 admission guard -> flush -> clean marker
shutdown/reboot -> native authority check -> storage freeze and sync -> ACPI transition
init -> spawn shell -> wait and reap -> restart shell
```

## Accepted behavior

- Killing QEMU can interrupt a write or leave the filesystem marked unclean.
- Synchronize after commands that can change files, including external programs and output redirection.
- Synchronize again before shutdown or reboot. Refuse a transition when synchronization fails.
- Freeze persistent filesystem access before the final synchronization. Do not permit subsequent writes during the transition.
- Restart the top-level shell after exit. Nested shells and scripts retain normal exit behavior.
- Print shell start and end messages on the diagnostic stream.
- Keep ACPI observation passive. Activate firmware power methods only for an explicit transition.

## Impact assessment

| Area | Requirements and effect |
| --- | --- |
| Scope | GOALS Storage and filesystems: preserve the existing ext2 profile and runnable shell. |
| Boot and platform | ARCHITECTURE Persistent QEMU storage: qualify both BIOS and UEFI transitions; physical hardware remains separate. |
| Memory and isolation | Native API implemented profiles: use a separate SystemControl right on the bootstrap root capability. Reject attenuated callers. |
| Concurrency | Serialize shutdown admission and ext2 access. Freeze the disk before the final flush; avoid shutdown with active disk operations. |
| Interfaces | Add a native kernel provider and explicit method IDs. Keep filesystem and launch layouts unchanged. |
| Runtime resources | Measure command latency, disk cleanliness, image size, and shell restart behavior against the current build. |
| Validation and documents | Add host contract checks and QEMU lifecycle tests. Obtain an independent adversarial review. |

## Implementation sequence

1. Add bounded native SystemControl methods and capability checks.
2. Add explicit ACPI shutdown and reset preparation without changing passive boot observation.
3. Add filesystem freeze and synchronization under the existing admission guard.
4. Add shell commands and command-completion synchronization.
5. Keep init alive to restart and reap shells.
6. Test abrupt termination after completed writes, shutdown, reboot, errors, and nested shell exit.

## Durability limits

Ext2 has no journal. Synchronization reduces the dirty interval but does not make interrupted metadata changes atomic.
External programs can write while a shell waits. Those writes remain unclean until the command ends or the program synchronizes them.
A failed flush must not produce a successful shutdown result or a false clean-state claim.

## References

- [Ext2 profile](../filesystems/ext2.md)
- [Program deployment](../filesystems/program-deployment.md)
- [Native API index](../native-api/README.md)
- [ACPI sleep states](https://uefi.org/specs/ACPI/6.6/16_Waking_and_Sleeping.html)

The project prose rules were checked. Full standard dictionary verification remains unavailable.

## Implementation and validation record

The implementation uses a native SystemControl provider. It adds no syscall number or startup-record field.
Init waits for the shell and reaps it before restart. Lifecycle messages use descriptor 2.
Interactive QEMU targets now permit guest reset and poweroff.

The first independent review found three issues. All three were confirmed and corrected.

| Finding | Correction and regression coverage |
| --- | --- |
| Reduced-hardware S5 can poll indefinitely | Reject that profile before namespace activation. A host test checks repeated rejection. |
| Automatic sync incorrectly required INSPECT | Bind SystemControl directly. A guest test retains control but removes root inspection rights. |
| Current ABI profiles retained the old rights mask | Update all three profiles to 8191 and distinguish filesystem descendant rights. |

The second independent review found no remaining defects. Final full-suite results are recorded below when available.
Hardware-reduced shutdown remains unsupported. Legacy ACPI shutdown and FADT reset pass QEMU tests; physical hardware remains unmeasured.

The initial full-suite attempt exposed local Unix-socket sandbox restrictions in monitor and fault-injection tests.
The final suite runs with local socket access. No production checks were weakened.
A timestamp test requested the new power right as a filesystem descendant right; its previous filesystem ceiling is now explicit.
An interactive input test raced an implicit-text warning. Quoting its path preserves the input assertions and removes that ambiguity.

## Runtime comparison

Baseline revision: `7501776997ff13ac7ffab87b74e514a5439bad9d`.
Candidate: this branch's system-lifecycle working diff, with the second-review corrections.
The host uses macOS arm64, GCC x86_64-elf 11.2.0, Debug configuration, and QEMU 11.1.3.
The guest uses q35, four CPUs, 128 MiB RAM, virtio-blk, and fresh OVMF variables for each run.
Firmware files are `/opt/homebrew/share/qemu/edk2-x86_64-code.fd` and `edk2-i386-vars.fd`.
The workload executes five echo commands with persistent-file replacement. Each command waits for the next prompt.
Each version starts from the same copied data image. Three repetitions ran per version.

| Measurement | Baseline | Candidate | Absolute difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| Five writes, median | 32.593958 ms | 34.238916 ms | +1.644958 ms | +5.0468% |
| ISO size | 4345856 bytes | 4352000 bytes | +6144 bytes | +0.1414% |
| Slab pages before workload | 166 | 167 | +1 page, 4096 bytes | +0.6024% |
| Slab pages after workload | 167 | 168 | +1 page, 4096 bytes | +0.5988% |
| Live process objects | 2 | 3 | +1 | +50% |
| Clean marker after workload | 0 of 3 runs | 3 of 3 runs | +3 runs | Undefined from zero baseline |

Baseline write times were 32.593958, 35.190166, and 32.063916 ms.
Candidate write times were 32.039667, 35.300625, and 34.238916 ms.
Slab and process counts matched across all three runs. No allocator failures occurred.
The extra process is the retained init supervisor. Timing samples are small and do not establish physical-hardware performance.
The candidate trades additional flush latency and a retained supervisor for shorter dirty intervals and shell recovery.

Evidence is in `build/system-lifecycle/measurements.json`, `measurements.log`, and `measure-*.writes.log`.
The reproduction script is `build/system-lifecycle/measure.py`.

A separate idle-memory observation used the same firmware and machine configuration, once per version.
Allocated physical memory increased from 21123072 to 21254144 bytes: 131072 bytes, or 0.6205 percent.
Both runs reported 24676 managed pages. Free pages changed from 19519 to 19487.
This measurement includes the retained init process and changed kernel allocations. It is not only slab memory.
See `build/system-lifecycle/memory.json` and `memory-*.idle.log`.

## Final checks

| Command | Result |
| --- | --- |
| `cmake --build build` | Passed |
| `cmake --build build --target os1_usb_image` | Passed |
| `cmake --build build-host-tests` | Passed |
| `ctest --test-dir build-host-tests --output-on-failure` | 499 of 499 passed |
| `ctest --test-dir build --output-on-failure -j 2` | 60 of 60 passed; local socket access enabled |
| Local Markdown link check and `git diff --check` | Passed |

The full QEMU and contract suite includes both firmware paths, SMP, USB storage failures, input, timestamps, deployment, and lifecycle tests.
Host tests cover denied control authority, malformed requests, freeze ordering, failed synchronization, failed platform transition recovery, and reduced-hardware rejection.
Guest tests verify automatic synchronization without INSPECT, denied power authority, shell restart, nested exit, SIGINT recovery, reboot, and shutdown.

Final logs are `build/system-lifecycle/host-tests.log` and `qemu-tests.log`.
The two lifecycle transcripts are `build/artifacts/smoke-system-lifecycle-{bios,uefi}.{interrupt,power}.log`.
No physical-device test was performed. A hardware test must qualify shutdown and reset separately on each target machine.

| Artifact | SHA-256 |
| --- | --- |
| Baseline ISO | `4ab54e642746652be2458595666579c1647dee2dea8b42f4c97892a315f09b38` |
| Candidate ISO | `e22c15544eec05fd67962898918cf1a378136bb24ed5fd658319cb9496e21126` |
| Candidate BIOS image | `c169e581a61da589155fff6fb5fa4407de43f0039d4001cdc812e9568762bbfb` |

Changed owned C/C++ files were formatted with the project configuration. Unrelated formatting changes were excluded from the diff.
