# Independent client sessions and rich parameters — 2026-11-05

Implemented and validated. This completes the sessions/authority/quota and bounded
rich-parameter portions of the [approved plan](2026-11-05-client-sessions-and-parameter-plan.md).
Shared-buffer transport remains the next phase. No shared-memory, general registry,
transparent restart, or generated SDK contract is implied.

## Counter sessions and authority

Each successful acquisition returns a fresh budgeted endpoint pair bound to the
class-selected Counter singleton. Sessions have distinct connection generations;
they report the same opaque object identity and observe shared value/revision.
Construction is confined to a separate manager-only control interface. The
bootstrap grants mutable activation at fd 3, the retained executable at fd 4,
and observer activation at fd 5. Policy derives from the receiving endpoint;
raw observer Add/Reset fail with ACCESS. Data clients cannot invoke Construct.

One kernel admission root reserves four sessions, split into two observer and
two mutable slots. Aliases/transfers share the account; they do not mint capacity.
Each session has four retained Operations and two retained subscription objects.
Completed Operations and unsubscribed handles keep their charge until released;
closing/reacquiring cannot bypass the root limit. Each subscription retains the
existing eight-event queue and local drop reporting. Payloads remain 256 bytes,
with at most four capability attachments per operation stage. Approximate kernel
allocation commitment is 12.6 KiB/session before allocator overhead or independently
owned attached objects, about 51 KiB for four sessions and accounts.

WAIT_MANY accepts 1–8 distinct retained instances, returns a ready index through
the syscall return register, and preserves deadline/stop/closure cleanup across fd
reuse. Counter waits on at most five endpoints and services one request per session
per round, rotating the first data session. FIFO is local to a session; handlers
remain serialized, with no hard latency/preemption guarantee. Events are volatile,
future-only independent broadcasts. Disconnecting one session leaves others usable;
manager-scope death stops the provider. Failure is sticky, with no replay/restart.

Exact operations, layouts, rights, lock order and error contracts are in the
[kernel session ABI](client-sessions-abi.md) and
[shared activation ABI](shared-activation-abi.md).

## One bounded parameter model in real calls

`paramdemo` shares one schema for its actual constructor and `render` method.
The common library validates schemas, parses raw dot/brace input, normalizes typed
defaults, and strictly encodes/decodes canonical little-endian bytes. Supported
values include signed/unsigned integers, booleans, closed enums, UTF-8 strings,
optional None/Some, structs, payload/unit variants and bounded lists. The demo
exercises optional/string/compound/enum/variant/default behavior; signed integers
and lists are additionally covered by common-library contract tests.

The shell retains the original raw parameter tail before whitespace tokenization.
Quotes/braces/dot paths are meaningful in rich tails; ordinary argv deliberately
keeps its earlier whitespace behavior. `intf` reads read-only retained ELF metadata
without running the image. Explicit `.os.parameter-bindings` associates the shared
schema with constructor and concrete interface/member IDs. The `call` command
retains the same authorized executable for inspection and launch; its private
provider receives only fd 3, with two-second startup/call deadlines and supervised
cleanup. Providers independently validate schema identity, payload shape and
application constraints.

The profile bounds are 16 types, 32 entries, depth 8, 64 value nodes, 2304 metadata
text bytes, 256 default bytes and 256 transport bytes. Interactive console lines
remain at most 127 bytes; parser callers may supply up to 4096. Strings in the demo
are bounded to 64 bytes. No floats, maps, recursive schemas or native layout
transport are added. Direct nested optionals require a record wrapper. Results
remain explicitly length-delimited; embedded NUL is preserved. The demo's
`source.file.path` is data, never implicit file authority; unit stdin/none produce
no source bytes. ASCII conversion and slicing are byte-oriented.

Run these exact native-shell commands:

