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
The initial path list contains `/bin`, then `/disk/bin`.
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
path.add "/disk/tools"
path.search "tool"
path.remove "/disk/tools"
```

Environment assignment replaces or adds one field under an existing Record.
Record members can be enumerated through the bag representation; this is not general service reflection.
`path` updates accept at most eight absolute strings, each at most 255 bytes.
Search retains the first executable candidate and launches that same object.
Only NOT_FOUND advances to the next directory. Explicit paths bypass search and aliases.

## Commands and arguments

| Alias | Canonical binding | Parameters |
| --- | --- | --- |
| `echo` | `out.print` | Optional String `text`, positional slot 1; default empty |
| `pwd` | `out.print fs.cwd` | None |
| `cd` | `fs.change_directory` | Required String `path`, slot 1 |
| `ls` | `fs.list` | Optional String `path`, slot 1; default cwd |
| `touch` | `fs.ensure_file` | Required String `path`, slot 1; no timestamp changes yet |
| `cp` | `fs.copy` | `source` slot 1, `destination` slot 2, named `mode`; default replace |
| `cat` | `fs.copy` | Same parameters; default input stream and output stream, append policy |
| `mv` | `fs.move` | Required String `source` slot 1 and `destination` slot 2 |
| `rm` | `fs.remove_file` | Required String `path`, slot 1 |
| `mkdir` | `fs.create_directory` | Required String `path`, slot 1 |
| `rmdir` | `fs.remove_directory` | Required String `path`, slot 1; empty directories only |

`fs.create_file` uses exclusive creation. `fs.ensure_file` preserves existing contents.
Built-in aliases work in scripts and interactive commands; custom alias definitions are deferred.
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
`--` ends option recognition; this stage does not support recursive flags.
Both `fs.cwd = "/tmp"` and `cd "/tmp"` validate directory authority before changing context.
A String parameter requires a String value; integers do not implicitly become path strings.
Copy accepts path strings and the existing `out` or `process.streams.*` bindings.
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

The second command resolves an object/property first, then warns on stderr if it treats `a.txt` as text.
The same unresolved token fails in a script. Failed getters and type conversions never become literal text.
`sh "script"` reads commands from a separate file handle and preserves the child's input stream.
`sh < "script"` also remains available.
Unhandled command failure stops the script; an interactive session continues after reporting it.
Bare property expressions display results only interactively. Listing, content commands, and explicit output work in both modes.

Legacy executables receive String arguments or serialized Integer/Boolean values through argv.
Native constructor and `call` parameter tails retain their separate versioned metadata parser.
Existing diagnostic commands, `write`, and `edit` retain their original command parsers.
Legacy `exec` and `call` prepare their arguments after redirection; ordinary external commands validate arguments first.
This stage does not claim that all legacy commands use reflective argument binding.

## Limits and next stages

| Bound | Current value |
| --- | --- |
| Shell input | 127 bytes plus NUL; at most 16 command tokens |
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
