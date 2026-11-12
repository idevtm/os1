# Shell syntax reference

This reference describes shell expressions and the declaration output of `inspect`.

| View | Purpose |
| --- | --- |
| [Expressions](#expressions) | Read values, invoke methods, and bind results. |
| [Inspection declarations](#inspection-declarations) | Read live object or ELF class descriptions. |
| [Types and values](#types-and-values) | Read type declarations, defaults, and tagged variants. |
| [Future proposals](#future-proposals) | Record ideas for later shell versions. |

## Expressions

The shell uses dots for live members and slashes for file paths.
In command position, `ls` can select an alias, `.ls` selects a binding, and `./ls` selects a file.
Quote a file operand to select a literal name when a live root claims its first segment.
See [name resolution and completion](typed-context.md#name-resolution-and-inspection).

A method can use positional or named inputs.
Enter invokes a method when its required inputs are available.
A property read invokes its getter. A field read returns its stored value.

```text
os.fs.list(path:"/tmp", list:true)
n = os.system.clock.now
n.year
n.format(style:"iso8601")
```

Assignment evaluates the right side once.
Scalar data, records, and lists copy their values. A live object or resource handle retains its identity.
The clock binding retains one sample. Another read of `os.system.clock.now` takes another sample.

## Inspection declarations

`inspect` uses one declaration style for live objects and executable classes.
These declarations are display syntax. The shell does not execute a declaration block.
Use `inspect --details TARGET` for the detailed metadata view.

```text
object .settings {
    field label: String value "my label"
    field enabled: Bool value true
}

property now: UtcTime { get } value unread

class "/bin/sh" {
    constructor(init_script: ScriptPath?)
    record ShellStartup {
        init_script: ScriptPath?
    }
    type OptionalScript = ScriptPath?
    type ScriptPath = String<240>
}
```

`object` identifies a live object or a child object reference.
`class` identifies an ELF class. A class can declare an interface with `implements`.
Methods and constructors list their inputs in parentheses. Methods use `-> Type` for their result.
Events list their payload. Properties show `{ get }`, `{ set }`, or `{ get set }` as metadata permits.
`Void` indicates no result. `unavailable` indicates missing information.

`=` introduces a declared default or initializer. `value` introduces observed state.
`value unread` indicates that a getter would be necessary. `value unavailable` indicates missing live data.
A getter result already exposed by reflection can appear after `value`.
Inspection does not call getters, methods, or constructors. ELF inspection reads metadata without starting the program.
Inspect a member directly to see its declaration. Evaluate the member to read its value.
The details view retains wire IDs, versions, offsets, documentation, units, and Buffer transfer information where available.
Resource and result types such as `Buffer` and retained `Operation` use their named types when supplied by metadata.

## Types and values

```text
type Text = String<256>
type Count = U32 range 0..1000
type Tags = List<Text, 8>

enum Mode: U8 {
    fast = 1
    balanced = 2
}

record FileSource {
    path: Text
    offset: U64 = 0
}

variant Source {
    case text: Text
    case file: FileSource
    case stdin
    case none
}
```

Rich ELF schemas describe `Bool`, signed and unsigned integers, bounded strings, records, closed enums, optionals, lists, and variants.
Integer declarations give widths and ranges. `String<N>` gives a string bound in bytes.
`List<T, N>` gives an element limit. Fixed arrays from flat ELF records use `Array<T, N>`.
Use `T?` for an optional type. `List<Text?, 8>` permits absent elements. `List<Text, 8>?` permits an absent list.

A required argument without a default must be supplied. Omission uses a declared default when one exists.
An optional argument without a default can be omitted. Explicit `null` suppresses a default only for an optional type.
`null` is a typed absence. `unread` and `unavailable` are display statuses.
Some live shell adapters permit omission without accepting `null`. Their declarations mark those inputs `optional`.

### Enums and variants

Inspection can show a typed enum value as `Mode.balanced`.
A variant value includes a case tag. Cases can carry a scalar, a record, or no payload.
For example, `Source.file({path:"/etc/readme",offset:0})` displays a record payload. `Source.stdin` displays a case without a payload.

The rich constructor and method argument parser uses dot selection for cases.

```text
paramdemo source.text:"hello"
paramdemo source.file:{path:"/etc/readme"}
paramdemo source.stdin
paramdemo source.none
```

When a schema declares an optional variant, a caller can supply `null` or select a case.
A selected case named `none` is a present variant value.
The [parameter schema reference](../native-api/parameter-schema.md) defines the accepted call forms and composition rules.
Rich typed arguments follow their schema. General shell bindings currently use the context data types.

## Future proposals

`ref n1 = os.system.clock.now` is proposed syntax for a live member reference.
It would retain a receiver and member. Each read would sample the property again.
Inspection would describe the reference without evaluating it.
