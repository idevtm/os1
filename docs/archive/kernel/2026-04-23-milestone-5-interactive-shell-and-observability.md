# Milestone 5 Design — Interactive Shell And Observability

> generated-by: Codex (GPT-5) · generated-at: 2026-04-23 · git-commit: `4decbecdc682eef29cd40eef857b3cc2d9559215`

## Purpose

Milestone 5 turns the current initrd demo userland into a real operator-facing environment.

Today `os1` can:

- boot through both frontends
- load protected user ELFs from the initrd
- switch between multiple user threads
- service a very small syscall ABI
- fall back to an idle thread once user work is done

But it still cannot:

- accept user input in ring 3
- present a shell prompt
- inspect kernel-managed state from user mode except through boot logs
- expose structured observability data through a stable user API

This milestone should close that gap with the smallest practical design:

- boot into an interactive shell immediately
- make the shell reachable as both `/bin/init` and `/bin/sh`
- accept console input from both keyboard and serial RX
- expose observability through explicit kernel API calls
- add minimal initrd-backed program execution
- ship the shell in the initrd next to the other user programs

## User-Visible Outcome

After this milestone, a normal boot should land at a prompt such as:

```text
os1>
```

The operator should be able to type commands and get structured system information without relying on serial boot logs.

The same shell path should also be scriptable over serial so CI and local smoke runs can drive commands and assert responses.

The first shell is intentionally narrow:

- one foreground shell
- one console
- line-oriented interaction
- built-in commands plus minimal initrd-backed command launch
- no `fork`, no pipes, no filesystem shell semantics yet

That is the fastest route to a useful terminal-first environment.

## Current Baseline And Constraints

The current tree matters because it constrains the shortest correct implementation:

- the kernel hardcodes `/bin/init` as the first user program
- there is no `exec`, `spawn`, `fork`, or file-descriptor model beyond `write`
- the user syscall ABI currently exposes only `write`, `exit`, `yield`, and `getpid`
- keyboard input is decoded in-kernel, but userland cannot read it
- the initrd currently ships `/bin/init`, `/bin/yield`, and `/bin/fault`
- `run` and `run_bios` are interactive through the QEMU display window, but their serial output is logged to files rather than attached to the terminal

Those facts strongly suggest that Milestone 5 should not try to solve “full process launching” and “real shell semantics” at the same time.

## Scope

This milestone covers:

- a minimal interactive shell program in the initrd
- a user-readable console input API
- shared keyboard and serial-RX console input
- a read-only observability API exposed by the kernel
- minimal initrd-backed program execution
- built-in shell commands that consume that API
- a shell executable implemented in freestanding `C++20`
- initrd packaging changes so the shell is present by default

This milestone does not cover:

- `fork`
- a filesystem-backed `execve`
- launching arbitrary user programs from outside the initrd
- general filesystems
- pipes, redirection, background jobs, or signals
- a POSIX-compatible terminal subsystem
- multi-terminal session management

## Design Goals

1. Reach an interactive shell as soon as possible after boot.
2. Make the shell a real user-mode program, not a kernel console command loop.
3. Expose observability through stable kernel API contracts rather than by parsing debug logs.
4. Preserve and expand automated testing through a serial-drivable shell path.
5. Avoid baking shell-specific hacks into the kernel that will block later process or filesystem work.
6. Keep the first shell implementation small enough to land quickly and test reliably.

## Recommended Technical Direction

### 1. Boot Directly Into The Shell

The kernel should continue to boot `/bin/init`.

Because there is still no generic user-space launch path today, the shortest clean design is:

- implement the first shell as a normal user program
- build it as `sh.elf`
- stage that same ELF into the initrd as both `/bin/init` and `/bin/sh`

That keeps the kernel boot contract unchanged while making the shell the first visible user program.

The existing demo programs should remain in the initrd:

- `/bin/yield`
- `/bin/fault`

They remain useful as regression assets and later shell targets once execution-by-path exists.

### 2. Add A Real User Input Path

The shell cannot exist without user-readable input.

The first new syscall should therefore be:

```c
long os1_read(int fd, void *buffer, size_t length);
```

Initial semantics:

