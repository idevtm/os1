# Rich parameter API (profile 1)

Headers: `interface/parameters.hpp`, `interface/parameter_fixture.hpp`; namespace
`os1::parameters`. Header-only freestanding C++20, no STL/allocation/exceptions.
`fixture::make()` constexpr-constructs a complete shared constructor/method schema.
Embed **one** instance and reference it at runtime:

```cpp
[[gnu::section(".os.parameters"), gnu::used]]
constinit const os1::parameters::Metadata parameter_metadata =
    os1::parameters::fixture::make();
```

Do not make another constexpr global copy. New section `.os.parameters`, magic
`metadata_magic`, format version 1, fixed `sizeof(Metadata)==5032` for current
ELF `interface::read_section`. `.os.interface` and `.os.constructor` v1 stay intact.
Use a static Metadata read buffer (5 KB), not a large nested stack buffer.
Validate with `parameters::validate(metadata, diagnostic)` after reading. Identity
is `schema_id` + `schema_version`; provider must bind those independently to its
method or constructor protocol. Metadata itself does not execute or route calls.

Conversions (all outputs unchanged on error):

- `parse(m, raw, byte_length, Value&, Diagnostic&)`: raw CLI, not reconstructed argv.
- `normalize(m, partialValue, Value&, Diagnostic&)`: C++ partial values/defaults.
- `encode(m, value, Wire&, Diagnostic&)`: validates normalized tree and constraints.
- `decode(m, bytes, byte_length, Value&, Diagnostic&)`: independent strict provider validation.
- `print(m, value, char_buffer, capacity, written, diagnostic)`: NUL-terminated canonical CLI.
- `semantic_fingerprint(m)`: documentation-independent compatibility digest, not authentication.

`Value` uses 64 fixed nodes and 256 string bytes (under 2304 bytes total). `Wire`
contains `size` and `data[256]`; copy only `size` bytes into constructor/IPC payload.
Schemas permit at most 16 types, 32 entries, depth 8, 2304 text bytes and 256
constant-default bytes. Callers should keep reusable Value/metadata storage static
where user stacks are small. Conversion has one bounded temporary Value.

Root node index 0 has schema type `m.root`. `child_key(value,parent,id)` retrieves a
record field by stable ID. Node `number` holds integer/enum bits, optional tag, or
variant case ID; `first` indexes optional Some/variant payload or record children;
`next` chains record/list children. String bytes are
`value.strings + node.begin` for `node.size`. `no_index` is absent. Never infer
presence from a scalar zero. C++ construction: `initialize(v,m.root)`, mark root
`state=1`, `children(m,v,0,d)`, assign desired child nodes with `state=2` and their
typed value; `normalize` resolves omitted state=0 children. Use `add` for compound
payloads. Encoding rejects invalid unchecked enum casts and mismatched references.

Fixture root field IDs: 1 initial u64 default 0, 2 ceiling u64 default 100,
3 mode enum default balanced, 4 source variant default stdin, 5 options record,
6 label optional string. Options field IDs: 1 limit u32 default10 (0..1000),
2 enabled bool defaulttrue, 3 prefix optional text, 4 fallback optional enum,
5 suffix optional text default Some("!"). Source cases: 1 text(string),
2 file(record path + offset default0), 3 stdin(unit), 4 none(unit).
`ProcessingMode` explicit u32 values: fast1, balanced2, thorough3, none4.
Example: `initial:10 source.text:"hello world" options:{limit:20, fallback.none}`.
Both constructors and a provider method should parse/decode this exact schema.
Provider-specific constraints such as initial <= ceiling remain application checks.

Diagnostics: Error enum, expected type index (inspect its cases/bounds), canonical
path[256], begin/end raw input offsets, static explanation. Metadata names, summaries,
descriptions, units and format are length-delimited spans into `m.strings` (UTF-8).
Display spans by length, never as C strings; text grants no authority. No callbacks
or executable defaults. Inspect `Entry.default_size` and Type::Optional for omission
help, and decode typed default bytes using the type's documented wire grammar.

Wire grammar, all little-endian, packed/no alignment:
- integer/enum exact declared width; signed two's complement; bool 0 or1;
- string: u16 byte length + UTF-8 bytes;
- optional: u8 tag0 (no bytes) or tag1 + element bytes;
- variant: u32 stable case ID + u16 payload bytes + payload (unit length0);
- record: u16 field count, then each ascending-ID field: u32 ID, u16 payload length,
  typed payload. All known fields present, including optional None. No omissions,
  unknown fields, duplicate IDs or out-of-order IDs on wire;
- list: u16 element count, then u16 payload length + typed payload per element.

Lists are bounded and use CLI `[value,value]`; variant list elements use `.case:value`
or `.unit`. Scalars/enum values use normal value syntax. Full printable values are
bounded by the transport's 256 bytes; no public ABI or payload capacity increases.

Schema admission also bounds the worst-case decoded node count of every type to
64. Direct Optional<Optional<T>> is rejected because the selected CLI grammar has
no distinct spelling for outer Some(inner None); wrap an optional field in a record
if that distinction is needed. Enum representations are unsigned fixed widths in
this profile. Text is length-delimited; consumers must retain lengths even for embedded NUL.

For transactional ELF extraction, prefer
`parameters::read_elf(reader, file_size, output, scratch)`. It reuses the existing
bounded ELF section reader; both Metadata buffers are caller-owned (static storage
recommended), must be distinct, and output changes only after full validation.

