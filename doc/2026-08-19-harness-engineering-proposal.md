# Harness Engineering Proposal for `os1`

> Status: Phases A, B, and C implemented
> Prepared: 2026-08-19
> Repository snapshot: `harness1` at `d255142`
> Source: OpenAI, [Harness engineering: leveraging Codex in an agent-first world](https://openai.com/index/harness-engineering/), 2026-02-11

## Executive Decision

Adopt the article's operating model selectively, with `os1`'s correctness and
educational goals setting the boundary.

`os1` already has a strong technical harness: explicit subsystem ownership,
host-testable kernel policies, dual-path QEMU smoke coverage, stable serial
markers, structured observability, and source-grounded design/review documents.
The highest-leverage next step is not more agent autonomy by itself. It is to
make the existing environment easier for an agent to discover, invoke, and
check mechanically.

The recommended program has four outcomes:

1. A small repository map tells an agent where authoritative knowledge lives.
2. One read-only verification interface reproduces the checks that matter.
3. Architecture, formatting, and documentation invariants become executable.
4. Reviews, plans, dependencies, and technical debt receive lightweight,
   recurring maintenance.

Do **not** adopt the article's zero-manually-written-code constraint or its
minimal merge-gate policy as project goals. Those were context-specific
experiments. Memory management, interrupt handling, synchronization, boot ABI,
UAPI, and real-hardware changes in an OS need risk-based review and blocking
validation regardless of who or what wrote the patch.

## Assessment Basis

This proposal was grounded in the current repository rather than inferred from
the article alone. The review covered:

- [README.md](../README.md), [GOALS.md](../GOALS.md),
  [ARCHITECTURE.md](ARCHITECTURE.md), [REFERENCES.md](REFERENCES.md), and
  [latest-review.md](latest-review.md)
- the current [GitHub Actions workflow](../.github/workflows/ci.yml), root
  [CMake configuration](../CMakeLists.txt), and
  [CMake presets](../CMakePresets.json)
- [host-test registration](../tests/host/CMakeLists.txt), the
  [QEMU smoke runner](../cmake/scripts/run_smoke.py), and the shell wrappers
  [run.sh](../run.sh), [host-tests.sh](../host-tests.sh), and
  [autoformat.sh](../autoformat.sh)
- source ownership under `src/`, current review findings, and the shape of the
  40 Markdown documents that existed under `doc/` before this proposal

The configured local test manifests expose 150 host tests and 11 QEMU smoke
tests. The initial documentation-only assessment inspected that inventory
without executing it; the Phase A implementation subsequently exercised it
through the shared fast and full verification contracts.

## What the Article Changes for `os1`

| Article principle | Current `os1` evidence | Gap | Proposal |
| --- | --- | --- | --- |
| Give agents a map, not a giant manual | `README.md`, `GOALS.md`, `doc/ARCHITECTURE.md`, and `doc/latest-review.md` are rich sources of truth | There is no checked-in root `AGENTS.md`, and the flat `doc/` directory has no complete index | Add a short `AGENTS.md` as a table of contents and `doc/README.md` as the knowledge index |
| Keep repository knowledge authoritative | Architecture, goals, standards, milestone plans, and reviews are versioned | Status metadata varies, historical and live documents are mixed, and freshness is not checked | Define document roles and metadata; validate links, status, and live-document freshness |
| Make the application legible to agents | Serial-driven QEMU tests, stable markers, captured logs, `observe`, and the event ring are unusually strong foundations | There is no single verification command or compact machine-readable failure bundle | Add one `fast`/`full` verification entry point and structured smoke summaries |
| Enforce invariants rather than implementations | The source tree has explicit ownership; CMake enforces image/layout constraints; the compiler uses `-Wall -Wextra` | Most source-boundary rules are prose-only; formatting is mutating and not checked in CI | Add ratcheted architecture checks, a non-mutating format check, and corrective failure messages |
| Treat plans as first-class artifacts | Detailed milestone, migration, integration, and review documents already exist | New work has no uniform active-plan template, decision log, or completion transition | Introduce an execution-plan template and active/completed indexes without moving history initially |
| Turn failures into missing harness capabilities | Reviews repeatedly identify structural prerequisites and source/document drift | Lessons remain distributed across dated reviews and are not always converted into checks | Require each repeated review finding to become a check, playbook, or explicit debt item |
| Perform continuous garbage collection | `latest-review.md` curates review history and several plans mark supersession | Cleanup is manual and episodic | Add a scheduled, initially non-blocking repository-health audit |
| Increase autonomy in stages | Tests and scripts can support agent-driven build, test, and QEMU validation | End-to-end expectations and human escalation boundaries are implicit | Define an autonomy ladder, with sensitive kernel decisions always requiring human judgment |

## Existing Strengths to Preserve

### 1. Testable kernel policy

The separation between the freestanding build and the host-test project is a
good harness design. Parsers, ABI rules, allocators, scheduler policy, virtual
memory, device helpers, and synchronization behavior can be exercised without
booting QEMU. This shortens the feedback loop and makes failures easier to
localize.

Preserve the distinction between:

- fast host tests for deterministic policy and boundary behavior; and
- QEMU smokes for boot integration, hardware-model interaction, and user-visible
  vertical slices.

New code should move into host-testable units whenever that does not distort the
kernel design.

### 2. Agent-legible runtime evidence

The smoke runner already streams serial output, records it, recognizes required
and forbidden markers, terminates early on success, and reports missing markers
on failure. The observe ABI and event ring provide structured kernel state
instead of forcing tools to interpret informal boot logs.

This is the project's closest match to the article's browser/log/metrics
legibility work. Extend this system rather than introducing a parallel test
framework.

### 3. Explicit architecture and historical reasoning

`doc/ARCHITECTURE.md` is a live source-structure contract, while dated plans and
reviews preserve why changes were made. That is valuable for both new engineers
and agents. The proposal should organize and verify this knowledge, not replace
it with a large agent-specific instruction file.

### 4. Reproducible build constraints

The repository already checks important boot-image sizes and layouts, requires
the cross compiler explicitly, pins third-party inputs, and makes UEFI smoke
availability mandatory in CI. These are examples of useful mechanical
constraints with clear failure modes.

## Proposed Target State

```text
AGENTS.md                         small map and operating contract
README.md                        human quick start and product overview
GOALS.md                         long-term product direction
doc/
├── README.md                    complete knowledge index
├── ARCHITECTURE.md              live implementation contract
├── QUALITY.md                   subsystem × validation matrix
├── TECH_DEBT.md                 explicit, evidence-linked debt ledger
├── REFERENCES.md               external standards
├── exec-plans/
│   ├── README.md                plan format and lifecycle
│   ├── active/                  current multi-step work
│   └── completed/               future completed plans
└── ...                          existing historical documents, left in place
tools/
├── verify.sh                    one read-only fast/full interface
├── check_architecture.py        dependency and ownership checks
├── check_docs.py                links, metadata, and index checks
└── repo_health.py               non-blocking maintenance audit
```

The existing dated documents should remain at their current paths during the
first implementation pass. Moving them immediately would create link churn
without improving the harness. The new index can classify them as live,
active, implemented, superseded, or historical in place.

## Workstream 1: Repository Map and Knowledge System

Priority: **P0**  
Effort: small  
First useful result: one pull request

### Add a short root `AGENTS.md`

Keep it under roughly 100 lines and use it as a routing document. It should
contain only:

- the project's purpose and non-goals;
- the authoritative document map;
- source-tree ownership at one-line granularity;
- exact fast and full verification commands;
- the rule that source wins over stale historical documentation;
- high-risk areas that require explicit human review;
- how to record a plan, a decision, or new debt; and
- instructions to leave the worktree clean of generated artifacts.

Do not duplicate the architecture narrative, build guide, or roadmap in this
file. Link to them.

### Add `doc/README.md`

Index all documents using these categories:

- **Live contracts:** architecture, goals, references, quality, debt.
- **Active proposals/plans:** approved or in progress.
- **Implemented designs:** still useful for rationale.
- **Reviews:** current pointer plus dated snapshots.
- **Historical/superseded:** retained for context but not normative.
- **Draft interfaces:** explicitly not the live UAPI.

Each entry should state status, scope, and the document that supersedes it when
applicable.

### Standardize lightweight metadata

Apply a small header to new documents:

```text
Status: proposed | active | implemented | superseded | historical
Owner: subsystem or maintainer
Last verified: YYYY-MM-DD at commit
Supersedes / superseded by: optional relative link
```

Do not mass-rewrite all historical documents. Add metadata when a document is
touched, and keep classification centrally in `doc/README.md` until then.

### Acceptance criteria

- A new agent can find the architecture, goals, current review, build commands,
  and validation commands within two links from `AGENTS.md`.
- Every Markdown document is listed or covered by an explicit index rule.
- Live and historical documents are visibly distinguishable.
- Broken relative links fail a repository check.

## Workstream 2: One Verification Contract

Priority: **P0**  
Effort: medium  
Dependency: Workstream 1 defines the documented commands

### Add one non-mutating entry point

Introduce `tools/verify.sh` with two stable modes:

```sh
tools/verify.sh fast
tools/verify.sh full
```

`fast` should run checks appropriate before every patch handoff:

1. formatting check without rewriting files;
2. documentation/link checks;
3. architecture/ownership checks;
4. host-test configure, build, and test.

`full` should add:

1. cross-toolchain configure and build;
2. boot-layout and image checks;
3. all registered UEFI and BIOS QEMU smokes.

The script should delegate to CMake/CTest rather than duplicate their logic.
Existing wrappers can remain as convenience commands, but CI and agent guidance
should use the same verification contract.

Keep every generated file, mutable OVMF copy, monitor socket, and test log under
the selected worktree's build directory. Support explicit build-directory
overrides so concurrent agents can validate independent changes without sharing
runtime state.

### Align local and CI behavior

The current workflow runs host tests, the cross build, and QEMU smokes, but it
does not check formatting, documentation, or architecture boundaries. CI should
invoke the same underlying targets as `tools/verify.sh`, with GitHub-specific
dependency installation kept outside the script.

### Improve failure artifacts incrementally

Extend the existing smoke runner to emit a small JSON summary beside each log:

- test name and boot path;
- elapsed time and timeout;
- expected, seen, missing, and rejected markers;
- log path;
- QEMU version and normalized arguments; and
- success/failure reason.

Keep the serial log as the primary diagnostic artifact. JSON is for concise
agent parsing and trend analysis, not a replacement for the transcript.

### Acceptance criteria

- A clean checkout has one documented fast command and one documented full
  command.
- Checks do not modify tracked files.
- Local and CI validation select the same tests and invariants.
- Every smoke failure identifies the missing/rejected marker and artifact path.
- A test that cannot run reports why; required CI coverage never silently skips.

## Workstream 3: Mechanical Architecture and Taste Guardrails

Priority: **P0**, after the unified verification command  
Effort: medium

### Begin with high-confidence boundaries

Derive the current include graph first, then enforce a small initial ruleset:

1. `src/common/` cannot depend on kernel or boot implementation headers.
2. `src/user/` cannot include kernel-internal headers; shared contracts must
   come from `src/uapi/` or explicitly shared libraries.
3. The shared kernel cannot depend on either BIOS- or Limine-specific frontend
   implementation.
4. Boot frontends may cross into the kernel only through the documented handoff
   and narrow shared low-level helpers.
5. Vendored/generated code is excluded from project style and ownership rules.

Add further subsystem rules only after they match the architecture that exists.
For example, the documented rule that drivers do not own machine-wide discovery
policy is worth enforcing, but the check must model legitimate driver/platform
interfaces before it becomes blocking.

### Use ratchets for existing violations

If the first graph contains exceptions, record a reviewed baseline and block
only new violations. Each exception must include a reason and a removal path.
This avoids turning the harness rollout into an unrelated source-tree rewrite.

### Add a small set of taste checks

Start with checks that have objective value:

- `clang-format` compliance on project-owned C/C++ files;
- no accidental tracked build artifacts;
- UAPI headers compile as their declared C or C++ interface requires;
- generated layout files are reproducible from their source templates; and
- new `TODO` entries carry a debt/issue reference or an explicit local rationale.

Avoid global file-length limits initially. ACPICA integration and low-level
orchestration have different legitimate shapes, so a naive line limit would
create exceptions rather than clarity. Report large files in repository health
output before deciding whether a subsystem-specific limit is useful.

Every check should explain how to fix the violation in its error message.

### Acceptance criteria

- The enforced dependency rules are documented in `doc/ARCHITECTURE.md`.
- The current tree passes or has a small, explicit ratchet baseline.
- New forbidden dependencies fail locally and in CI with a corrective message.
- Formatting drift is detected without modifying the worktree.

## Workstream 4: Plans, Quality, and Dependency Legibility

Priority: **P1**  
Effort: medium

### Make execution plans operational

Use checked-in execution plans for multi-step work. A plan should contain:

- goal and non-goals;
- current evidence and constraints;
- phased implementation with acceptance criteria;
- progress log;
- decision log with rationale;
- validation evidence; and
- completion/supersession state.

Small changes can still use an ephemeral plan. Do not require a document for
every patch.

### Add `doc/QUALITY.md`

Track validation by subsystem and environment, for example:

| Subsystem | Host tests | UEFI QEMU | BIOS QEMU | Real hardware | Main gap |
| --- | --- | --- | --- | --- | --- |
| Boot/handoff | Yes | Yes | Yes | Partial/manual | Repeatable real-hardware evidence |
| Memory management | Yes | Indirect | Indirect | Partial/manual | Stress and long-run coverage |
| Scheduling/SMP | Yes | Yes | Yes | Partial/manual | Device IRQ distribution |
| Storage | Yes/partial | Yes | Yes | Partial/manual | Persistent filesystem/VFS |
| Networking | Helper tests | ARP smoke | ARP smoke | Partial/manual | Protocol stack |

The table should point to evidence and gaps, not award a vague score. Generated
test counts can be refreshed automatically; judgments remain human-owned.

### Add `doc/TECH_DEBT.md`

Seed it from repeated findings in `doc/latest-review.md`, including the resource
descriptor/handle decision, observe privilege tiers, ACPICA upgrade/footprint
policy, AP-targeted device interrupts, and future TLB-shootdown granularity.

Each item needs impact, evidence, prerequisite, next action, and status. Dated
reviews remain snapshots; the debt ledger becomes the live queue.

### Document major dependencies

Create a compact `doc/DEPENDENCIES.md` for ACPICA, Limine, and GoogleTest:

- exact pin and purpose;
- boundary between upstream and `os1` glue;
- upgrade procedure and required validation;
- local modifications policy;
- footprint or artifact-size baseline where relevant; and
- fallback/removal implications.

This directly addresses the current ACPICA review gap while preserving the
project's good decision to use a mature implementation instead of maintaining a
general AML interpreter.

### Acceptance criteria

- Every active multi-step effort has a discoverable plan or is explicitly small
  enough not to need one.
- The quality matrix links claims to tests or artifacts.
- Repeated review findings have one live debt record.
- Dependency upgrades have a documented, reproducible validation path.

## Workstream 5: Recurring Repository Garbage Collection

Priority: **P2**  
Effort: small after earlier checks exist

Add a scheduled repository-health workflow, non-blocking at first, that reports:

- broken or unindexed documentation;
- live documents whose verified commit is no longer an ancestor of the current
  change without a later verification record;
- new architecture exceptions;
- tracked build artifacts;
- unreferenced source/test files;
- unresolved `TODO`/debt references;
- test inventory changes;
- large-file and duplicate-helper candidates; and
- third-party pin changes without dependency-note updates.

The workflow should produce a concise artifact or open a targeted maintenance
issue/PR only when there is an actionable delta. It must not generate noisy
weekly churn.

Promote a recurring finding to a blocking check only when:

1. the rule is objective;
2. the remediation is understood;
3. false positives are acceptably low; and
4. the check's failure message teaches the fix.

## Workstream 6: Staged Agent Autonomy

Priority: **P2**  
Dependency: stable maps, checks, and failure artifacts

Use a capability ladder rather than a blanket autonomy policy:

| Level | Agent capability | Required evidence | Human role |
| --- | --- | --- | --- |
| 0 | Orient and propose | Repository map and cited source evidence | Approve scope |
| 1 | Implement and run fast checks | Clean diff plus `verify fast` | Review behavior and design |
| 2 | Reproduce, fix, and run full validation | Before/after test or smoke evidence plus `verify full` | Review high-risk areas |
| 3 | Prepare PR and respond to CI/review | Passing required checks and resolved feedback | Approve merge |
| 4 | Merge low-risk maintenance changes | Proven low-risk class, complete audit trail, rollback path | Exception monitoring |

The following always require human judgment, regardless of level:

- user/kernel ABI and persistent on-disk format decisions;
- memory-safety, privilege, credential, or isolation policy;
- locking-order and interrupt-context changes;
- boot/handoff contract changes;
- new third-party runtime dependencies; and
- claims of real-hardware support.

Repository-local playbooks or agent skills may be added later for repeated
workflows such as boot-smoke triage, UAPI changes, and driver bring-up. Build
them only after the underlying workflow is stable; otherwise they encode churn.

## Merge Policy: Recommended Adaptation

The article's fast-correction merge philosophy should not be copied wholesale.
`os1` should use three risk classes:

| Risk class | Examples | Blocking checks | Review expectation |
| --- | --- | --- | --- |
| Low | Documentation index, comments, non-behavioral tooling metadata | Docs, format, architecture checks | One focused review; fast merge is reasonable |
| Medium | Host-testable policy, shell tools, parser changes, test harness changes | Fast checks plus targeted QEMU smokes | Human or independent agent review of behavior |
| High | Boot, MM, interrupts, SMP, syscalls/UAPI, drivers, security | Full host and QEMU matrix; targeted regression evidence | Human review required; real-hardware evidence when claimed |

Flaky tests should not be ignored merely because retries are cheap. A flaky test
may be quarantined only with an owner, linked debt item, expiry condition, and
retained coverage elsewhere. Kernel timing failures are often product signals,
not CI inconvenience.

## Recommended Delivery Sequence

### Phase A — Foundation

Deliver as three small, reviewable changes:

1. `AGENTS.md`, `doc/README.md`, document roles, and the execution-plan template.
2. `tools/verify.sh`, non-mutating format/docs checks, and CI alignment.
3. Initial architecture checks with a reviewed baseline.

Exit condition: a fresh agent session can orient, make a small change, and
produce the same fast validation result as CI without additional instructions.

Implementation record (2026-08-19): the repository and documentation maps,
execution-plan lifecycle, shared fast/full verifier, non-mutating format and
documentation checks, CI integration, and ratcheted architecture rules are now
checked in as one coherent foundation. The current tree has no architecture
exceptions; legacy formatting deviations are pinned by content hash so new or
changed drift fails without rewriting source.

### Phase B — Legibility

1. Add smoke JSON summaries and upload logs/summaries on CI failure.
2. Add `doc/QUALITY.md`, `doc/TECH_DEBT.md`, and `doc/DEPENDENCIES.md`.
3. Convert the next substantial feature into the first active execution plan
   using the new template.

Exit condition: a failure or project gap can be traced from index to evidence to
an owned next action.

Implementation record (2026-08-19): every registered smoke now emits a bounded,
atomic JSON summary beside its full serial log; each summary carries explicit
test and boot-path identity, timing, marker state, stable result classification,
artifact paths, QEMU version, and normalized arguments. CI retains summaries,
serial logs, and the CTest failure log on failure. The live
[quality](QUALITY.md), [technical-debt](TECH_DEBT.md), and
[dependency](DEPENDENCIES.md) contracts link current claims and gaps to evidence,
owners, prerequisites, upgrade procedures, and next actions. The completed
[Phase B execution plan](exec-plans/completed/2026-08-19-phase-b-legibility.md)
made this work the first user of the active/completed plan lifecycle; it records
validation and the decision not to mask a recurring BIOS spawn flake with
retries.

### Phase C — Compounding Maintenance

1. Add the scheduled repository-health report.
2. Promote reliable findings into blocking checks.
3. Add one repeated-workflow playbook at a time.
4. Pilot Level 2 autonomy on a bounded, QEMU-reproducible bug before considering
   broader PR automation.

Exit condition: the repository detects and corrects common drift continuously,
without depending on a periodic large manual cleanup.

Implementation record (2026-08-19): the fast contract now blocks exact
dependency-pin/checksum/document drift, tracked build artifacts, new unowned
task markers, and malformed live debt records. The existing full CI workflow
runs weekly or on manual dispatch, generates bounded Markdown/JSON repository
health evidence even after verification failure, and uploads one artifact bundle
without opening an issue or pull request. Advisory baselines cover test
inventory, architecture exceptions, large files, unreferenced translation units,
and ACPICA/kernel footprint; the first report exposed and led to removal of the
proven-dead legacy `src/boot/bios/disk16.asm` instead of normalizing it.

The repository-local [smoke triage playbook](playbooks/smoke-failure-triage.md)
and [autonomy contract](AUTONOMY.md) record a real-QEMU Level 2 pilot: an
intentional marker-contract mismatch retained `failed / timeout` evidence, the
corrected contract retained `passed / all_markers_seen` evidence, and the full
51-tooling/150-host/11-smoke verification matrix passed. This authorizes the
bounded reproduce-correct-validate loop only; Levels 3/4 and sensitive system
decisions still require explicit human authority. The completed
[Phase C execution plan](exec-plans/completed/2026-08-19-phase-c-compounding-maintenance.md)
preserves the detailed decisions and evidence.

## Foundation Completion Assessment

### Verdict

The harness-engineering foundation is **implemented in the repository**. Phases
A–C cover the article's core prerequisites: a small map into a repository-local
system of record, one local/CI verification contract, mechanically enforced
boundaries, machine-readable runtime evidence, first-class plans and debt,
staged autonomy through Level 2, and a quiet recurring garbage-collection
signal.

What remains is operational acceptance, not another broad foundation-building
phase. Until the gates below have real usage evidence, describe the foundation
as **implementation-complete and operations-provisional**. Do not add more
generic harness machinery merely to make the system look more agent-first.

| Foundation capability | Current evidence | Remaining acceptance evidence |
| --- | --- | --- |
| Repository map and system of record | `AGENTS.md`, the complete documentation index, live contracts, execution-plan lifecycle, and checked links/metadata | One cold-start agent session must complete a real scoped change without private setup instructions or copied context |
| Reproducible verification and isolation | `tools/verify.sh fast/full`, CI parity, build-directory overrides, 51 tooling tests, 150 host tests, and 11 QEMU smokes | Run two worktrees concurrently with distinct build directories and retain proof that artifacts, sockets, firmware copies, and summaries do not collide |
| Agent-legible runtime behavior | Serial logs, schema-v1 smoke summaries, stable markers, the observe ABI/event ring, and a completed before/after Level 2 pilot | Resolve [TD-007](TECH_DEBT.md#td-007--isolate-the-intermittent-bios-child-launch-fault) with a deterministic reproducer or regression; a streak of passing reruns is not completion |
| Enforceable architecture and maintenance rules | Formatting/architecture/dependency/hygiene/debt checks with corrective messages and ratchet baselines | Use normal work to validate false-positive rates; promote only repeated, objective findings rather than inventing speculative rules |
| Recurring garbage collection | Weekly/manual health workflow, bounded Markdown/JSON report, advisory baselines, and artifact-only publication policy | Produce one successful manual GitHub artifact, then review four scheduled reports across the real CI toolchain and record accepted variance or remediation in the owning live document/debt item |
| Human/agent responsibility boundary | `doc/AUTONOMY.md` defines permanent human decisions and proves Level 2 without granting publication or merge authority | Confirm the boundary during the next real high-risk design decision; no Level 3/4 capability is implied by completing these foundation gates |

The foundation is accepted when all four concrete gates are closed:

1. **Cold-start operability:** a fresh agent follows only `AGENTS.md` and linked
   repository documents to implement one real low-risk change, run the correct
   verifier, and hand off a reviewable diff. Record time, missing context, and
   any human intervention; turn each repeated intervention into a map, check,
   playbook, or owned debt item.
2. **Concurrent isolation:** two worktrees run verification concurrently with
   separate `OS1_BUILD_DIR` and `OS1_HOST_BUILD_DIR` values without shared
   mutable state or artifact leakage.
3. **Scheduled evidence:** a manual dispatch and four normal weekly health runs
   produce readable artifacts. Review test inventory, live-document ancestry,
   large/unreferenced source deltas, and footprint variance before changing a
   baseline or promoting a check.
4. **Trustworthy blocking signal:** TD-007 is deterministically reproduced and
   fixed or reduced to a deterministic lower-level regression. Do not close it
   by adding retries, quarantine, or a larger timeout.

These are evidence-gathering tasks. They should result in small corrections to
the existing harness when evidence exposes a real gap, not a new parallel set of
commands, dashboards, or agent-only documentation.

## Post-Foundation Roadmap

The article's strongest lesson is to use real product work to reveal missing
capabilities. Once the four acceptance gates are closed, stop treating the
harness as the product. Apply it to `os1` goals, and feed only demonstrated
friction back into repository scaffolding.

### 1. Use Level 2 on a real OS objective

Resolve TD-007 first because an intermittent blocking smoke weakens every later
signal. Then select a real, bounded vertical slice from the product/debt roadmap.
For high-risk work such as the descriptor/handle model in TD-001, a human first
chooses the contract; the agent may then implement the approved design with
host-testable policy, before/after evidence, and full QEMU validation.

For every failed run, ask which category owns the correction:

- a product defect needs the narrowest deterministic regression;
- repeated mechanical drift needs a blocking check with remediation text;
- a stable diagnostic workflow needs a playbook;
- missing rationale or navigation needs a live document or decision record; or
- unresolved judgment needs an owned technical-debt item.

Do not treat “prompt the agent harder” or “rerun until green” as a corrective
category.

### 2. Build a small replay/evaluation corpus from real failures

Index representative failures only after they occur and have a trustworthy
oracle. Initial candidates already include the smoke deadline race, vendored
artifact symlink substitution, marker-contract drift, duplicate task-marker
masking, and—once deterministic—the BIOS child-launch fault. Each replay should
state the input, expected classification, forbidden workaround, corrective
evidence, and verification tier.

This is an evaluation harness for repository behavior, not a benchmark of how
much code an agent produces. Track first-attempt correct classification,
time-to-owned-subsystem, false-green outcomes, and human intervention.

### 3. Pilot Level 3 without merge authority

After the foundation gates and one real Level 2 product cycle succeed, choose a
low- or medium-risk maintenance change. Let an agent prepare a **draft** pull
request, run an independent adversarial review, respond to CI/review feedback,
and update the branch until required checks pass. Publication and merge remain
explicitly human-authorized external actions.

The pilot succeeds only if:

- local and CI evidence agree without bypasses or hidden retries;
- review comments are answered with code/evidence rather than assertion;
- the final diff remains within its stated risk class and scope; and
- rollback is obvious and documented.

Record the pilot in `doc/AUTONOMY.md`; do not generalize authority from one
successful pull request.

### 4. Increase OS-specific runtime legibility

Extend observability where real work is still hard to diagnose: bounded QEMU
repetition for timing faults, allocation/fault injection, scheduler and IRQ
event snapshots, long-run stress summaries, and reproducible machine/firmware
manifests for any real-hardware claim. Prefer structured state and deterministic
oracles over more informal serial text.

New observability must respect the permanent human boundaries for privilege,
credentials, memory safety, interrupt/locking contracts, boot handoff, and
real-hardware support claims.

### 5. Let garbage collection graduate cautiously

After scheduled evidence demonstrates a low-noise rule, allow the maintenance
workflow to prepare a targeted patch or draft PR instead of only reporting it.
Keep one finding per change, retain the source artifact, and require the same
fast/full checks as a human-authored patch. Do not open recurring issues for
unchanged observations and do not auto-update baselines to make reports green.

### 6. Consider Level 4 only for a proven allowlist

Level 4 is not the default destination for kernel development. Consider it only
after a reviewed sample of successful Level 3 changes demonstrates stable CI,
no escaped regressions, a tested rollback path, and a narrow path/rule allowlist.
Reasonable candidates are mechanical documentation-index repair or regeneration
of explicitly non-behavioral metadata; boot, MM, interrupts, SMP, syscalls/UAPI,
drivers, security policy, dependencies, and real-hardware claims remain
human-merged.

### Recommended next three execution plans

1. **Foundation operational acceptance:** cold-start drill, concurrent-worktree
   proof, manual health artifact, and four scheduled report reviews.
2. **TD-007 deterministic BIOS reproduction:** retain the first failure and
   reduce it without retry/quarantine before changing kernel behavior.
3. **Level 3 draft-PR pilot:** only after the first two plans close, and only on
   a bounded low/medium-risk maintenance task with explicit human publication
   and merge approval.

## Measures of Success

Track outcomes that reflect saved human attention:

- first-attempt success rate for setting up and running `verify fast`;
- median fast/full verification duration;
- time from a failed smoke to identification of the missing subsystem;
- number of architecture rules enforced versus prose-only;
- broken/unindexed/stale live-document count;
- smoke flake rate and retry rate;
- repeated review findings that recur without becoming checks or debt records;
- escaped regressions by risk class; and
- human review time spent on mechanical issues versus design judgment.

Do not use generated line count, number of agent-authored commits, or PR volume
as primary success measures. The project goal is a small, understandable,
technically serious OS, not maximum code production.

## Risks and Mitigations

| Risk | Mitigation |
| --- | --- |
| Guardrails freeze an imperfect current architecture | Start with high-confidence rules and ratchet existing exceptions |
| More documentation creates more stale documentation | Keep `AGENTS.md` small, index document roles, and verify only live contracts for freshness |
| A unified script duplicates CMake/CI logic | Make it an orchestrator over CMake and CTest, not a second implementation |
| Scheduled audits create noise | Begin non-blocking and report only actionable deltas |
| Agent-specific conventions reduce portability | Keep source-of-truth docs tool-neutral; isolate optional skills/playbooks |
| QEMU success is mistaken for hardware support | Make real-hardware evidence a separate quality dimension and human-reviewed claim |
| Faster merge flow hides kernel concurrency defects | Retain full blocking checks and risk-based human review for sensitive subsystems |

## Explicit Non-Goals

- Requiring all code to be agent-generated.
- Reorganizing all existing documents immediately.
- Auto-merging kernel behavior changes.
- Replacing ACPICA or other mature, pinned dependencies simply to make all code
  locally owned.
- Adding generic dashboards before current logs and test outputs have structured
  summaries.
- Encoding every style preference as a lint rule.
- Treating repository automation as a substitute for architecture decisions.

## Immediate Recommendation

Keep the completed Phase A, B, and C contracts on every patch and in CI, and
close the four operational acceptance gates above. The first substantial work
after that should be a real `os1` objective, beginning with deterministic
reduction of TD-007—not another generic harness layer. Let normal use expose
signal and capability gaps, then encode only the lessons that recur.

Pilot Level 3 through a separate execution plan after the foundation is proven.
Level 4 remains a later, allowlisted decision and is not implied by this
proposal; Phase C authorizes neither automated publication nor merge.

That sequence applies the article's central lesson to `os1`: human attention
should go to OS design, invariants, and acceptance criteria, while the repository
itself makes routine navigation, validation, and drift detection reliable for
both people and agents.