- only `fd == 0` is supported
- reads come from the active console input stream
- the call blocks until input is available
- the initial mode is canonical line input, not raw key events
- newline terminates a completed line
- backspace editing and local echo are handled in the kernel console layer
- unsupported file descriptors return an error

This is intentionally narrower than Unix `read(2)`. It gives the shell what it needs without pretending the kernel already has a general VFS or descriptor stack.

### 3. Replace The One-Character Console Input State With A Small Input Queue

The current terminal path still behaves like a demo input hook, not a multi-read API.

Milestone 5 should replace that with a bounded console input buffer owned by the kernel, for example:

- a byte ring buffer or line buffer for the active console
- populated by decoded keyboard input
- also populated by serial RX so scripted shell sessions use the same user-visible path as local interactive sessions

Recommended behavior:

- printable characters append to the pending line
- backspace edits the pending line if non-empty
- enter commits the line and wakes a blocking `read`
- the shell receives bytes, not scancodes

Recommended serial policy:

- serial RX feeds the same logical console input queue as the keyboard
- local echo is preserved so scripted sessions can assert complete transcripts
- local interactive `run` targets may still prefer the graphical window, while smoke and automation paths use serial stdio

This is the right level of abstraction for the first shell.

### 4. Expose Observability Through A Structured Read-Only ABI

The shell should not scrape serial logs, reach into shared kernel memory, or depend on raw internal headers.

The recommended API is one generic read-only observability syscall:

```c
long os1_observe(uint64_t kind, void *buffer, size_t length);
```

Why this shape is recommended:

- it keeps the syscall surface small
- it fits the existing `syscall3` user stub pattern
- it avoids a growing pile of ad hoc `get_*` syscalls
- it naturally supports versioned record formats

Each response should begin with a small header:

```c
struct Os1ObserveHeader {
    uint32_t abi_version;
    uint32_t kind;
    uint32_t record_size;
    uint32_t record_count;
};
```

Then the payload contains `record_count` fixed-size records of the requested type.

If the caller buffer is too small, the syscall should fail loudly rather than truncating silently.

### 5. First Observability Kinds

The initial shell does not need every kernel detail. It needs a few high-value snapshots.

Recommended first kinds:

```c
enum {
    OS1_OBSERVE_SYSTEM = 1,
    OS1_OBSERVE_PROCESSES = 2,
    OS1_OBSERVE_CPUS = 3,
    OS1_OBSERVE_PCI = 4,
    OS1_OBSERVE_INITRD = 5
};
```

Recommended record contents:

`OS1_OBSERVE_SYSTEM`

- boot source
- bootloader name
- console backend kind
- tick count or uptime tick counter
- total/free page counts
- process count
- runnable thread count
- discovered CPU count
- PCI device count
- `virtio-blk` presence and capacity summary

`OS1_OBSERVE_PROCESSES`

- pid
- tid
- process state
- thread state
- user/kernel flag
- CR3
- short process name

`OS1_OBSERVE_CPUS`

- logical slot
- APIC id
- BSP flag
- booted flag
- current pid/tid if present

`OS1_OBSERVE_PCI`

- segment
- bus / slot / function
- vendor id / device id
- class / subclass / prog-if
- interrupt pin / line
- summarized BAR metadata

`OS1_OBSERVE_INITRD`

- path
- file size

That is enough to make the shell genuinely useful for system inspection.

### 6. Prefer Shared UAPI Headers

Milestone 5 expands the ABI enough that duplicated definitions become a maintenance risk.

Recommended direction:

- move syscall numbers and observe-ABI structs into a shared UAPI location such as `src/uapi/os1/`
- make both kernel and user code include those headers

This is cleaner than keeping one kernel-only syscall enum and a second user-only copy that drift apart.

If that cleanup feels too wide for the first patch set, the fallback is:

- keep the current split for existing syscall numbers
- add new shared observe definitions in one small common header consumed by both sides

### 7. Add Minimal Initrd-Backed Program Execution

The shell should be installed as `/bin/sh`, but a useful shell also needs a way to launch companion programs from the initrd.

The requested `exec` primitive is directionally correct, but one architectural point matters:

- `exec` alone replaces the calling process image
- a shell that uses only `exec` cannot run a command and then return to the prompt

So the shortest correct execution model for Milestone 5 is:

