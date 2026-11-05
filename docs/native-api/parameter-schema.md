# Parameter schemas, descriptions, and dot-based CLI values

Status: bounded profile 1 implemented 2026-11-05. The design rationale below is
retained, including explicitly conceptual examples for broader APIs. Exact wire
layout, limits, metadata bindings and runnable commands are in the
[implemented rich-parameter ABI](abi/rich-parameters-abi.md) and
[implementation record](../archive/native-api/2026-11-05-client-sessions-and-parameters.md).

The native shell preserves the raw rich argument tail for quotes, braces and dot
paths. `paramdemo` uses the same schema for its constructor and `render` method.
Counter retains its scalar `.os.constructor` v1 and `--name value` syntax;
TextProcessor retains its existing methods/properties. General bindings and
reflective result/event/property schema adoption remain later work.

## 1. One schema for methods and constructors

Use the same parameter descriptor and type vocabulary for method inputs,
constructor arguments, results, property values, and event payload fields.
Parameters belong to an input record, rather than being an untyped positional
argument list. A constructor is a typed invocation whose result also carries
object authority; richer arguments do not give the caller a creation-policy knob.

| Descriptor element | Meaning |
| --- | --- |
| Stable field ID | Numeric identity in the owning record; never reused incompatibly. |
| Canonical name | Human/tool name and CLI path segment. Names do not route wire calls. |
| Type reference | Scalar, enumeration, string, record, optional, bounded list, or tagged variant. |
| Omission rule | Required, absent optional value, or a declared constant default. |
| Constraints | Numeric range, byte/element limits, and supported structured constraints. |
| Default value | An optional typed constant validated against the same type/constraints. |
| Summary / description | Optional standard human-readable text for help and discovery. |
| Semantic annotations | Optional structured units/format hints, separate from prose. |

Record fields, enumerators, and variant cases have stable numeric identities,
names, and optional text. A record field can refer to another record; a variant case can carry a
scalar, enumeration, record, list, another variant, or no payload. Use declared tagged cases,
not heuristic unions such as “try integer, then string.” Disallow recursive type
cycles initially; bound nesting depth, field/case counts, string/list lengths,
metadata bytes, and decoded allocation before admitting a schema.

Start with explicit-width integers, booleans, closed enumerations, UTF-8 strings,
records, optionals, and closed variants. Bounded lists are also implemented in profile 1. Floats, maps,
recursive values, open unknown-case preservation, and executable default
expressions should not enter the first richer codec implicitly. A path-shaped
string is still data: it does not open a file or confer a file capability.

### Enumerations

An enumeration is a named type with a bounded set of named constant values,
analogous to a C++ `enum class`. It selects exactly one value and carries no
per-value payload. A variant instead selects a case with a declared payload
shape, which may be empty. Keep the two types distinct even when a variant
happens to contain only payload-free cases.

An enum definition has a stable type ID, canonical name, explicit fixed-width
integer representation (recommend u32 by default), and a bounded enumerator table.
Each enumerator has a unique canonical name, an explicitly assigned numeric
value within that representation, and optional summary/description text. The
numeric value is its wire identity; declaration order must not determine it.
Initially reject numeric aliases, duplicate names, and values outside the declared
representation. Removing an enumerator never makes its number available for reuse.

Conceptual schema and corresponding C++ binding:

```yaml
name: ProcessingMode
underlying: u32
values:
  - {name: fast, value: 1, summary: "Favor processing speed."}
  - {name: balanced, value: 2, summary: "Balance processing speed and precision."}
  - {name: thorough, value: 3, summary: "Favor processing precision."}
```

```cpp
enum class ProcessingMode : uint32_t {
    fast = 1, balanced = 2, thorough = 3
};
```

A parameter refers to this enum type; its default can be a typed enumerator such
as `ProcessingMode.balanced`. Enums also work inside structs, optional values,
lists, variant payloads, and constructor arguments. No implicit zero value or
first-enumerator default exists. Optional None remains distinct from a named
value such as `none`.

