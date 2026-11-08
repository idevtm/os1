# ACPI Embedded Controller support

This implementation adds bounded EC OperationRegion access for explicit shutdown. It addresses the laptop's reported missing `EmbeddedControl` handler for `ECOR`.
The [hardware retest](#hardware-result--2026-11-08) confirms successful laptop shutdown.

```text
shutdown -> namespace initialization -> discover PNP0C09 and validate _CRS/_GLK
    -> install device-scoped handlers -> ACPICA _REG connection -> _PTS(5)
    -> serialized EC byte transactions -> existing S5 power entry
```

## Scope and impact

| Area | Requirements and effect |
| --- | --- |
| Project scope | [GOALS: Real hardware enablement](../../GOALS.md#real-hardware-enablement): implement the missing firmware transport without bypassing `_PTS`. |
| Boot and platform | [ARCHITECTURE: System lifecycle](../ARCHITECTURE.md#system-lifecycle): activate EC support only during explicit shutdown, after namespace initialization. Keep ordinary observation passive. |
| Memory and isolation | Use four static controller slots and a bounded resource buffer. Validate ports, widths, addresses, and duplicate resource claims. |
| Concurrency | Admit one transaction per controller. Poll with interrupts enabled; return an error for concurrent access. Keep handler contexts until namespace termination. |
| Interfaces | Preserve SystemControl records, rights, and status codes. Register ACPICA handlers within each discovered EC device. |
| Runtime resources | Bound poll attempts and stale-output draining. Measure image size, memory, and command latency against the preceding diagnostic image. |
| Validation and documents | Test a real AML EC region with a simulated port device, protocol errors, resource rejection, and both boot paths. Obtain independent review. |

The [Native API index](../native-api/README.md#implemented-profiles) identifies SystemControl as an implemented profile. This change adds no new public ABI.
BootInfo, user-memory mappings, descriptors, and IPC layouts remain unchanged because the adapter uses existing kernel firmware interfaces.

## Initial profile

- Discover `PNP0C09` devices through ACPICA. Check `_STA` separately because ACPICA discovery does not require the enabled bit.
- Accept two distinct, single-byte System I/O resources from `_CRS`: data first, status/command second.
- Accept absent or zero `_GLK`. Reject required Global Lock ownership because the current ACPICA build cannot implement it safely.
- Install all handlers before requesting ACPICA `_REG` connection methods.
- Support 8-, 16-, 32-, and 64-bit accesses within the 256-byte EC address space.
- Use read and write commands without burst mode. Leave SCI queries, `_Qxx` dispatch, ECDT early activation, and memory-mapped controllers for later work.
- Keep the controller unavailable after a transport failure. Do not retry an uncertain partial transaction.
- Leave partially completed writes visible to firmware; multi-byte access is not an atomic device transaction.

## Implementation sequence

1. Add validated controller discovery and bounded port transactions.
2. Register the EC address-space handler during explicit shutdown preparation.
3. Reset driver state after namespace termination.
4. Add protocol and AML integration regression tests.
5. Build both boot images and run host and QEMU checks.
6. Record measurements and obtain independent review before hardware retesting.

## Evidence and limits

The laptop photo identifies `ECOR` under the `EC0` firmware path. It does not supply `_CRS` or `_GLK` values.
The preceding diagnostic image is the baseline; its ISO SHA-256 is `d39b66a745523843ec0bcb89f0d988625dbf449747105940d5d92143a0489e19`.
It contains commit `7df6f85` plus the reviewed diagnostic working changes. Copies and the prior diff are retained in `build/acpi-ec/`.

The [ACPI EC specification](https://uefi.org/specs/ACPI/6.5/12_Embedded_Controller_Interface_Specification.html) defines commands, resources, and Global Lock requirements.
The [SystemControl ABI](../native-api/abi/system-control-abi.md) defines shutdown authority and failure behavior.
Full ASD-STE100 dictionary verification remains unavailable.

## Transaction contract

```text
read:  IBF clear -> command 0x80 -> IBF clear -> address -> OBF set -> data
write: IBF clear -> command 0x81 -> IBF clear -> address -> IBF clear -> data -> IBF clear
```

Each handler call admits one transaction for its controller. Contending calls return `AE_TIME` without changing the active transaction.
The driver does not disable interrupts while it polls. Each host write includes a one-microsecond delay before the next status observation.
A shared 10000-attempt budget permits at most 100 milliseconds of explicit polling delays per handler call.
Write delays add at most 24 microseconds for a 64-bit write. Scheduling delays can increase elapsed time beyond these explicit delays.
Before each byte command, the driver drains at most 16 stale output bytes. An additional pending byte causes failure.
Read results become visible only after all requested bytes succeed. Write failures can leave earlier bytes modified.
Any transport failure disables later transactions on that controller until reboot. Invalid arguments and admission conflicts do not disable it.

Discovery uses four static controller slots and a 512-byte resource buffer. Duplicate ports, unsupported resources, and oversized descriptions fail before protocol traffic.
Absent `_GLK` means exclusive ownership under the ACPI contract. Nonzero `_GLK` returns `global-lock-required` before handler installation.
All handlers are installed before ACPICA executes connection methods. Handler contexts remain valid until namespace termination completes.
ACPICA's public connection API discards AML `_REG` failures. The driver instead walks EC regions and retains the internal connection helper's status.
This preserves ACPICA connection flags and stops shutdown when a connection method fails. Orphan `_REG` methods also retain their evaluation status.
The driver checks each controller's recorded transport error after connection. A failed initialization remains failed until namespace termination or reboot.

Namespace object initialization still precedes EC installation. Firmware that requires EC access during early initialization needs a later activation design.
The current implementation does not claim general runtime EC service, thermal management, battery support, or event delivery.

## Hardware retest

The November 8 laptop report identifies `ECOR`, `EC0.RRAM`, `EC0.ECOS`, `OEMS`, `PTS`, and `_PTS` in the failed call chain.
The new handler addresses the missing EC transport. The laptop's resource layout and Global Lock requirement remain unverified.

1. Use the normal `os1.sh` USB menu to update the boot image.
2. Boot the Asus G74SX with the updated image.
3. Run `shutdown`.
4. If shutdown fails, photograph the complete ACPICA and system diagnostic block.
5. After a failure, verify that a persistent test file can be written and read.

Successful simulated transactions do not prove physical shutdown. A transport error requires reboot before another EC attempt.

## Validation and review record

The host suite passes all 511 tests. Seven EC tests use the real ACPICA interpreter with generated AML and a simulated port controller.
They cover shutdown-time region connection, firmware writes, declared ports, byte order, invalid accesses, admission conflicts, and persistent handler registration.
Failure cases cover stale responses, input and output timeouts, partial reads, unsafe resources, disabled devices, and failed connection methods.
Orphan connection methods receive separate success and failure coverage. Transport failure prevents subsequent attempts from using the ports.

The first independent review found two defects. Regression tests reproduced both before correction.
The driver now preserves connection-method failures and explicitly checks the `_STA` enabled bit.
The second independent code review accepted these corrections and their interactions. It found no remaining implementation defects.
The failed regression transcript is `build/acpi-ec/review-regression-before.log`.
The successful host transcript is `build/acpi-ec/host-tests.log`.

The public ACPICA connection API suppresses AML evaluation errors. The adapter uses ACPICA's internal connection helper to preserve its bookkeeping and status.
This dependency needs review when the ACPICA version changes. No third-party sources were modified.

### Build and regression evidence

Kernel and USB image builds passed. The existing image-size, overlap, and 16 KiB post-image reserve checks remain unchanged.
All 60 QEMU and contract checks passed before the review corrections. Eight affected checks passed again with the corrected image.
The final checks cover BIOS and UEFI boot, passive observation, SMP balancing, and system lifecycle behavior.
QEMU has no EC in these configurations. These checks protect existing behavior; the host AML tests validate the new EC path.

```sh
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
cmake --build build
cmake --build build --target os1_usb_image
ctest --test-dir build --output-on-failure -j 2
ctest --test-dir build --output-on-failure -R 'os1_smoke_system_lifecycle|os1_smoke_observe|os1_smoke_balance|os1_smoke$|os1_smoke_bios$'
python3 build/acpi-ec/measure.py
python3 build/acpi-ec/measure-shutdown.py
```

The full suite required local Unix-socket access outside the sandbox. The final focused checks ran inside the sandbox.
Logs are in `build/acpi-ec/`: `host-tests.log`, `qemu-tests.log`, `final-qemu-tests.log`, `final-kernel-build.log`, and `final-usb-build.log`.
Initial correction builds exposed fixture declaration duplication and an ACPICA header-order dependency. Both were corrected before the final checks.
The existing host assert-macro warning remains unrelated. Source formatting and local document-link checks passed; unrelated formatting was excluded.

### Comparable measurements

The baseline is the preceding diagnostic working tree on `7df6f8537dfc4e397d97dd3e2bb8fbdb6fc47225`.
The candidate adds the EC source changes and review corrections. Its complete code patch is `build/acpi-ec/candidate-source.patch`.
The patch SHA-256 is `ab0ea18dca9aa797d756f43eab87b8d572539929fead080944a11681d35a6ae8`.

Both versions use macOS 26.6.2 arm64, x86_64-elf GCC 11.2.0, Debug, `OS1_KMEM_DEBUG`, and `OS1_QEMU_DEBUGCON`.
The guest uses QEMU 11.1.3, q35, four CPUs, 128 MiB, virtio-blk, and fresh firmware variables.
Firmware uses Homebrew `edk2-x86_64-code.fd` and `edk2-i386-vars.fd` from `/opt/homebrew/share/qemu/`.
Each trial starts from the same copied data image. No measurement writes to the user's persistent image.

Three trials per version measure five persistent-file replacements. Each command waits for the next prompt.
Three additional trials per version measure shutdown from command submission until QEMU process exit.
Shutdown measurements include host scheduling and QEMU exit overhead. They do not measure EC transaction latency because this QEMU configuration has no EC.

| Measurement | Baseline | Candidate | Absolute difference | Relative difference |
| --- | ---: | ---: | ---: | ---: |
| Five replacements, median | 28.154001 ms | 30.532292 ms | +2.378291 ms | +8.4474% |
| Shutdown to QEMU exit, median | 11.028208 ms | 9.282624 ms | -1.745583 ms | -15.8284% |
| ISO size | 4362240 bytes | 4362240 bytes | 0 bytes | 0% |
| BIOS kernel file | 405760 bytes | 405760 bytes | 0 bytes | 0% |
| Reserved kernel window | 602112 bytes | 602112 bytes | 0 bytes | 0% |
| Allocated physical memory before workload | 21266432 bytes | 21266432 bytes | 0 bytes | 0% |
| Slab pages before and after workload | 168 | 168 | 0 pages | 0% |

The new code fits existing image alignment and reservation space. Static controller records add bounded internal storage without changing these measured sizes.
Both versions report 19484 free pages from 24676 managed pages and three process objects. No allocator failure occurred.
All six file-workload trials reached a clean filesystem marker. Raw samples and transcripts remain in the same evidence directory.
Short samples do not establish a performance improvement or regression. No numerical latency requirement applies to this initial profile.
Physical EC timing remains unmeasured. The subsequent [hardware retest](#hardware-result--2026-11-08) confirms successful laptop shutdown.

The persistent data image retained SHA-256 `7a9bb6b5b3eec7360dd447da5bab87d4e643a9cc0eced62a88b7a2a685208a48`.

| Final artifact | SHA-256 |
| --- | --- |
| UEFI ISO | `6c6b727ae07603f613a855403d1337461448259636239a0ad4c3273c9db3fcd9` |
| BIOS image | `2d52bf89ba040a4ea96b37ece4155db7604d3c9c8915c8ec74ef9c22a6d00848` |

The final independent review verified the corrected implementation, documentation, test evidence, measurements, and artifact hashes.
Both original findings remain corrected. No blocking findings remain. The subsequent hardware result follows.


## Hardware result — 2026-11-08

The user confirmed that `shutdown` now powers off the Asus G74SX. This retest follows the EC implementation and resolves the reported laptop shutdown failure.

| Item | Recorded result |
| --- | --- |
| Machine | Asus G74SX laptop |
| Operation | Shell `shutdown` command |
| Result | Successful shutdown, reported by the user |
| Previous failure | Missing `EmbeddedControl` handler for `ECOR`; `_PTS` aborted with `AE_NOT_EXIST` |
| Tested change | EC support from this implementation, reported after the laptop retest |
| Evidence limits | No new image hash, timing, firmware revision, or repeat count was supplied. |

The build hashes above identify local artifacts. They do not independently identify the image deployed for this hardware retest.
This result confirms the reported shutdown operation on this laptop. It does not qualify SCI events, other EC controllers, or required Global Lock support.

### Recording impact and validation

This documentation update records hardware evidence for GOALS **Real hardware enablement** and ARCHITECTURE **System lifecycle**.
The SystemControl implemented profile remains unchanged. Earlier failure reports remain as historical evidence.
Boot, memory, isolation, concurrency, interfaces, and runtime resources are unaffected because this update changes only documentation.
Existing implementation measurements remain valid. No additional runtime benchmark or kernel build is required for this record.
Validation checks the reported result, current status statements, local links, and whitespace. Independent review covers the documentation delta and PR description.
Full ASD-STE100 dictionary verification remains unavailable.