- `exec(path)` replaces the current process image with an initrd program
- `spawn(path)` creates a child process from an initrd program
- `waitpid(pid)` blocks until that child exits and returns its status

That keeps the ABI small while still giving the shell normal foreground-command behavior.

Recommended first semantics:

- paths are initrd-only
- no environment block
- no pipes or descriptor remapping
- argument passing may be omitted in the first cut if it meaningfully reduces scope
- `exec` and `spawn` fail loudly for missing initrd paths or invalid ELF payloads

The shell should use:

- built-ins for observability and shell control
- `spawn + waitpid` for normal command execution
- an explicit `exec` built-in only when the user intentionally wants to replace the shell

### 8. Implement The Shell In Freestanding C++20

Moving the shell from `C` to `C++20` is reasonable and should not derail the milestone if the runtime model stays deliberately small.

Recommended constraints:

- compile the shell with `x86_64-elf-g++` in freestanding mode
- disable exceptions, RTTI, unwind tables, and thread-safe local statics
- do not depend on `libstdc++`, `libsupc++`, or hosted I/O facilities in the first cut
- avoid heap allocation unless we also add tiny `new/delete` support intentionally
- avoid global constructors until we explicitly add constructor startup support

Recommended default style:

- use `C++20` language features for clarity
- prefer fixed buffers and explicit parsing
- use small header-only utilities only when they do not pull runtime dependencies
- keep the shell ABI-facing pieces `extern "C"` compatible where needed

That means `C++20` for structure and maintainability, not “hosted C++ userland” yet.

### 9. The First Shell Should Mix Built-Ins With External Initrd Commands

Recommended initial built-ins:

| Command | Purpose | Kernel API dependency |
| --- | --- | --- |
| `help` | list supported commands | none |
| `echo` | print arguments for basic sanity | `write` |
| `pid` | show shell pid | `getpid` |
| `sys` | one-line or multi-line system summary | `observe(system)` |
| `ps` | show process/thread table snapshot | `observe(processes)` |
| `cpu` | show discovered CPU/APIC state | `observe(cpus)` |
| `pci` | show enumerated PCI devices | `observe(pci)` |
| `initrd` | show packaged initrd files | `observe(initrd)` |
| `exec` | replace the shell with another initrd program | `exec(path)` |
| `exit` | terminate shell | `exit` |

External command policy:

- if the first token is not a built-in, the shell should try initrd execution
- the first lookup rule can be simple: absolute path if the token starts with `/`, otherwise `/bin/<token>`
- the shell should launch those programs with `spawn + waitpid`

That gives the operator immediate observability and a minimal command-launch path without pretending the system already has a full Unix process model.

This also preserves the value of `/bin/yield` and `/bin/fault` as executable regression assets rather than only as passive initrd contents.

### 10. Shell Interaction Model

The shell parser should stay intentionally tiny:

- fixed maximum line length, for example `128` or `256`
- whitespace tokenization only
- no quoting in the first cut
- no history
- no tab completion
- unknown commands print a short error and return to the prompt

That is enough for an early kernel-facing operator shell.

Example session:

```text
os1> help
help echo pid sys ps cpu pci initrd exec exit
os1> sys
boot=bios console=vga cpus=4 procs=1 runnable=1 pci=7 virtio_blk=yes free_pages=...
os1> ps
pid tid state   mode   name
1   1   running user   /bin/init
os1> pci
00:01.0 8086:1237 class=06:00
00:03.0 1af4:1042 class=01:00 virtio-blk
os1> yield
[user/yield] tick 0
[user/yield] tick 1
[user/yield] tick 2
```

## Implementation Plan

Each phase below is intended to be landable as its own PR while keeping CI green.

The important rule is:

- no phase should require a future phase to prove that it works
- once a phase lands, its tests remain as regression coverage for all later phases

### Phase 1: Console Input Foundation

Deliver the smallest end-to-end user-readable input path:

- add `SYS_read`
- add a console input queue
- feed that queue from both keyboard and serial RX
- keep input canonical and line-oriented
- prove that a user process can block on `read(0, ...)` and receive complete lines

Recommended temporary userland shape for this phase:

- keep `/bin/init` extremely small
- it may act as a line-echo stub instead of a real shell yet

Why this phase stands alone:

- it validates the hardest transport boundary first
- it decouples console input work from shell parsing and process-launch work