CLI symbolic assignment `mode:fast` and dot selection `mode.fast` normalize to the
same enum value. Prefer dot selection in canonical pretty-printed output, matching
the user's resolution direction. Within a struct, `options:{mode:fast, limit:10}`
and `options.mode.fast options.limit:10` are equivalent. The schema determines
that `fast` is an enumerator, not a record field or variant case. A selected
enumerator is terminal: it takes no payload or additional path segments.
Reject raw numeric CLI values, unknown names, and multiple selections of one enum;
use names for discoverability. This does not require adding the tagged-colon or
constructor-style alternatives for variants.

The codec writes the explicit numeric value in the schema's declared width and
byte order, not a native C++ object representation or the name's text. Validate
membership before encoding and after decoding, including values supplied through
an unchecked C++ integer-to-enum cast. Initial enums are closed: an unknown wire
value is an error, never silently coerced to a default. Adding a value requires
schema/version compatibility handling so an older receiver is not sent an
unsupported value. Open enums/unknown-value preservation can be a separate later
profile. Bit flags and combinations such as `read|write` are a distinct proposed
bitmask type, not implicit enum behavior.

## 2. Optional values, absence, and defaults

Keep three distinctions: whether a caller supplied a field; the field's resulting
typed value; and whether its type can represent absence. Optional does not mean
“zero,” and defaulted does not mean “nullable.”

| Field declaration | Omitted by caller | Explicit `null` | Explicit zero/empty value |
| --- | --- | --- | --- |
| Required T, no default | Error | Error | Accept if valid for T. |
| T with a default | Use the default | Error | Use the explicit value. |
| Optional<T>, no default | None | None | Some(value), if valid for T. |
| Optional<T> with a default | Use that default | None, suppressing the default | Some(value). |

Examples: `limit:0` is zero, `label:""` is an empty string, `label:null` is None
only when label is optional, and `label:"null"` is the literal string. A unit
variant case named `none` is still a *present variant value*; `source.none` and
`source:null` have different types/meaning when source is optional.

Defaults are typed constants, not strings to reinterpret in each target language.
They may be scalars, typed enum values, records, tagged variants, or bounded lists. Validate defaults
at schema publication and again when reading untrusted metadata. No environment
lookup, clock read, allocation of an object, method invocation, or executable
expression is allowed inside a metadata default. Provider-dependent behavior
should be explicit in the API (for example an `automatic` variant case).

Recommended compound-default rule: a whole-value default applies only when that
field is omitted. If a record is explicitly supplied, resolve its missing fields
using the record's own field defaults/optional rules. Do not silently overlay a
partial explicit record onto a different whole-record default. Prefer field-level
defaults for tunable options so `options:{limit:10}` has predictable behavior.
An absent optional record does not materialize merely because its child fields
have defaults; `options:{}` does materialize it and resolves those children.

Normalize after parsing the entire value tree. An omitted default and an explicitly
provided equal value then have the same canonical value. This also gives class
code a sound way to compare normalized constructor arguments for singleton reuse;
the class still decides whether differences conflict, are ignored, or reconfigure
state. The manager does not invent argument equality or a reuse policy.

Changing a default can change behavior without changing a byte layout. Treat it
as a semantic schema/version decision, not a documentation-only edit. The caller
and provider must agree on the parameter schema used for normalization.

## 3. Standard parameter descriptions

Every parameter, nested field, enumerator, and variant case **may** carry:

- `summary`: one short sentence stating its purpose (suggested limit 160 UTF-8 bytes).
- `description`: plain UTF-8 text explaining meaning and usage (suggested limit
  2048 bytes), referenced through a bounded metadata string table.

Use the same fields for constructors. Type-level descriptions explain reusable
structure; field-level descriptions explain its meaning at this call site. A
missing description is legal; inspectors can still show type, required/optional
state, bounds, and defaults. Do not fabricate explanatory text from an empty field.

