# Typed context and basic object commands

The first shell stage implements typed context, navigation, aliases, and basic file operations.
The [implementation record](typed-context-plan.md) contains the impact assessment, checks, measurements, and review results.

```text
bind positional/named arguments
  -> interactive: resolve expressions -> warn on unresolved text
  -> script:      resolve expressions -> fail on unresolved text
  -> validate types and metadata
  -> prepare streams
  -> local helper -> granted native resource operations
```

## Context and authority

`process.environment` is a bounded bag of records, lists, strings, integers, and booleans.
Children inherit independent copies through [startup ABI version 3](../native-api/abi/initrd-vfs-abi.md#typed-environment-and-namespace-extension).
The boot context starts with an empty search list. The disk startup profile adds `/bin`.
No PATH text variable mirrors that list.
Code for `path` and `fs` resides in the linked user runtime; neither requires a service process.

Root, cwd, and standard streams remain separately granted resource handles.
`fs.cwd` reconstructs a root-relative String from the retained cwd directory.
Renaming that directory changes its displayed path without changing its identity.
An unreachable cwd produces an error. Changing to an absolute reachable directory can recover the session.
A child cannot change its parent's cwd or environment.

```text
process.environment.build = {jobs:4, label:"debug", features:["console", "storage"]}
echo process.environment.build.label
process.environment.build
path.list
path.add "/tools"
path.search "tool"
path.remove "/tools"
```

Environment assignment replaces or adds one field under an existing Record.
Record members can be enumerated through the bag representation; this is not general service reflection.
`path` updates accept at most eight absolute strings, each at most 255 bytes.
Search retains the first executable candidate and launches that same object.
Only NOT_FOUND advances to the next directory. Explicit paths bypass search and aliases.

## Name resolution and inspection

The shell selects aliases, live bindings, and filesystem entries by position and spelling.
An explicit spelling keeps its meaning when names collide.

```text
ls    -> command alias
.ls   -> live binding
./ls  -> file in the current directory
```

A bare command name selects an active alias first. The shell then checks live bindings and searches for a program.
A leading dot selects the live context. Enter on `.` lists its roots and bindings.
Enter on an object lists its immediate members. A property or field displays its value.
An object listing can write to a redirected output. Typed calls use their existing redirection rules.
A bare method uses its supplied arguments and defaults. If a required input is missing, the shell reports its name.

A slash selects a filesystem path. Quotes select a literal path in a file operand.
A file operand of `.` selects the current directory.
An unquoted dotted name selects a live member if its first segment names a live binding.
If that member is absent, the shell reports an error.
A method used to compute a path requires an explicit call with parentheses.
Use `./report.txt`, `"report.txt"`, or `'report.txt'` to select a colliding file.
In a path or stream operand, `process.streams.input` selects the live stream.

| Command | Result |
| --- | --- |
| `inspect`, `inspect .` | List live roots and bindings. |
| `inspect .os.fs.list` | Show the method definition and its defaults. |
| `inspect /bin/textprocessor` | Read the image interface and constructor metadata. |
| `help ls` | Show the alias and its settings. |

Inspection reads descriptions and image metadata without invoking methods or starting programs.
The live context listing contains roots and bindings.
Existing `context` and `intf` commands remain available for scripts.

Tab browses the applicable domains at the cursor.
After `.`, it offers live roots. After `./`, it offers filesystem entries.
After `inspect `, it offers live references and paths.
Tab inserts a sole match and opens a menu for several matches.
A selected spelling preserves the target when you press Enter.

## Startup configuration

Init passes the optional String constructor argument `init_script:"/etc/shell/init.sh"`.
The shell applies the profile before the first prompt.
The profile configures the command search list and the alias table.

```text
/sbin/init -> /bin/sh constructor -> /etc/shell/init.sh -> search list + aliases
```

The profile accepts literal calls to `process.path.add`, `process.path.remove`, and the alias mutation methods.
If a statement fails, the shell reports its file and line and restores the previous configuration.
A missing file leaves the initial configuration in place.
Child shells can load aliases from the inherited profile path. They keep their inherited search list.

```text
process.path.add(path:"/bin")
process.shell.aliases.add(name:"ls", target:"os.fs.list")
process.shell.aliases.add(name:"cp", target:"os.fs.copy")
process.shell.aliases.add_switch(name:"cp", spelling:"-r", assignment:{parameter:"recursive", value:true})
process.shell.aliases.show(name:"cp")
process.shell.aliases.remove_switch(name:"cp", spelling:"-r")
process.shell.aliases.remove(name:"cp")
process.shell.aliases.list()
```

An alias names a qualified shell method or a supported property.
An alias can set a default for a supported Boolean input.
It can also define one short switch.
An explicit input overrides the alias default. Duplicate explicit assignments fail before invocation.
Explicit paths bypass the alias table.

## Commands and arguments

| Alias | Canonical binding | Parameters |
| --- | --- | --- |
| `echo` | `out.print` | Optional String `text`, positional slot 1; default empty |
| `pwd` | `out.print fs.cwd` | None |
| `cd` | `fs.change_directory` | Required String `path`, slot 1 |
| `ls`, `ll` | `os.fs.list` | Optional String `path` and Boolean `list`. `ls -l` and `ll` set `list:true`. |
| `touch` | Ensure file, then native `set_times` | Required String `path`, slot 1; create if absent, then update access and modification times |
| `cp` | `fs.copy` | `source` slot 1, `destination` slot 2, named `mode`; default replace |
| `cat` | `fs.copy` | Same parameters; default input stream and output stream, append policy |
| `mv` | `fs.move` | Required String `source` slot 1 and `destination` slot 2 |
| `rm` | `fs.remove_file` | Required String `path`, slot 1 |
| `mkdir` | `fs.create_directory` | Required String `path`, slot 1 |
| `rmdir` | `fs.remove_directory` | Required String `path`, slot 1; empty directories only |

`os.fs.list` uses the current directory when you omit `path`.
With `list:true`, it prints the file type, size in bytes, modification time in nanoseconds, and name.
The shell prints `modify_ns:unsupported` when the file has no modification time.
The detailed format requires lookup and inspect access for directory entries.

```text
ls -l /tmp
ll /tmp
os.fs.list(path:"/tmp", list:true)
```

## Diagnostic aliases

```text
startup profile -> short alias -> os.system.diagnostics method -> report
```

| Alias | Method under `os.system.diagnostics` |
| --- | --- |
| `sys` | `summary` |
| `ps` | `processes` |
| `cpu` | `processors` |
| `pci` | `pci` |
| `initrd` | `initrd` |
| `devices` | `devices` |
| `usb` | `usb` |
| `resources` | `resources` |
| `kmem` | `memory` |
| `events` | `events` |
| `irqs` | `interrupts` |

The methods print the existing reports and use the current observation permissions.
Run `context os.system.diagnostics` to see the methods.
Call `os.system.diagnostics.summary()` to print the system report without an alias.
Existing disk profiles keep their contents during deployment.
To add these aliases to an existing profile, copy the new declarations from the [default profile](../../src/user/data/shell-init.sh).

## Process, logs, and power

```text
help -> active aliases and reflected methods
pid -> process.pid
history -> process.shell.history.print
dmesg -> os.system.log.print
sync, shutdown, reboot -> os.system methods
```

Read `process.pid` to get the current shell process ID as an Integer.
The default `pid` alias prints the same ID with its CLI label.
The log aliases accept the existing filters. `history on` and `history off` control command capture.
The power aliases require the system-control right.

Run `help` to list built-in commands, active aliases, and methods and properties in the shell registry.
Run `help ls` to show an alias and its settings.
Run `help exec` to show the syntax for a built-in command.
Run `help os.system.diagnostics` to inspect an object.
Use the object path to see methods on reflected hardware objects.

## Filesystem commands

`fs.create_file` uses exclusive creation. `fs.ensure_file` preserves existing contents.
The disk startup profile installs the default aliases. Alias changes apply only to the current shell.
All method and property names use snake case.

```text
cd "/tmp"
mkdir "work"
cd "work"
echo "line1" > "a.txt"
cp destination:"b.txt" "a.txt"
fs.copy "a.txt" mode:append destination:"b.txt"
mv "b.txt" "renamed.txt"
cat "renamed.txt"
rm "renamed.txt"
cd ".."
```

Named arguments do not consume positional slots. Duplicate, unknown, missing, and excess arguments fail before invocation.
`--` ends option recognition. The default profile defines `-r` for `cp` and `rm`.
Both `fs.cwd = "/tmp"` and `cd "/tmp"` validate directory authority before changing context.
A typed String parameter requires a String value. A short path operand such as `cat 123` uses the literal filename.
An explicit typed copy call accepts path strings and stream references.
Short file commands treat bare operands as paths and preserve the established stream names.
Completion quotes a filename that has a stream name.
General variables containing File objects remain later work.

A directory destination appends the source basename for copy and move.
Copy rejects identical retained input/output objects before truncation, including shell output redirection.
Copy writes existing stream objects at their current position, subject to their open policy.
Partial output remains after a later I/O error; the shell reports failure and does not replay the operation.
`cat` without a source, or with `-`, preserves the existing input-stream behavior.
Move supports same-filesystem cross-directory replacement. Cross-filesystem moves fail without a copy/delete fallback.
Mutation paths with trailing separators are rejected; they cannot silently select a regular file.

## Interpretation and scripts

Quoted strings are portable between CLI and scripts.
A double-quoted string supports `\n`, `\r`, `\t`, `\\`, and `\"`; single-quoted strings preserve their bytes.
Records use `{field:value}` and lists use `[value, value]`.
Signed 64-bit integer and Boolean literals are supported.
General operators, concatenation, formatting, loops, and functions remain deferred.

```text
echo fs.cwd
cat a.txt
cat "a.txt"
sh "/tmp/task.sh"
```

The second command passes a literal path in both the prompt and a script.
Use an explicit typed call to pass an object reference or computed path.
Failed getters and type conversions report errors.
`sh "script"` reads commands from a separate file handle and preserves the child's input stream.
`sh < "script"` also remains available.
Unhandled command failure stops the script; an interactive session continues after reporting it.
Bare property expressions display results interactively. An explicit live reference such as `.process.pid` also displays its value in scripts.

Legacy executables receive String arguments or serialized Integer/Boolean values through argv.
Native constructor parameter tails use the versioned metadata parser.
`write` and `edit` use their own command parsers.
`exec` prepares its arguments after redirection. Ordinary external commands validate arguments first.
The historical `call` command is retired. `paramcheck` still exercises rich provider calls.

## Limits and next stages

| Bound | Current value |
| --- | --- |
| Shell input | 1023 bytes plus NUL; legacy commands retain the 16-token limit |
| Environment | 48 nodes, 2048 text bytes, depth eight |
| Shell path values | 255 bytes; backend component limits still apply |
| Copy chunk | 256 bytes through copied File/stream calls |
| Native mutation names | 63 bytes |
| Tmpfs tree | Maximum depth 32; existing node/page quotas remain |

The [metadata and traversal extension](filesystem-tools.md) implements recursive commands, `find`, `du`, `stat`, and `df`.
The remaining [utility profile](file-utilities.md) remains accepted future work.
The [text utilities](text-utilities.md) and [pager](pager.md) are implemented.
Timestamp mutation remains a subsequent stage.
Permissions and links remain deferred. General OS service discovery and generated bindings are separate Native API work.

The [guided command extension](guided-command-plan.md) adds nested calls, shell-local bindings, and interactive history.
