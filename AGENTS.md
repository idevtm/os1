# Project instructions

These instructions apply to this repository and all its subdirectories.
Every development task requires an impact assessment, appropriate validation, and independent review before completion.

| Required step | Details |
| --- | --- |
| Assess the change | [Requirement impact](#mandatory-impact-assessment) |
| Format owned source files | [Source formatting](#source-formatting) |
| Update technical documents | [Documentation standard](#mandatory-documentation-standard) |
| Check the result | [Validation](#validation) |
| Obtain a separate review | [Independent review](#mandatory-independent-review) |

## Mandatory independent review

Obtain an independent adversarial review before declaring a development task complete.
This includes features, bug fixes, refactors, tests, build settings, dependencies, documentation, and project instructions.

Delegate the review to a separate agent with fresh context.
The reviewer must not edit project files.
Give the reviewer these inputs:

- Task requirements and acceptance criteria.
- The complete task diff, including new files, separated from unrelated working-tree changes.
- Relevant source paths, contracts, and documentation.
- The impact assessment and validation results, including skipped checks.

Ask for concrete defects, regressions, missing validation, and conflicts with the requirements.
Require evidence and severity for each finding.
Do not impose a finding quota.
The implementer's own review does not satisfy this requirement.

Verify each finding before making a correction.
Add regression tests for confirmed behavioral defects.
Run the affected checks after corrections.
Obtain another independent review of the corrections and their interactions with the original change.
After three review cycles, report unresolved substantive findings before starting another cycle.
Do not declare completion while blocking findings remain.

If independent review is unavailable, disclose the limitation.
In that case, report the review requirement as unmet.
Include review coverage, finding dispositions, and validation evidence in the final task report.

## Mandatory impact assessment

Before implementation, read the relevant [project goals](GOALS.md), [architecture](docs/ARCHITECTURE.md), and subsystem or ABI documents.
Identify the affected requirements by document and section heading.
Distinguish implemented ABI profiles, accepted designs, proposals, and historical reports through the [Native API index](docs/native-api/README.md).

Assess each area below for every development task.
Include indirect effects through shared code, tests, dependencies, build settings, and documentation.
Group unaffected areas with an explicit reason.

| Area | Required assessment |
| --- | --- |
| Project scope | Educational clarity, subsystem boundaries, simplicity, and runnable vertical slices. |
| Boot and platform | UEFI and BIOS paths, `BootInfo`, higher-half mappings, firmware, and device assumptions. |
| Memory and isolation | Ownership, allocation failure, cleanup, user-pointer checks, permissions, and resource limits. |
| Concurrency | SMP behavior, locking, interrupt context, blocking, wakeups, ordering, and object lifetime. |
| Interfaces | Syscalls, UAPI layouts, descriptors, IPC, binary formats, user programs, and compatibility. |
| Runtime resources | Latency, CPU use, memory use, image size, disk use, and I/O where affected. |
| Validation and documents | Host tests, QEMU smokes, hardware evidence, build reproducibility, and contract accuracy. |

Before completion, measure affected runtime properties against a comparable baseline, even when the task does not claim an improvement.
Record revisions, workload, toolchain, build settings, firmware, machine configuration, CPU count, and memory limits as applicable.
Record the measured values, units, repeat count, and relevant logs.
Report absolute and relative differences, requirement status, and resource tradeoffs.
If the baseline is zero, mark the relative difference as undefined.

Keep pre-existing failures separate from regressions introduced by the task.
Do not weaken requirements to make a result pass.
Do not present host-test or QEMU results as proof of physical-hardware behavior.
If a check cannot run, mark it as unmeasured with the reason and next validation step.

Keep the impact assessment and evidence in the task report and any pull request description.
For documentation-only tasks, assess claims, links, measurements, and executable examples.
Do not build the kernel or run runtime benchmarks solely for a documentation change.

## Source formatting

Format changed project-owned C and C++ files with `clang-format` and the root [.clang-format](.clang-format).
Select the file paths explicitly before invoking the formatter.
Check the resulting diff for unrelated changes.
For other source languages, follow the existing local style and configured tooling.

Exclude `third_party/`, Git submodules, downloaded dependencies, generated files, and build directories from automatic formatting.
Apply these exclusions to editor actions, scripts, and recursive commands.
Do not rely only on formatter ignore files.
Do not modify upstream formatter settings to enforce project policy.
Disable dependency formatting targets that could run automatically when integrating a dependency with CMake.

The current `autoformat.sh` formats the whole `src/` tree.
Do not use it for a scoped change without checking every selected file and the resulting diff.
Keep unrelated formatting out of the task diff.

## Mandatory documentation standard

Write new and revised technical prose in **ASD-STE100 Simplified Technical English, Issue 9**.
This applies to project instructions, READMEs, architecture notes, API guides, specifications, test guides, reviews, and generated reports.
Use the [official ASD-STE100 standard](https://www.asd-ste100.org/) as the language authority.
Use the [project glossary](docs/GLOSSARY.md) for domain terms and their meanings.
The glossary and rules below do not replace the standard or its dictionary.

- Write in the active voice.
- Put one instruction in each procedural sentence.
- Limit procedural sentences to 20 words and descriptive sentences to 25 words.
- Keep each paragraph on one subject, with at most six sentences.
- State a necessary condition before its instruction.
- Use consistent technical terms and American English spelling.
- Preserve exact code identifiers, commands, paths, constants, and ABI names.
- Preserve historical measurements and units during language revisions.
- Mark obsolete commands and interfaces as historical.
- Review generated prose before adding it to the repository.

Do not revise third-party documents to satisfy the project language standard.
Sentence-length checks alone do not establish full ASD-STE100 compliance.
Report review limitations, including unavailable access to the complete standard or dictionary.

### Document structure

Start new technical documents and revised sections with a brief purpose and main result.
Place a visual overview before the detailed explanation.
Use diagrams for flows and relationships, tables for comparisons, and byte layouts for binary formats.
Link overview topics to stable headings that contain the full definitions, constraints, and procedures.
Keep the necessary textual detail alongside the visuals.

Keep technical documents in `docs/`, with subsystem READMEs beside their source.
Follow the [documentation index](docs/README.md#organization-rules) for topic placement, filenames, and archive policy.
The root `README.md`, `GOALS.md`, and `AGENTS.md` are exceptions.
Update [the architecture](docs/ARCHITECTURE.md) when responsibilities or data flows change.
Update affected ABI documents when interface contracts change.
Keep navigation links current.
Clearly separate current behavior, accepted plans, proposals, and historical evidence.

## Validation

Choose checks from the impact assessment and the [documented build and test workflows](README.md).
Run affected host tests for logic that can execute outside the kernel.
Run relevant QEMU smoke tests for guest behavior.
For shared boot or kernel changes, cover both UEFI and BIOS unless an explicit scope reason excludes one path.
For concurrency changes, include relevant SMP coverage.
Record exact commands, results, and any environment limitations.

For documentation-only changes, verify local links, command syntax, and current API claims against the repository.
Check measurement claims against their recorded evidence.
Do not execute destructive examples to validate their syntax.

These instructions require agent checks and reports; they do not create an automated CI or merge gate.