Use a consistent authoring order: purpose, interpretation/units, relevant limits,
and the effect of omission. Keep machine-readable ranges, default values, units,
and enum/variant choices in structured metadata, rather than making a consumer parse
prose to find them. Help output combines those fields with the supplied text.
Examples can be optional typed values validated by the schema, not arbitrary
strings assumed to be valid calls. Initial text can use one declared language;
localized alternatives can later be keyed by the same stable field/case IDs.

Conceptual example (not the current ELF record layout):

```yaml
name: limit
id: 3
type: u32
default: 100
constraints: {min: 1, max: 1000}
unit: items
summary: "Maximum number of entries to return."
description: "Limits this response; it does not change the directory contents."
```

The canonical extractor must preserve text, examples, constraints, case meanings,
and constructor documentation. `intf`/help, generated C++ documentation, and future
automated callers should consume the same description records. Text-only changes
do not change wire compatibility; an artifact/documentation digest may change
independently of a semantic schema fingerprint.

Descriptions aid discovery and planning; they do not prove behavior or grant
permission. Automated callers treat metadata text as untrusted descriptive data,
not instructions that override user intent, authorization, or validation. Declared
side effects, required authority, and error meanings also belong to the member's
structured contract. Inspection never executes a constructor, getter, or method.

## 4. Dot resolves a typed path; colon supplies its value

The base form is `parameter-path:value`, with whitespace between top-level
assignments. Dot selects the next segment using the **known schema**:

| Current schema node | Meaning of the next dot segment |
| --- | --- |
| Input record / struct | A declared parameter or field. |
| Enumeration | One declared enumerator, selecting that complete value. |
| Variant | One declared case, selecting that case. |
| Selected case with record payload | A field of that payload. |
| Optional record/enum/variant | Descend into its present value; selecting a child makes it Some. |
| Scalar, string, selected enumerator, or unit | Further path segments are invalid. |

The same spelling can be a record field in one schema and a variant case in
another. Within one schema it is unambiguous; there is no field-vs-case guessing.
Resolution never dereferences a remote object, invokes a property getter, or
interprets a native address. A command/member expression may also use dots in a
future shell, but must remain separate from resolving its input parameter paths.

Use case-sensitive identifiers starting with a letter or underscore, followed by
letters, digits, underscores, or hyphens. This accommodates `param-name` from the
requested syntax. Literal dots/colons are excluded from canonical field names in
the initial profile. Renames/aliases need explicit metadata and duplicate-ID
checks; do not silently normalize hyphens, underscores, or case.

All examples below are proposed native-shell syntax:

```text
counter initial:10 ceiling:100
textprocessor text:"string with spaces"
textprocessor text:'string with spaces and " quotes'
textprocessor options:{limit:10, enabled:true}
textprocessor options.limit:10 options.enabled:true
```

The last two forms normalize to the same record. Inside braces, commas separate
fields and whitespace is allowed; outside braces, whitespace separates assignments.
Brace member paths are relative to that record. A dot in a string value is ordinary
text, not another path segment: `file-name:"report.txt"`.

## 5. Variants using dot selection

Recommended primary spelling: `parameter.case:payload`.

Suppose source has cases `text: string`, `file: {path: string, offset: u64 = 0}`,
and payload-free `stdin`:

```text
process source.text:"Hello world"
process source.file:{path:"/etc/motd", offset:0}
process source.file.path:"/etc/motd" source.file.offset:0
process source.stdin
```

A bare path is allowed **only** when it selects an enum value or a payload-free
variant case.
`source.stdin` supplies the complete value; bare `source`, `enabled`, or
`source.text` fails. Ordinary booleans remain explicit: `enabled:true`.
This keeps unit cases pleasant without introducing generic presence flags.

Structs and variants compose, including within brace notation:

