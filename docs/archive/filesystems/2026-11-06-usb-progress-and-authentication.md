# USB progress and authentication handoff

The initial USB writer now reports each stage and elapsed waiting time.
The sudo handoff explicitly enables line input and Enter-to-newline conversion.

```text
menu exit -> normalize terminal input -> sudo authentication
  -> starting writer -> image check -> device check -> unmount
  -> write percentage -> cache synchronization -> readback percentage -> eject
```

## Observations and reproduction

The user's captured output ends with sudo requesting a password and reporting cancellation.
It contains no writer-start message. The privileged write did not start in that captured run.
The user also reports that pressing Enter produced no visible response.

A normal controlling-terminal probe passed both before and after the actual image-build path.
Disabling `ICRNL` reproduced the reported symptom: the menus accepted Enter, but the password reader did not receive a complete line.
The test uses a dummy token and a separate `/dev/tty` password-reader process.
It does not invoke sudo or inspect a real password.
The source of the user's disabled or otherwise affected terminal input remains unconfirmed.

A separate regression confirmed that the old writer produced no feedback before disk inspection after authentication.
The old copy loop also gave no byte progress or cache-synchronization status.
These are distinct from failure to complete sudo authentication.

## Change and impact

| Area | Requirement and effect |
| --- | --- |
| Scope | GOALS: Simplicity first; make the current operation visible and preserve usable terminal input. |
| Boot/platform/interfaces | No kernel, BootInfo, Native API, image-format, or partition-layout changes. |
| Memory/isolation | Password input remains entirely within sudo. Progress uses stderr and never enters snapshot stdout. |
| Concurrency | One short-lived host reporter thread prints once per second. Context exit stops and joins it, including failures. |
| Host terminal | Enable canonical input, signals, and CR-to-NL translation for sudo; preserve prior settings after return. |
| Runtime resources | Compare the same 162 MiB file-copy/readback workload before and after progress reporting. No new disk accesses are required by reporting. |
| Validation | File-backed transfer, blocked cache synchronization, failure cleanup, output separation, and real controlling-terminal input tests. |

Status messages begin as soon as the privileged writer starts.
Byte percentages count completed transfer blocks. Waiting messages report elapsed time without claiming device completion.
A successful write reaches synchronization, readback verification, metadata-tail checks, and ejection before reporting success.
The input correction does not collect, store, or echo the sudo password.

## Validation and measurements

Five progress and authentication tests pass, including normal input, disabled `ICRNL`, and cancellation cases.
Twenty-one existing USB menu/policy tests and nine managed-image tests also pass.
The four affected USB CTest contracts passed in 2.88 seconds.
Command: `ctest --test-dir build -R 'os1_usb_.*contract' --output-on-failure`.
Kernel builds and QEMU tests were not repeated because this correction changes only host feedback and terminal handoff.
No physical device was accessed. Real sudo/PAM authentication remains untested in the agent environment.

Baseline: the working tree immediately before this correction, including the uncommitted USB data-deployment changes atop `22e2865`.
Candidate: the same tree with progress and terminal normalization.
Environment: macOS 26.6.2 ARM64, Python 3.15.2; three repetitions per version on regular temporary files.
Both paths copy 169869312 bytes, flush with fsync, and verify the SHA-256 readback.
The candidate includes reporter thread creation and shutdown; output is captured to a memory stream.
Baseline median: 0.082953 seconds. Candidate median: 0.092753 seconds.
Change: +0.009799 seconds, or +11.81%.
These local-file timings do not measure USB performance. No numeric latency gate applies to this feedback change.
Peak memory and physical-device timing remain unmeasured.
Evidence is retained in `build/usb-progress-evidence/`.

## Review

An independent read-only review found no blocking defect in the isolated correction.
The review covered progress ordering, output separation, thread cleanup, terminal restoration, privilege boundaries, and validation evidence.
Real sudo authentication and physical USB behavior remain untested.
The complete ASD-STE100 dictionary was unavailable; prose follows the repository structure and terminology rules.