Phase 1 validation:

- scripted serial input reaches user mode on both boot paths
- typed keyboard input still works locally
- a line such as `ping` is echoed back from user mode as a stable smoke marker

Recommended smoke additions:

- `shell input ready`
- `shell input echo: ping`

### Phase 2: Minimal C++20 Shell Prompt

Once console input works, replace the temporary init stub with the real shell:

- add a freestanding `C++20` user-program build rule
- add `src/user/programs/sh.cpp`
- stage the shell ELF as both `/bin/init` and `/bin/sh`
- add `help`, `echo`, `pid`, and `exit`
- boot directly into the shell prompt

Why this phase stands alone:

- it proves the shell runtime model independently of observability and process launch
- it validates the `C++20` toolchain choice before more features depend on it

Phase 2 validation:

- both boot paths reach `os1>` automatically
- scripted serial input can run `help`, `echo hi`, and `pid`
- the shell stays responsive after multiple commands

Recommended smoke additions:

- `shell prompt ready`
- `shell help ok`
- `shell echo ok`
- `shell pid ok`

### Phase 3: Structured Observability

Add the read-only kernel API and the corresponding shell built-ins:

- add `SYS_observe`
- define versioned observe headers and records
- implement `system`, `processes`, `cpus`, `pci`, and `initrd` snapshots
- wire the `sys`, `ps`, `cpu`, `pci`, and `initrd` commands

Why this phase stands alone:

- it expands the shell’s value without changing the process model
- failures are isolated to ABI design and data marshaling rather than launch semantics

Phase 3 validation:

- `sys` returns a coherent summary
- `ps` returns at least the shell process/thread
- `cpu` reflects ACPI-discovered CPUs
- `pci` reflects enumerated devices including `virtio-blk`
- `initrd` lists the packaged user programs

Recommended smoke additions:

- `shell sys ok`
- `shell ps ok`
- `shell cpu ok`
- `shell pci ok`
- `shell initrd ok`

### Phase 4: Initrd Child Process Execution

Add the minimal process-launch path the shell needs for normal foreground commands:

- add `SYS_spawn`
- add `SYS_waitpid`
- refactor the existing initrd ELF loader so it can create a child process by path
- let the shell execute `/bin/<name>` commands and return to the prompt afterward

Scope boundary for this phase:

- do not add `exec` yet
- keep argument passing optional if it materially delays the phase
- focus on child creation, waiting, and clean shell recovery

Why this phase stands alone:

- it proves the shell can launch companion programs without also changing current-process replacement
- it keeps the failure surface smaller than `spawn + waitpid + exec` in one PR

Phase 4 validation:

- running `yield` from the shell launches `/bin/yield`
- the child exits and the prompt returns
- running `fault` kills only the child and the prompt returns
- an unknown command reports a clean error without destabilizing the shell

Recommended smoke additions:

- `shell spawn yield ok`
- `shell spawn fault ok`
- `shell prompt resumed`

### Phase 5: Exec Image Replacement

Add current-process replacement only after child launch is already stable:

- add `SYS_exec`
- refactor the loader so it can replace the current process image by initrd path
- add an explicit shell `exec` built-in

Why this phase stands alone:

- `exec` has distinct lifecycle and teardown risks from `spawn`
- separating it keeps child-launch regressions from being conflated with image-replacement bugs

Phase 5 validation:

- `exec /bin/yield` replaces the shell process
- the requested program runs successfully
- the original shell prompt does not reappear after successful `exec`
- the system transitions cleanly once the replacement program exits

Recommended smoke additions:

- dedicated `exec` smoke, separate from the default interactive-shell smoke
- `shell exec ok`

### Phase 6: Tooling And CI Tightening

Once the behavior is stable, make the test and run ergonomics explicit:

- drive the shell through serial input in smoke tests by default
- keep all earlier phase tests as regression coverage
- optionally add dedicated shell-oriented smoke targets rather than overloading one giant test
- optionally add a dedicated interactive serial run target if the existing `run` target remains display-first

Why this phase stands alone:

- it is packaging and automation work, not core behavior work
- it can be reviewed without mixing new kernel ABI changes into test harness changes

Phase 6 validation:

