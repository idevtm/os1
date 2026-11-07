# Documentation

Start here for current contracts, implementation priorities, and operating guides.
Completed plans and older reviews are preserved in the archive.

| Read first | Purpose |
| --- | --- |
| [Architecture](ARCHITECTURE.md) | Current system structure, responsibilities, and data flow |
| [Native API roadmap](native-api/roadmap.md) | Implemented services and remaining priorities: input, scoped discovery, bulk I/O, and storage qualification |
| [Native API index](native-api/README.md) | Implemented ABI profiles, accepted design, and explicitly marked proposals |
| [Latest review](latest-review.md) | Latest whole-project review and subsequent implementation evidence |
| [USB image workflow](hardware/usb-image-workflow.md) | Build, write, verify, and eject a boot image |
| [Project goals](../GOALS.md) | Long-term scope, priorities, and dependencies |
| [Glossary](GLOSSARY.md) and [references](REFERENCES.md) | Domain terminology and external specifications |

## Topic map

```text
docs/
  ARCHITECTURE.md          Current system overview
  GLOSSARY.md              Domain terms
  REFERENCES.md            External specifications
  latest-review.md         Stable review entry point
  native-api/             Object model, roadmap, and design profiles
    abi/                  Implemented records and behavior
  filesystems/            Temporary and persistent filesystem contracts
  kernel/                 Synchronization, ACPI, and measurement clock
  hardware/               USB images, boot logs, and firmware investigation
  shell/                  Interactive files, editor, and language proposal
  reviews/                Latest whole-project review snapshot
  archive/                Older reviews and completed plans or records
    reviews/
    native-api/
    filesystems/
    shell/
    kernel/
    boot/
    tooling/
```

| Topic | Entry point |
| --- | --- |
| Native API | [Contracts, profiles, and ABI index](native-api/README.md) |
| Filesystems | [Temporary and persistent storage](filesystems/README.md) |
| Kernel | [Kernel contracts](kernel/README.md) |
| Hardware and boot media | [Hardware guides](hardware/README.md) |
| Shell | [Current shell and language proposal](shell/README.md) |
| Reviews | [Current review](reviews/README.md) |
| History | [Archive index](archive/README.md) |

## Organization rules

Keep current guides and maintained contracts in their topic folders.
Use descriptive kebab-case filenames; retain conventional uppercase names for root reference documents.
Use dates for immutable review snapshots and implementation evidence.
Place completed plans and historical records in the matching archive topic.
Archiving a record does not close unresolved findings; current indexes must retain links to relevant follow-ups.

Keep the latest whole-project review in `reviews/` and its stable pointer in `latest-review.md`.
When a new review replaces it, move the previous snapshot into `archive/reviews/` and update affected links.
A focused subsystem assessment does not replace a whole-project review.

A document's location does not establish implementation status or ABI stability.
Keep implemented behavior, accepted direction, proposals, and historical evidence explicitly labeled.
Preserve measurements, revision identities, and examples when reorganizing records.
Update repository links and source comments whenever a document moves.

## Filesystem utility extension

- [Metadata and recursive commands](shell/filesystem-tools.md)
- [Text utilities](shell/text-utilities.md)
- [Interactive pager](shell/pager.md)
- [Native metadata and traversal ABI](native-api/abi/filesystem-metadata-abi.md)
- [Implementation and validation record](archive/shell/2026-11-07-filesystem-traversal.md)