# Rich parameter runtime integration

This unit adds a dedicated `paramdemo` provider, preserving TextProcessor and all
scalar Counter/activation interfaces. The constructor and `render` method share
exactly `parameters::fixture::make()` and the same parse/normalize/encode/decode
pipeline. The provider embeds one rich schema in `.os.parameters`; runtime refers
to that retained object. No second full schema appears in `.rodata`.

## Commands in the booted native shell

```
intf /bin/paramdemo
paramdemo
paramdemo initial:4 source.text:'Hi there' options.suffix:null
call paramdemo render mode.thorough source.text:"hello world" options:{limit:5,suffix:null}
call paramdemo render source.text:'a"b' options.suffix:null
call paramdemo render label:null source.none
paramdemo mode:bogus
call paramdemo render source.bogus
paramcheck
```

Expected results, respectively: offline help; constructor count 1/text `!`;
constructor count 12/text `Hi there`; method count 5/text `HELLO`; method count
3/text `a"b`; method count 1/text `!`; structured enum error; structured unknown
case error; `[user/paramcheck] ok`.

The shell snapshots the **original raw line before whitespace tokenization**.
Only command/image/method prefixes are skipped. The exact remaining byte span
reaches the shared parser for rich constructors, `exec`, and `call`. Ordinary
program argv and legacy `counter --initial 10 --ceiling 100` remain unchanged.
Quotes are deliberately meaningful only in the rich parameter tail; this does
not add general shell quoting, pipelines, expansion, or evaluation.

The existing console/shell accepts at most 127 bytes per line. Keep interactive
commands below this bound. Common parser callers can supply up to 4096 bytes;
normalized binary payloads remain at most 256 bytes. `paramcheck` exercises an
exactly 256-byte binary request without depending on console line capacity.

## Behavior and transport

The dedicated example renders `label + prefix + source + suffix`. Optional None
contributes nothing, while a present empty string remains a present value. The
suffix defaults to `!`; explicit `options.suffix:null` suppresses it. `source.text`
is literal text; `source.file.path` is rendered as text after a bounded byte offset
and **never opened**. The unit `source.stdin` and `source.none` cases produce no
source bytes in this capability-free demonstration. There is no implicit stdin
capability granted to the private provider.

`options.limit` limits source bytes; `enabled:false` suppresses conversion. The
`fast` mode lowercases ASCII and `thorough` uppercases ASCII; `balanced` and `none`
preserve bytes. When mode is `none`, a present fallback mode is used. Prefix,
label, and suffix are not case-converted. Slicing is byte-oriented (not Unicode
code-point truncation). Input strings are validated UTF-8 and remain explicitly
length-delimited, including embedded NUL.

The returned count is `min(ceiling, initial + rendered_byte_count)` with overflow
avoided by subtraction. `initial > ceiling` is rejected by the provider. Reply
bytes are little-endian u64 count followed by up to 248 raw rendered bytes; reply
payload length supplies the text length. No native object layout crosses IPC.
The constructor prints the same behavior when launched normally; a private
provider validates/constructs before announcing Ready and then processes methods.

The provider checks constructor schema ID/version independently; the method binds
schema version to its interface ID/version and member ID independently of caller
metadata. Every request goes through strict shared binary decoding before render.
Malformed records, closed enum/variant values, optional tags and trailing bytes
produce `OS1_ERR_INVALID` without dispatching behavior.

## Discovery and private client

New `.os.parameter-bindings` v1 is an 80-byte read-only ELF record defined in
`src/common/interface/parameter_binding.hpp`. Fixed fields are magic/version/size,
schema ID/version, constructor flag, interface ID/version, member count, and eight
u32 member IDs. IDs map to implemented Method records in `.os.interface`; unused
slots must be zero. The current profile permits one rich schema per image, shared
by the explicitly listed constructor/method bindings. There is no inferred
association based on a fixture's field names or a method's name.

`intf` reads `.os.interface`, legacy `.os.constructor`, `.os.parameters`, and
`.os.parameter-bindings` from the retained file descriptor without launching it.
It reports constructor/method input bindings, type references, fields/cases,
enumeration identities, optional omission/nullability, canonical typed defaults,
bounds, summaries, descriptions, units and formats. A malformed binding fails
inspection. `.os.constructor` v1 and its old display remain supported.

`call <image> <method> <raw arguments>` reads metadata from the same retained
EXECUTE|READ|SEEK|INSPECT executable handle used to launch. `ParameterClient`
validates interface/binding metadata, sends normalized constructor defaults,
creates a private endpoint pair, and grants only the provider endpoint as fd 3.
The child receives no root, cwd, standard streams, extra images, or ambient path
authority. Ready identity is checked. Startup and each call have a two-second
monotonic deadline. On completion/failure the caller closes its endpoint,
requests kernel-supervised private-child stop, and reaps. Operations are closed.

The reusable client supports any implemented method explicitly bound to this
single-schema profile. For `paramdemo.render`, shell output decodes count/text;
other bound interfaces display response bytes as hex. These response presentation
choices do not determine input layout. `ParameterClient` and VFS normalization
scratch buffers are intended for this single-threaded synchronous userspace.


See the [implementation record](2026-11-05-client-sessions-and-parameters.md) for validation.