```text
process job:{source.file:{path:"/etc/motd"}, options:{limit:10}}
process job.source.file.path:"/etc/motd" job.options.limit:10
process output.file:{path:"/tmp/result", encoding.utf8}
```

In the last example encoding is a variant with a payload-free utf8 case. No
case inference is allowed from the payload's shape. A quoted value for a
variant still requires its explicit case: `source:"hello"` is not shorthand
for `source.text:"hello"`.

The earlier alternatives are retained for comparison, not accepted aliases:

| Form | Example | Tradeoff |
| --- | --- | --- |
| **Dot selection — selected direction** | `source.text:"hello"` | Uses one resolution operator for fields and cases; supports deep paths. |
| Tagged colon | `source:text:"hello"` | Compact, but overloads colon as both assignment and case selection. |
| Constructor style | `source:text("hello")` | Familiar, but looks like execution and adds parentheses. |
| Tagged record | `source:{case:text,value:"hello"}` | Explicit for generic tools, but verbose and introduces synthetic keys. |

Choose one primary native CLI form, not four interchangeable grammars. Canonical
machine export may separately use `{case_id, value}` without making that CLI syntax.

## 6. Quoting and lexical rules

Double quotes delimit strings and support a small specified escape set:
`\"`, `\\`, `\n`, `\r`, `\t`. Literal UTF-8 text is accepted; reject malformed UTF-8,
unknown escapes, unterminated quotes, and strings beyond the schema's byte bound.
Single quotes contain literal text, including double quotes and backslashes;
use double-quoted form when the string itself contains a single quote.

```text
title:"A \"quoted\" title"
title:'A "quoted" title'
title:"O'Brien"
path:"/tmp/a:b.txt"
```

Quote values containing whitespace, commas, braces, colons, or quote characters.
Bare strings may use a conservative delimiter-free token form when the schema
expects a string. Numbers/booleans are parsed by the declared type; strings are
never guessed into another type. Bare `null` is reserved for optional absence;
quote it for literal text. Reject overflow and trailing numeric junk. Do not
implicitly expand variables, glob paths, substitute commands, or concatenate
quoted and unquoted fragments in this value grammar.

Native `os1` must parse the raw command or preserve typed token boundaries before
its current whitespace tokenizer loses quotes and braces. Do not reconstruct
original quoting by joining argv with spaces. An external-shell wrapper has a
different input boundary: pass each assignment as a complete argument while
preserving its inner value-language quoting, for example:

```sh
os1call 'source.text:"hello world"' 'options:{limit:10, prefix:"a b"}'
```

`os1call` is an illustrative host-tool name, not an installed command. The outer
shell removes its single quotes; the inner double quotes reach the value parser.
The native examples need no such outer wrapper layer.

## 7. Conflicts, normalization, and diagnostics

Parse assignments into one typed value tree, then apply omission/default rules
and validate constraints. Never implement assignments as order-dependent mutation
of an already constructed object.

- Reject duplicate assignments to the same leaf, even if their values match.
- Permit disjoint paths under one record or the **same** variant case.
- Reject selecting two cases of one variant or two values of one enum, regardless
  of assignment order. `mode:fast mode.thorough` is a conflict, not an override.
- Reject explicit ancestor/descendant overlap, such as `options:{limit:10}` plus
  `options.limit:20`; do not use last-wins merging. Use one brace value or disjoint
  leaf assignments. This rule also applies to an explicit `null` plus a child path.
- Reject unknown fields, enumerators or cases, missing required values, and
  incompatible value types.
- A selected record-valued case may omit only fields with defaults/optional types.
  An empty record cannot satisfy a required field without a default.

Return a structured diagnostic with the canonical parameter path, input span,
error code, expected type/allowed cases, and a short explanation. Examples:
`source.file.path: required string missing`, `source: both text and file selected`,
`limit: 1001 exceeds maximum 1000`. Suggest corrections without executing a call.