- CI executes the shell-command transcript deterministically on both boot paths
- local scripted runs match CI behavior closely
- interactive runs remain usable for manual debugging

## File And Module Plan

Likely new files:

- `docs/archive/kernel/2026-04-23-milestone-5-interactive-shell-and-observability.md`
- `src/user/programs/sh.cpp`
- `src/user/include/os1/observe.h` or shared `src/uapi/os1/observe.h`
- `src/user/include/os1/process.h` or shared `src/uapi/os1/process.h`

Likely updated files:

- `src/user/CMakeLists.txt`
- `src/user/include/os1/syscall.h`
- `src/user/lib/syscall.c`
- `src/kernel/syscall_abi.h`
- `src/kernel/kernel.cpp`
- `src/kernel/keyboard.cpp`
- `src/kernel/terminal.cpp`
- `src/kernel/terminal.h`
- `src/kernel/task.cpp`

Possible supporting files if the console path is split more cleanly:

- `src/kernel/console_input.cpp`
- `src/kernel/console_input.h`

## Testing Strategy

### Phase-By-Phase Validation Rule

Each implementation PR should:

1. add exactly the tests needed to prove its own new behavior
2. keep all previously landed shell and smoke checks passing
3. avoid introducing behavior that cannot be exercised until a later PR

That rule is more important than the exact number of phases above. If a proposed PR cannot be validated in isolation, it is too large or sliced incorrectly.

### Functional Checks

The milestone should be considered complete only if:

1. both boot paths reach a shell prompt automatically
2. the shell accepts typed input from both the keyboard and serial RX without panicking the kernel
3. `sys`, `ps`, `cpu`, `pci`, and `initrd` return coherent data
4. the shell remains in user mode and uses syscalls for all functionality
5. the shell ELF is shipped in the initrd together with the other user programs
6. initrd-backed external commands can be launched from the shell and return control afterward

### Smoke Expectations

At minimum, the smoke logs should include markers such as:

- shell prompt appeared
- `sys` command succeeded
- `ps` command succeeded
- external `/bin/yield` execution succeeded
- external `/bin/fault` execution killed only the child and returned the shell prompt

Because serial RX is part of the chosen design, CI should drive those shell commands directly through the existing smoke harness rather than leaving shell behavior as a manual-only feature.

Recommended smoke structure by the end of the milestone:

- one baseline shell smoke that reaches the prompt and exercises built-ins
- one observability smoke that checks `sys`, `ps`, `cpu`, `pci`, and `initrd`
- one child-launch smoke that checks `yield`, `fault`, and prompt recovery
- one separate `exec` smoke that validates image replacement without expecting prompt return

## Risks And Tradeoffs

### 1. Shared Keyboard And Serial Input Needs Clear Ownership

If both input sources feed one console queue, the rules for echo, line editing, and active-console ownership need to stay simple and explicit.

### 2. Too Many One-Off Syscalls Will Rot The ABI

That is why a generic `observe(kind, buffer, size)` interface is recommended over a growing list of narrowly named inspection syscalls.

### 3. `exec` Alone Does Not Produce Real Shell Behavior

That is why this design assumes `spawn + waitpid` alongside `exec`.

### 4. Freestanding C++20 Can Creep Into Runtime Work If Left Unbounded

The shell can use `C++20` productively, but only if we keep it out of hosted-library territory for now.

### 5. Exposing Raw Kernel Internals Would Be A Mistake

The observe API must copy out stable records, not leak raw pointers or private kernel structs.

## Recommended Defaults

If we want the fastest path to value, the recommended defaults are:

- boot directly into the shell by staging the shell ELF as both `/bin/init` and `/bin/sh`
- add `read(fd=0, ...)` in canonical line mode
- feed the console input queue from both keyboard and serial RX
- add one generic `observe` syscall for structured snapshots
- add `exec`, `spawn`, and `waitpid` for initrd-backed execution
- keep the shell in freestanding `C++20` without exceptions, RTTI, or hosted-library dependencies
- keep `/bin/yield` and `/bin/fault` in the initrd as executable regression companions

## Chosen Direction

The current iteration assumes:

- serial RX is included in Milestone 5
- the shell is staged as `/bin/sh` and also used as `/bin/init`
- observability uses one generic syscall
- the shell executable moves to freestanding `C++20`
