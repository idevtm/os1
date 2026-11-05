# Documentation reorganization — 2026-11-05

This change renames `doc` to `docs` and separates current guidance from historical evidence.
It preserves the preceding uncommitted Native API graduation and roadmap work.

| Current material | Historical material |
| --- | --- |
| Architecture, glossary, references, latest-review pointer | Older project and subsystem reviews |
| Topic indexes, Native API contracts, implemented ABI profiles | Completed implementation plans and validation records |
| Kernel contracts, USB workflow, boot-log guide, UEFI investigation | Original boot, kernel, and tooling milestones |

## Placement and compatibility

The [documentation index](../../README.md) defines the topic hierarchy and maintenance rules.
Current Native API filenames use descriptive kebab-case names.
Historical records retain dated filenames and their evidence.
The latest whole-project review remains accessible outside the archive.
The existing root README, GOALS, and AGENTS files retain their locations.

Markdown targets and repository-relative references were updated across documents, source comments, scripts, and CMake diagnostics.
No compatibility directory or duplicate copy remains at the old root.
External bookmarks to old file paths require the new locations or Git history.
The [Native API index](../../native-api/README.md) still distinguishes implementation from proposals; moving files does not change ABI status.

## Impact assessment

| Area | Assessment |
| --- | --- |
| Project scope | Supports GOALS “Documentation quality” and “Simplicity first” through topic indexes and a separate archive. |
| Architecture | Preserves the current-state role of ARCHITECTURE and separates dated evidence from maintained contracts. |
| Boot/platform | Only documentation references in boot comments and USB diagnostics change; no image format, firmware, or boot logic changes. |
| Memory/isolation/concurrency | No ownership, allocation, permissions, scheduler, locking, or wait behavior changes. |
| Interfaces | File locations and documentation links change; syscall numbers, wire formats, examples, and API authority remain unchanged. |
| Resources | No runtime workload changes; CPU, memory, latency, image size, and device I/O need no new measurement. |
| Validation | Check relocation completeness, links, examples, source-reference-only edits, formatting, and independent review. |

## Validation scope

Compare against the working tree at the start of this task, including the preceding uncommitted documentation work.
Keep pre-existing missing historical source links separate from relocation regressions.
Check new indexes and maintained navigation for valid local paths and headings.
Review example preservation and every non-Markdown change.
Do not run kernel builds, QEMU, benchmarks, or destructive USB examples for this documentation-only change.

The full ASD-STE100 Issue 9 standard and dictionary were unavailable.
New prose follows the repository's stated rules; full standard compliance is not established.

## Check results

| Check | Result |
| --- | --- |
| Relocation inventory | All 73 documents preserved; the old directory is absent. |
| Discoverability | Every relocated document has an inbound link in the documentation tree. |
| Local Markdown targets | No new missing paths; 136 pre-existing historical missing-link occurrences remain. |
| Heading references | No new unresolved heading links; one pre-existing unresolved fragment remains. |
| Examples | All 152 existing fenced blocks preserved, allowing documentation path substitutions only. |
| Non-Markdown files | Six files changed only through documentation path substitutions. |
| Formatting | Applied clang-format to the changed preloader comment and boot-log header; excluded unrelated pre-existing formatting changes. |
| Runtime checks | Not run: no executable behavior, ABI, or workload changed. |

The link and content comparison used a saved pre-task working-tree snapshot.
It distinguishes earlier uncommitted work from this reorganization.
External links and historical source revisions were not revalidated by this navigation check.