A pretty-printer should emit one deterministic spelling: canonical names, dot
enum/variant selection, explicit string quoting, and stable field ordering. Parse →
normalize → print → parse must preserve the typed value, including None versus
empty strings and variant unit cases. Help can show omitted defaults without
requiring every interactive command to spell them out.

## 8. Canonical binary values and caller-side conversion

Recommended richer representation: bounded length-delimited records keyed by
stable numeric field IDs; scalars and enum values have explicit widths and byte
order, strings/lists
have explicit byte/element lengths, and variants carry a stable numeric case ID
plus a bounded payload. Optionals have an explicit None/Some tag. Do not transmit
native pointers, C++ padding, std::variant layout, or descriptor integers as values.
Capabilities remain separate checked attachments with explicit schema/protocol roles.

Resolve defaults into a complete typed value before encoding. Every known input
field has a normalized value; Optional None is encoded explicitly. This matters
for an optional field with a nonempty default: explicit `null` must not disappear
on the wire and be replaced by the provider's default. A provider validates the
complete canonical record rather than guessing which missing fields the caller
intended to default. Absence in the CLI and None in the wire value are distinct
stages, not interchangeable encodings.

Canonical ordering uses stable field IDs, with duplicate fields, noncanonical
encodings and unknown required features rejected. Initial closed variants reject
unknown case IDs; never decode them as another case or discard their payload.
Freeze exact headers, alignment, extension rules and size arithmetic in a new
versioned codec profile after fixture tests. Do not reinterpret today's flat
metadata v1 or confuse its format version with startup ABI version 2.

The conversion flow is:

1. Retain the authorized class binary or live interface reference and read its
   bounded schema; bind inspection to the exact image/protocol being used.
2. Parse CLI names, dot paths, quotes and braces into typed values using that schema.
3. Resolve omissions/defaults, validate bounds and constraints, and produce errors
   before launch/call submission if the input is invalid.
4. Encode canonical binary values and separately prepare authorized attachments.
5. Pass constructor data through the startup/construction boundary, or method data
   through copied IPC. The provider independently validates schema identity,
   encoded lengths, constraints, and application semantics before dispatch.
6. Decode results using the agreed schema; retain normal Operation/lifetime and
   transport-vs-application error behavior.

A C++ caller bypasses text parsing but uses the **same** value normalization and
codec. Generated optional/variant wrappers can fit the target runtime; today's
freestanding userspace does not acquire a standard C++ library merely because
std::optional/std::variant are convenient conceptual equivalents.

Variable-length values remain bounded. Current inline requests and constructor
data have 256-byte limits; a richer encoding does not raise them. Report an
explicit size error when headers plus values exceed the selected transport's
limit. Shared buffers require the separate mapping/lifetime work in the
[next-phase plan](../archive/native-api/2026-11-05-client-sessions-and-parameter-plan.md).

## 9. Acceptance contracts

- Schema tests for stable references, bounds, cyclic types, duplicate IDs/names,
  invalid defaults, duplicate/out-of-range enumerators, and malformed/oversized
  description strings.
- Golden binary vectors for optional None/Some, empty strings, nested records,
  enum values, selected variant payloads/unit cases, and normalized defaults.
- Equivalent brace/dot forms, quote/escape fixtures, and deterministic parse/print
  round trips; conflicting cases and overlapping assignments must fail in either order.
- Enum fixtures for symbolic/dot equivalence, optional None versus a named none
  value, conflicting selections, unknown wire values, invalid C++ casts, and
  compatibility when a newer schema adds an enumerator.
- Constructor and method fixtures using exactly the same schema/value machinery.
- Provider rejection of malformed binary callers that bypass the CLI, including
  duplicate fields, truncated lengths, overflow, wrong tags and excessive depth.
- Description extraction/help output without executing class code; documentation
  edits do not change wire compatibility, and descriptions cannot grant authority.
- Explicit compatibility tests for old scalar metadata/CLI behavior during the
  migration. The current implementation preserves the legacy syntax while adding
  rich constructor and method tails.
