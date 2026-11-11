# Interactive files and line editor

The shell can create, append, and edit files in `/tmp`.
These files disappear on reboot. The boot initrd remains read-only.

```text
Console / input file -> shell command -> granted Directory and File resources
                         |                 |
                         |                 +-- native create, truncate, append
                         +-- redirected descriptor streams -> child program
```

| Task | Command | Details |
| --- | --- | --- |
| Create an empty file | `touch /tmp/note` | Preserve contents; update access and modification times. See the [timestamp ABI](../native-api/abi/filesystem-times-abi.md). |
| Enter new text | `write /tmp/note` | Refuses an existing file. |
| Replace text | `write --replace /tmp/note` | Truncates before input. |
| Append text | `write --append /tmp/note` | Creates the file if absent. |
| Edit text | `edit /tmp/note` | Buffers changes until explicit save. |
| Read a file | `cat /tmp/note` | `cat` without paths, or with `-`, reads standard input. |

## Enter text

Enter `write /tmp/note` at the shell prompt.
Type the file contents on subsequent lines.
Enter a single dot on its own line to finish.
Enter two dots to insert a literal dot line.

```text
os1> write /tmp/note
write: enter text; . or Ctrl-D ends, .. enters a dot line
Hello from os1.
Second line.
.
write: saved
os1> cat /tmp/note
Hello from os1.
Second line.
```

Ctrl-D also ends input when the pending console line is empty.
With pending text, Ctrl-D delivers those bytes without a newline; press it again to signal EOF.
EOF consumes one console record. It does not close the console.
Serial, PS/2, and USB keyboard input use this rule.

Each input line permits 127 text bytes, plus an optional newline.
An overlong line returns RANGE and discards the complete line.
After a writer error, input drains to the dot or EOF terminator.
Earlier successfully written lines remain; the command does not report success.

## Redirect streams

| Operator | Behavior |
| --- | --- |
| `< path` | Read standard input from a file. |
| `> path` | Create or truncate standard output. |
| `>> path` | Create or append standard output. |
| `<< marker` | Collect literal lines until a line exactly matches the marker. |

```text
cat /disk/etc/motd > /tmp/motd
cat < /tmp/motd >> /tmp/log
cat << END > /tmp/message
First line.
Literal > text.
END
```

Redirection works for built-ins and external commands.
The shell restores its streams after the command returns or launch fails.
Diagnostics and prompts use standard error.
Successful foreground commands return directly to the prompt, without spawn or resume messages.
A nonzero child exit reports `command failed: <command> status=<value>`; launch and wait errors remain visible.
A successful `exec` retains redirected streams in the replacement image.
A shell reading a finite input file exits at EOF; an interactive console shell accepts new input after Ctrl-D.
For example, `sh < /tmp/script > /tmp/output` writes command results without shell prompts or startup messages.

Quote paths with spaces, for example `cat "/tmp/my note"`.
Single and double quotes protect operators; backslash escapes outside single quotes.
Typed constructor and method arguments retain their original syntax after redirection removal.
The `echo` command accepts one optional String. It prints that String followed by a newline.
With no argument, it prints one newline. Quoted spaces and parentheses remain part of the String.
For example, `echo "f(x)" > /tmp/result` writes `f(x)` and a newline to the file.
See [Typed context and basic commands](typed-context.md) for the `echo` binding.

Collection prints no additional prompt. Console input echo stays active.
If the console rejects a command before collection, it discards the body through the recognized delimiter.
Content has no variable expansion, dot termination, or special operator interpretation.
The shell collects it in an unlinked `/tmp` file before opening the output file.
Premature EOF or collection failure prevents output creation or truncation.
After an overlong line or storage failure, collection drains to the marker or EOF.
If parsing fails, the console discards the body when it can identify the delimiter.
A here-document operator inside a typed expression stays part of the expression.
Temporary storage is released when the command finishes or preparation fails.

Commands permit 1023 bytes; legacy commands retain the 16-argument limit.
The parser rejects duplicate stream redirections, incomplete quotes, and missing operands before changing files.
Pipes, command chaining, background execution, and numbered descriptor redirection are not implemented.

## Use the line editor

Enter `edit /tmp/note` to open a file or start a new buffer.
Line numbers start at one.
This is a line editor; it does not require cursor addressing or a full-screen terminal.

| Editor command | Effect |
| --- | --- |
| `p` | Print all lines with line numbers. |
| `p first [last]` | Print the selected range. |
| `a [after]` | Append after a line; default is the last line. `a 0` inserts at the start. |
| `i line` | Insert before a line. |
| `c first [last]` | Replace the selected range. |
| `d first [last]` | Delete the selected range. |
| `w` | Save to the original path and continue. |
| `wq` | Save and quit. |
| `q` | Quit only when no unsaved changes remain. |
| `q!` | Discard changes and quit. |

Append, insert, and change commands accept text until a lone dot or Ctrl-D.
Two dots insert a literal dot line.
A failed insertion leaves the previous buffer unchanged and drains input to its terminator.
Appending after an unterminated last line adds the separating newline.

```text
edit /tmp/note
a
First line.
Second line.
.
p
c 2
Replacement line.
.
wq
```

The editor holds at most 64 KiB of text and a separate 64 KiB insertion buffer.
These buffers occupy shell BSS; they do not add 128 KiB to the initrd file.
Saving truncates the destination before writing.
A failed save can leave a partial file; the editor retains its dirty buffer for another save attempt.
There is no atomic replacement or concurrent-edit conflict detection.

At the editor command prompt, console Ctrl-D follows the guarded `q` rule.
Finite redirected input cannot provide another command after EOF.
At that EOF, the editor exits and reports discarded unsaved changes; use `w` or `wq` in scripts to save.
Other unrecoverable input errors terminate the editor with a diagnostic.

## Native API and limits

File creation and truncation use [native Directory/File calls](../native-api/abi/tmpfs-abi.md).
Stream copying uses the same open instances through descriptor operations.
The shell introduces no second handle table or filesystem authority model.

Native `append` and descriptor APPEND select the current file end under the filesystem lock.
Each append unit is at most 256 bytes and cannot overlap another append unit.
A larger descriptor write can interleave with other writers between units.
The [filesystem quotas](../native-api/abi/tmpfs-abi.md#namespace-and-storage-rules) also apply to here-documents and editor saves.

See the [implementation record](../archive/shell/2026-11-06-interactive-files.md) for validation, measurements, and review results.
