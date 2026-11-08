# Live runtime context discovery

The shared user runtime describes inspectable bindings and copied environment data without activation.
The shell uses these descriptions for `context` and Tab completion.

```text
existing resource grants + typed environment
                     |
             bounded local View
                     |
       resolve -> enumerate -> describe
                     |
          context command / Tab list
```

| Topic | Definition |
| --- | --- |
| Available objects and limits | [Snapshot contract](#snapshot-contract) |
| Interactive behavior | [Shell controls](#shell-controls) |
| Input ownership and scripts | [Input handoff](#input-handoff) |
| Measurements and tests | [Implementation record](context-completion-plan.md#validation-evidence) |

## Snapshot contract

This implemented profile uses a fixed registry in [view.hpp](../../src/common/discovery/view.hpp).
It is a user-runtime API, not a new syscall or a general remote discovery protocol.
The [refresh helper](../../src/user/include/os1/discovery.hpp) queries existing descriptor metadata and copies the current environment.
It does not open paths, bind providers, activate classes, read file contents, or invoke arbitrary getters.
Refresh performs no filesystem writes and starts no processes.

| Path | Available content |
| --- | --- |
| `os.fs`, alias `fs` | Existing filesystem helpers, filtered by root lookup and descendant rights |
| `os.system` | Existing synchronization and power methods when SYSTEM_CONTROL is granted |
| `os.system.log` | Native log descriptions and shell printing when LOG_READ is granted |
| `process.environment` | Copied typed records, lists, strings, integers, and Booleans |
| `process.path`, alias `path` | Local path operations; search additionally requires lookup and execute authority |
| `process.streams` | Present standard descriptors; Console descriptions follow each descriptor's rights |
| `process.streams.output.print`, alias `out.print` | Shell output helper when output has WRITE authority |
| `process.context.list`, alias `context` | Bounded structural inspection |
| `process.shell.history` | Current shell history description; absent from ordinary process views |
| `text`, `pager` | Existing shell text and pager helpers; absent from ordinary process views |

The registry does not enumerate arbitrary open descriptors or runtime provider registrations.
It does not expose unactivated applications, devices, network services, or class files as live objects.
Resource inspection requires the existing INSPECT right for QUERY and FILE_INFO.
A grant without INSPECT is omitted, even when the process can invoke operations through that grant.
Discovery does not weaken inspection authority to expose those hidden bindings.
An omitted member therefore does not prove that invocation is impossible.
Local helper descriptions do not guarantee access to every pathname or argument.
The invoked operation still checks its actual resources and authority.
Descriptors closed after refresh can make a previously described operation unavailable.

Each member has a bounded name, kind, type, signature, required rights, and flags.
Native descriptions also include interface identity, version, and member identity.
Local signatures provide concise help; they are not complete executable parameter schemas.
Flags distinguish local helpers, native members, aliases, and shell-callable members.
Native metadata alone does not add shell invocation support.

`View` supports `resolve`, `container`, and `enumerate`.
`Member.value` indexes the copied environment when the member describes environment data.
List children use decimal indices, such as `process.environment.path.0`.
The text inspector prints member kinds and signatures; it does not print arbitrary property values.
Typed clients can inspect copied environment values directly.

| Bound | Value |
| --- | --- |
| Snapshot nodes | 128 |
| Members per page or displayed page | 8 |
| Name storage | 64 bytes, including terminator |
| Signature storage | 96 bytes, including terminator |
| Path storage | 256 bytes, including terminator |
| Resolution and alias depth | 16 |
| Environment | Existing 48-node, 2048-byte text limits |

Refresh assigns a new nonzero generation and invalidates old enumeration cursors.
A cursor from another generation or parent returns CANCELLED.
Invalid cursors return INVALID; unavailable paths return no match.
Environment validation rejects control characters in field names before display.
Aliases resolve through bounded traversal; cycles fail resolution.
The current registry exposes no events because its available services have no implemented event members.

## Shell controls

The shell owns presentation and editing independently of discovery.
It displays scrolling text lists on serial, VGA, and framebuffer consoles.
Each list page contains at most eight choices.

| Key | Action |
| --- | --- |
| Tab | Open filtered choices for the current unquoted dotted token |
| Up / Down | Select a choice while the list is open |
| Tab on an object | Insert its name and enumerate its children |
| Tab on a leaf | Insert its name and display its kind and signature |
| Enter with an open list | Accept the selection without execution |
| Enter with a closed list | Execute the command |
| Escape | Close the list; a serial Escape uses a bounded 50 ms sequence timeout |
| Ctrl-C | Cancel the current line |
| Backspace | Remove the final byte |

Native members without shell invocation support display an explicit metadata-only message.
Completion does not read property values or subscribe to events.
This slice supports editing at the line end, with a 127-byte command bound.
Filename completion, history navigation, cursor editing, and arbitrary provider discovery remain separate work.
Quoted or escaped tokens do not receive object completion.

```text
context
context os.system
context process.environment
process.context.list path:process.environment.path
os.fs.list "/tmp"
echo os.fs.cwd
process.path.list
```

These commands inspect or use existing bindings.
The `os.fs` and `process.path` forms share the existing `fs` and `path` implementations.
Scripts use the same inspection command but retain their existing input parser.

## Input handoff

The interactive prompt acquires a [ConsoleInputSession](abi/kernel-console-abi.md#interactive-input-session) with explicit handoff enabled.
It releases ownership before command execution, including cancellation and error exits.
Queued typeahead then enters the canonical reader for commands, heredocs, or foreground applications.
Overflow produces RANGE instead of executing a truncated line.
When canonical input is already pending, the shell reads that line through its existing reader.

Pager sessions retain their existing discard-on-release policy.
Script and redirected input do not acquire an interactive prompt session.
Navigation bytes use the same ABI for PS/2 and USB input; serial input accepts bounded escape sequences.
Successful key polling and expected WOULD_BLOCK results do not fill the normal diagnostic ring.
Diagnostic capture still records instrumented calls, and genuine polling failures remain visible.

Physical keyboard behavior requires separate hardware qualification.
QEMU checks cover emulated PS/2, USB, serial input, ownership handoff, and both boot paths.