```text
activationcheck
intf /bin/counter
counter --initial 10 --ceiling 100
intf /bin/paramdemo
paramdemo initial:4 source.text:'Hi there' options.suffix:null
call paramdemo render mode.thorough source.text:"hello world" options:{limit:5,suffix:null}
call paramdemo render source.text:'a"b' options.suffix:null
call paramdemo render label:null source.none
paramdemo mode:bogus
paramcheck
```

The constructor prints count 12/text `Hi there`. Calls print count 5/text `HELLO`,
count 3/text `a"b`, and count 1/text `!`; explicit null suppresses optional defaults.
The malformed enum reports code, path, input offsets, expected type and explanation.
`paramcheck` exercises real binary calls, malformed enum/variant/optional input,
constructor identity, application constraints, recovery and exact 256-byte
acceptance/257-byte rejection. TextProcessor methods/properties, Counter private
mode, scalar metadata v1 and legacy `--name value` remain compatible.

See the [rich-parameter ABI and commands](rich-parameters-abi.md) for exact metadata,
canonical wire grammar and C++ APIs; the [parameter profile](os-api-draft/parameter-schema-and-cli.md)
retains the design rationale and conceptual examples for future APIs.

## Capacity decision and checks

The completed user tools produced a 183,808-byte plain initrd; the unoptimized
kernel was 467,456 bytes. Their combined size exceeded the existing BIOS staging
span. Kernel C++ now uses `-Os`, preserving freestanding, no-red-zone and
integer-register-only flags. User section GC retains all named metadata with
KEEP, and Counter references its packaged constructor schema. No regression
program or smoke was removed.

The same low-memory span is rebalanced into 360 KiB kernel staging
`[0x10000,0x6A000)` and 192 KiB initrd `[0x6A000,0x9A000)`. The final stripped
BIOS kernel is 332,288 bytes (36,352 bytes spare); initrd is 183,808 bytes
(12,800 bytes spare). PT_LOAD ends at `0x16D000`; with the unchanged 16 KiB
post-image reserve it requires `0x171000`, below reserved end `0x192000`.
The page bitmap, loader algorithm, first-4-MiB mapping, firmware ceiling and
boot-log placement are unchanged. Disk kernel slot is LBA 65–784, initrd
785–1168; journal begins at 1280. ELF-aware size/overlap checks remain mandatory.
This avoids a high-memory loader/compression project. Optimized code changes
machine-code layout; QEMU validation does not establish new physical-hardware
qualification.

Validation completed against the combined final sources:

- `cmake --build build -j 4`: BIOS raw, UEFI ISO and USB images built, including
  BIOS execution/staging/initrd and Limine shim layout assertions.
- `cmake --build build-host-tests -j 4` and
  `ctest --test-dir build-host-tests --output-on-failure`: all 266 tests passed.
- `ctest --test-dir build --output-on-failure`: all 14 checks passed, including
  UEFI/BIOS baseline, observation, SMP balance, expanded spawn, exec, xHCI,
  no-UART and both script contract checks. The first sandboxed run passed 12;
  the two Unix-socket checks were rerun with sandbox escalation as part of the
  complete successful 14-check run. No assertion was weakened.
- The parameter workers additionally passed all 24 common/runtime interface tests
  under ASan/UBSan and compiled affected freestanding sources with warnings as errors.
- Packaged provider metadata remains read-only: one 5032-byte `.os.parameters`,
  one 80-byte binding and the existing `.os.interface` section. Legacy constructor
  and interface sections remain retained after strip/GC.
- Source-only adversarial passes covered kernel admission/waits, raw parsing,
  protocol binding, provider authority/lifetime and final capacity/build integration;
  `git diff --check` passed.

Publishing exposed a GCC host-build failure: the legacy kernel `static_assert`
macro shadowed the native C++ keyword and broke standard-library declarations.
Removing the obsolete macro restores native compile-time checks. A minimal
two-argument assertion reproduced the failure before the fix and compiled after
it; a false assertion still failed compilation. The host suite and image builds
passed again after this compatibility fix.
