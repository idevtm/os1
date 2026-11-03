# Level 5 Agentic Software Engineering Roadmap for `os1`

> Status: proposed
> Owner: repository maintainers
> Last verified: 2026-08-20 at `82e5964`
> Supersedes: none

## A. Executive Assessment

### Decision

Do not pursue repository-wide Level 5 autonomy now. Pursue a measured path to a
small, reversible Level 5 envelope for documentation and harness maintenance,
while keeping kernel behavior, public contracts, dependencies, security policy,
and real-hardware claims below Level 5 until separate evidence justifies each
expansion.

`os1` has unusually good foundations for a small teaching operating system:
one local/CI verification contract, 150 host tests, 11 UEFI/BIOS QEMU smokes,
machine-readable smoke outcomes, explicit source ownership, pinned major
dependencies, a technical-debt ledger, and an advisory health report. Those
foundations make reliable Level 3 plausible and provide a credible route to
Level 4 for selected task classes.

They do not yet justify removing human implementation review. The repository has
one controlled Level 2 pilot, a known intermittent BIOS smoke failure, no agent
benchmark corpus, no independent certifier, no recorded comparison between human
review and automated evaluation, no hermetic toolchain definition, no security
scanning or agent identity model, and no release/deployment/rollback system.

For this repository, two target systems must be kept distinct:

1. **Repository factory:** approved specification in; certified commit and
   reproducible QEMU artifacts out. Constrained Level 5 is plausible for proven
   low-risk classes.
2. **Operating-system delivery factory:** approved specification in; released
   software safely running on real machines out. This target is not yet defined
   because `os1` has no production environment, release contract, hardware
   fleet, telemetry/SLOs, or rollback channel.

The next objective is therefore reliable Level 3, not Level 5 branding. Every
later increase must answer: what evidence now replaces the human decision being
removed?

### Current maturity

Using the maturity definitions in the source prompt rather than the
repository-local capability numbering in [AUTONOMY.md](AUTONOMY.md), current
engineering practice has an **L2 ceiling and varies from L0 to L2**, with the
largest gaps around security, deployment, production validation, and recovery.
No capability or task class has demonstrated repeatable L3 operation. The
existing autonomy document reaches the same practical boundary: its Level 2 is
implemented and piloted, while higher levels are definitions only.

### Largest constraints

- No measured agent reliability across representative work.
- No executable specification, acceptance-contract, or risk-classification
  format.
- Builder, reviewer, and certifier roles are not separated.
- Tests are broad but lack systematic property, fuzz, mutation, fault-injection,
  long-run, and schedule-stress evidence.
- The cross toolchain and QEMU environment are discovered/installed, not pinned
  as a hermetic execution image.
- A blocking smoke remains intermittently faulty under
  [TD-007](TECH_DEBT.md#td-007--isolate-the-intermittent-bios-child-launch-fault).
- Security policy is a placeholder, and there is no agent credential, egress,
  or audit design.
- “Production,” deployment, canary, and rollback have no repository-backed
  meaning yet.

### Realistic feasibility

| Target | Feasibility | Reason |
| --- | --- | --- |
| Reliable L3 for ordinary repository work | High | The existing plan, verification, QEMU, and evidence surfaces are sufficient foundations once repeated benchmarks exist. |
| L4 for documentation, tooling, tests, and low-risk userland | Moderate to high | Acceptance can be made objective, but independent certification and human-review replacement must be proved. |
| Constrained L5 for non-runtime docs/harness changes | Moderate | Merge can be reversible and no production runtime exists, but only after shadow evidence and an explicit merge policy. |
| Constrained L5 for selected QEMU-only runtime work | Low to moderate | Requires stronger oracles, deterministic stress, independent evaluation, and proven recovery. |
| Repository-wide or real-hardware L5 | Not currently justified | There is no deployment target, production observation, rollback channel, or evidence for high-risk kernel decisions. |

### Highest-risk assumptions to test

1. The current QEMU and host-test suite catches the same serious defects that a
   human reviewer catches.
2. Marker-based integration tests are strong enough to certify behavior rather
   than merely confirm that a path was reached.
3. The intermittent BIOS failure can be isolated rather than normalized as
   unavoidable noise.
4. Independent evaluation adds meaningful defect detection rather than a second
   correlated opinion.
5. A stable, reproducible build environment can be created without obscuring
   the project's intentionally transparent toolchain.
6. The project has enough real task volume to estimate reliability without
   overfitting to a small historical corpus.

## Establishing Current Reality

Every row below separates demonstrated capability from its consequence. An
absent repository artifact is treated as absent capability; this proposal does
not infer unpublished branch protection, external release processes, secrets,
or infrastructure.

| Current reality | Evidence | Consequence |
| --- | --- | --- |
| Architecture and subsystem ownership are explicit and partly enforced. | [AGENTS.md](../AGENTS.md) maps ownership; [ARCHITECTURE.md](ARCHITECTURE.md#source-tree-ownership) is the live contract; [check_architecture.py](../tools/check_architecture.py) enforces four include-boundary rules with zero current exceptions in [the health baseline](../tools/repository-health-baseline.json). | Agents can navigate safely, but finer rules such as driver/platform policy, UAPI stability, interrupt-context constraints, and locking order remain prose/human-review controls. |
| Build, host-test, QEMU, and CI workflows share one entry point. | [README.md](../README.md#repository-verification), [verify.sh](../tools/verify.sh), and [CI](../.github/workflows/ci.yml) all use `tools/verify.sh fast|full`. | Reproduction is approachable and local/CI drift is reduced. Environment setup and toolchain versions remain variable. |
| Test inventory is broad for the project size. | [QUALITY.md](QUALITY.md) records 150 host tests and 11 QEMU smokes; live `ctest -N` manifests at this snapshot confirm both counts. | The suite is a strong regression harness, but test count is not a correctness probability and coverage gaps remain explicit. |
| Integration outcomes are machine-readable. | [run_smoke.py](../cmake/scripts/run_smoke.py) produces atomic JSON plus serial logs; [retained before/after evidence](evidence/phase-c-level2-before.json) demonstrates the schema. | An automated certifier can consume outcomes without interpreting free-form claims, but current assertions remain predominantly marker based. |
| Runtime debugging has structured snapshots and events. | [ARCHITECTURE.md](ARCHITECTURE.md#observability-abi), [QUALITY.md](QUALITY.md#smoke-evidence-and-failure-triage), and the [smoke triage playbook](playbooks/smoke-failure-triage.md) document `observe`, the event ring, serial logs, and failure classification. | QEMU failures are inspectable. There are no continuous metrics, traces, crash dumps, deterministic scheduler replay, or real-hardware collection channel. |
| CI is verification-only and least-privilege by default. | [ci.yml](../.github/workflows/ci.yml) runs on PRs, `main`, schedule, and manual dispatch with `contents: read`; it references no GitHub secrets and only uploads failure/health artifacts. | CI cannot merge, release, deploy, or repair. This is a good containment baseline, not an L5 delivery system. |
| Major dependencies are pinned and checked. | [DEPENDENCIES.md](DEPENDENCIES.md), [dependency-lock.json](../tools/dependency-lock.json), and [check_dependencies.py](../tools/check_dependencies.py) cover ACPICA, Limine, and GoogleTest. | Source inputs are controlled, but system packages, Homebrew cross tools, CMake, Ninja, `clang-format`, OVMF, and QEMU are not captured in a hermetic environment manifest. |
| Static policy is useful but narrow. | Kernel/user builds use `-Wall -Wextra`; [verify.sh](../tools/verify.sh) runs format, docs, architecture, dependency, and hygiene checks. No first-party configuration for `clang-tidy`, compiler sanitizers, CodeQL, fuzzing, mutation testing, or secret scanning is present. | Existing checks catch drift and compiler warnings but do not replace security analysis, undefined-behavior detection, or test-strength measurement. |
| Planning and knowledge maintenance are explicit. | [Execution Plans](exec-plans/README.md), the [template](exec-plans/_template.md), [TECH_DEBT.md](TECH_DEBT.md), [latest-review.md](latest-review.md), and [doc/README.md](README.md) define lifecycle and indexing. | Agents can discover intent and debt. There is no machine-readable task specification, risk record, acceptance trace, or benchmark result store. |
| Maintenance signals are advisory and intentionally quiet. | [repository_health.py](../tools/repository_health.py) reports drift, test inventory, source size, unreferenced translation units, and footprint deltas; [ADR-001](decisions/001-scheduled-health-report-policy.md) forbids automatic issue/PR mutation. | Entropy is visible without alert churn. Findings still require human triage, and no evidence yet supports automatic remediation. |
| One bounded agent workflow has been exercised. | [AUTONOMY.md](AUTONOMY.md#level-2-pilot--bios-marker-contract-regression) retains a controlled marker mismatch and correction, followed by full verification. | The reproduce/fix/verify loop is demonstrated once. It is not a reliability estimate and did not test a kernel implementation change. |
| A real intermittent failure is unresolved. | [TD-007](TECH_DEBT.md#td-007--isolate-the-intermittent-bios-child-launch-fault) records a BIOS child-launch timeout, explicitly without retries or quarantine. | The factory cannot treat a red/green result as deterministic until flake rate and cause are understood; retries would hide evidence. |
| Security functionality is not implemented. | [src/kernel/security/README.md](../src/kernel/security/README.md) reserves future ownership; [TD-002](TECH_DEBT.md#td-002--add-credentials-and-observe-access-policy) records missing credentials and observe authorization. | Authentication/security changes have no safe autonomous envelope. The guest itself is not a production security boundary. |
| There is no release or deployment pipeline. | The only tracked workflow is [ci.yml](../.github/workflows/ci.yml); repository automation contains no release, publish, canary, production, or rollback command. [GOALS.md](../GOALS.md) defines a teaching OS and QEMU-first development. | Deployment and production-validation maturity are L0. “Production software out” must initially mean a certified commit/artifact, not an autonomously installed OS. |
| Real-hardware evidence is absent. | Every row in [QUALITY.md](QUALITY.md#coverage-matrix) says no repeatable real-hardware evidence is recorded. | QEMU success cannot authorize real-hardware claims or autonomous hardware rollout. |
| Issue conventions are not repository-defined. | PR-numbered commits exist in history, but `.github/` contains only the CI workflow and no issue/PR templates. Multi-step plans are documented under `doc/exec-plans/`. | A factory needs a repository-local intake and acceptance contract rather than depending on unstated GitHub conventions. |
| Workspace isolation is partial. | [verify.sh](../tools/verify.sh) accepts independent build directories and rejects unsafe in-repository locations; no canonical command creates, resumes, or removes an isolated worktree/run. | Concurrent validation is possible, but run identity, lifecycle, cleanup, and recovery are not yet auditable as one unit. |

### Execution and inspection inventory

| Agent operation | Current support | Smallest missing capability |
| --- | --- | --- |
| Create an isolated workspace | Partial: independent build directories are supported. | One wrapper to create/name/checkpoint/retire a disposable Git worktree and bind all output to its run ID. |
| Bootstrap dependencies | Partial: README prerequisites and submodule initialization are explicit. | A non-interactive bootstrap/preflight command plus a pinned environment manifest; do not add a container unless variance proves it necessary. |
| Build | Yes: CMake/Ninja produce UEFI and BIOS artifacts. | Capture normalized tool versions and artifact provenance in the evidence packet. |
| Start the system | Yes: CMake `run*` targets start QEMU in display or serial mode. | A bounded, non-interactive run interface for factory scenarios rather than an open manual session. |
| Reproduce an issue | Partial: the smoke playbook covers structured QEMU failures. | Task manifests for host failures and concurrency/performance cases, plus seed/control capture. |
| Run targeted tests | Yes, by CTest regex/individual targets, but impact selection is manual. | A declared requirement-to-test map checked by the certifier; selection never replaces the final risk-required superset. |
| Run complete tests | Yes: `tools/verify.sh full`. | Stable reference environment and repetition policy for nondeterministic classes. |
| Inspect logs and guest state | Yes: serial logs, smoke JSON, `observe`, and event ring. | Cross-run query/index tooling only if artifact volume later makes plain files inadequate. |
| Inspect metrics/traces | Partial: snapshots/events exist, but no sustained metrics, tracing, or SLOs. | Scenario-specific counters, duration/resource budgets, and bounded soak evidence. |
| Exercise APIs/UI | Not applicable as a web/API product; the current user interface is a serial-driven shell/framebuffer terminal. | Keep serial scenario driving as the canonical automation surface; add visual evidence only for future framebuffer/GUI claims. |
| Reset state | Partial: builds regenerate test images in isolated output directories. | An explicit reset contract that identifies retained evidence versus disposable disks, firmware variables, sockets, and worktrees. |
| Clean up resources | Partial: the smoke runner terminates QEMU and writes atomic outcomes. | Run-level cleanup verification for processes, sockets, worktrees, and temporary capabilities. |

## B. Autonomy Maturity Matrix

These are justified upper bounds at this snapshot, not permissions. “L2” means
humans still inspect essentially all output. A capability is not credited with
L3 merely because an agent could plausibly execute it once.

### By engineering capability

| Capability | Current level | Evidence and boundary |
| --- | ---: | --- |
| Specification | L1 | Goals, architecture, plans, and decisions are strong, but humans author the important contracts and no executable acceptance schema exists. |
| Planning | L1 | A plan template and lifecycle exist; there is no repeated evidence of unattended agent planning producing accepted outcomes. |
| Implementation | L2 (bounded) | One Level 2 harness pilot exists. Humans remain responsible for review and sensitive decisions. |
| Testing | L2 | An agent can invoke a deterministic fast/full interface and parse evidence, but test selection and adequacy remain human judgments. |
| Debugging | L2 (smoke failures) | The triage playbook and JSON/log pair support reproduce/isolate work; TD-007 proves the process is not yet reliably deterministic. |
| Review | L1 | Dated reviews exist, but there is no independent evaluator pipeline or measured defect-detection baseline. |
| Architecture enforcement | L2 | Four high-confidence dependency boundaries are executable; broader semantics, UAPI, locking, and interrupt rules remain human reviewed. |
| Security | L1 | An agent can inspect dependency integrity and proposed boundaries, but there is no threat model, secret scanner, static security suite, agent identity, credential policy, or guest security model. |
| Deployment | L0 | No deployment target or workflow exists. |
| Production validation | L0 | No production environment, SLO, canary, or telemetry is defined. |
| Maintenance/refactoring | L1 | Scheduled health evidence is advisory; there is no autonomous remediation and no task-level success corpus. |
| Incident recovery | L1 | Smoke triage exists, but no automated revert, artifact rollback, hardware rollback, or recovery drill exists. |

### By representative task class

| Task class | Current justified level | Why it stops there |
| --- | ---: | --- |
| Documentation-only change | L2 | Link/index checks are strong, but factual and architectural accuracy still require human inspection. |
| Local mechanical refactor | L2 | Format, host tests, and full verification help; semantic equivalence is not independently certified. |
| Deterministic bug fix | L2 | Reproduce/fix/verify is supported; only one controlled pilot exists. |
| Intermittent/concurrency bug fix | L1 | No deterministic scheduler, repetition contract, or minimized TD-007 reproduction exists. |
| Small userland feature | L2 | QEMU shell smokes can provide a vertical slice, but acceptance and independent review remain manual. |
| Cross-module kernel feature | L1 | Execution plans and full verification exist, but no repeatable unattended implementation evidence exists and high-risk architecture/concurrency decisions remain human-owned. |
| UAPI or boot-handoff change | L1 | These are explicit human-review boundaries and require compatibility/negative evidence not standardized today. |
| Dependency upgrade | L1 | Pins and procedures exist, but release-note/provenance review, toolchain effects, and upstream trust remain human tasks. |
| Performance change | L1 | No stable benchmark, budget, or regression threshold is defined. |
| Persistent-data migration | L0 | No persistent filesystem/schema or migration/rollback machinery exists. |
| Authentication/security change | L0/L1 | The credential and permission model is not implemented or decided. |
| Billing/payment change | L0 (not applicable) | No billing or payment subsystem exists; no autonomy claim is meaningful. |
| CI/infrastructure change | L2 for CI-only edits | The workflow is testable, but environment pins and rollback of external effects are absent. |
| Release/deployment change | L0 | No release or deployment system exists. |
| Real-hardware support claim | L0/L1 | No repeatable hardware evidence exists and human review is permanently required by current policy. |

## C. Target L5 Architecture

### Repository-specific operating model

The first meaningful factory target is:

```text
human-approved intent and risk policy
                |
                v
     discovery and clarification
                |
                v
  versioned specification + acceptance contract
                |
                v
 deterministic risk/envelope classification
                |
                v
       plan and affected-owner map
                |
                v
 isolated, least-privilege implementation run
                |
                v
 targeted checks -> verify fast/full -> stress/fault checks
                |
                v
 independent certifier + hidden acceptance cases
                |
                v
 integration replay on current main
                |
                v
 reproducible commit/artifacts + evidence certificate
                |
                v
 shadow decision or policy-authorized merge
                |
                v
 QEMU canary and post-merge observation
                |
         promote, repair, or revert
```

A later real-hardware branch may extend the flow, but only after the repository
defines a release artifact, lab inventory, cohort/canary model, health signal,
and recovery mechanism.

### Objective transition gates

| Transition | Gate question | Required objective answer |
| --- | --- | --- |
| Intent → discovery | Is the requested outcome authorized and bounded? | Intent owner, non-goals, maximum risk tier, resource budget, and prohibited effects are recorded; unresolved authority is zero. |
| Discovery → specification | Is repository reality sufficiently known? | Affected contracts, source owners, tests, dependencies, and known debt are linked; assumptions are enumerated and every material ambiguity is resolved or escalated. |
| Specification → risk classification | Can success and failure be observed without reading the implementation? | Each requirement maps to at least one positive and one relevant negative acceptance case; risk inputs are complete and schema-valid. |
| Risk classification → plan | Is the task inside the active autonomy envelope? | A deterministic policy returns an allowed level; any high-risk override has a human-signed decision; no prohibited capability is requested. |
| Plan → implementation | Can the run fail without harming trusted state? | Clean isolated worktree, unique run ID, fixed input commit, bounded filesystem/network/CPU/time/token permissions, no standing secret, and a cleanup checkpoint exist. |
| Implementation → deterministic verification | Does the candidate satisfy mechanically checkable contracts? | Targeted tests and the risk-required `verify` mode pass once without retry; no unapproved path, dependency, ABI, or artifact change exists. |
| Verification → independent evaluation | Are the supplied tests sufficient and is the diff consistent with the accepted specification? | A certifier that did not see builder reasoning passes hidden/derived cases, change-impact review, adversarial failure-path review, and required security checks with no unresolved severity-1/2 finding. |
| Evaluation → integration | Does the candidate still work on current `main`? | Clean rebase/merge simulation plus a fresh required verification run passes; evidence is tied to the integrated tree hash. |
| Integration → artifact certification | Can the result be reproduced and audited? | Two clean builds in the reference environment produce the required normalized hashes; provenance, logs, test results, diff, and rollback instructions form a complete evidence packet. |
| Certification → merge | Is autonomous merge authorized for this task class now? | Risk tier is on the active allowlist, statistical gate remains healthy, branch protections/checks pass, budget is respected, and no halt condition is active. |
| Merge → QEMU canary | Does the integrated result preserve baseline behavior? | Full QEMU matrix and task-specific probes pass from the merged revision; health deltas remain within approved ratchets. |
| Canary → promotion | Is there evidence of safe operation, not merely boot? | Required behavioral invariants, sustained-run window, performance/resource budgets, and failure counters pass. Until such signals exist, promotion stops at certified repository artifacts. |
| Observation → repair/rollback | Is an automatic response safe and bounded? | A known failure class maps to a pre-authorized repair or revert; at most two repair attempts are permitted before halt/escalation; rollback verification must pass. |

### Minimal factory components

Prefer repository-local files and commands before a service or multi-agent
platform:

```text
factory/
├── schemas/                 task, risk, evidence-certificate schemas
├── policies/                versioned autonomy allowlist and halt rules
├── benchmarks/              immutable task inputs and hidden evaluator fixtures
├── runs/<run-id>/           plan, logs, results, costs, decisions, artifacts
└── tools/                   thin orchestration over existing CMake/CTest scripts
```

The exact directory name is a future implementation decision. The important
interfaces are durable task input, isolated execution, machine-readable
evidence, and a certifier that can reject the builder's result.

### Builder and certifier separation

Start with one builder and one certifier, not a team of conversational agents.

- The builder receives the approved specification, public acceptance cases,
  repository map, and bounded tools.
- Deterministic checks run before any model-based certification.
- The certifier receives the specification, final diff, observable artifacts,
  hidden cases, and check results, but not the builder's chain of reasoning.
- The certifier cannot modify the candidate and cannot waive a failed gate.
- An adversarial/security pass is a separate checklist or tool stage only when
  the task's risk tier requires it.
- The certification policy, not either model, emits the final pass/fail result.

Cross-model-family review should be an experiment, not an assumption. Adopt it
only if a controlled comparison shows materially higher serious-defect recall or
lower correlated failure at acceptable cost.

## D. Gap Analysis

| Problem | Repository evidence | Required capability | Expected benefit |
| --- | --- | --- | --- |
| Tasks are prose without an executable acceptance contract. | [Execution-plan template](exec-plans/_template.md) records goals and acceptance bullets but has no task schema or requirement-to-test trace. | Bounded task manifest with intent, non-goals, risk inputs, observable acceptance cases, budgets, and escalation conditions. | Makes clarification, certification, and policy decisions reproducible. |
| No reliability baseline exists. | [AUTONOMY.md](AUTONOMY.md) records one controlled pilot; no benchmark directory or repeated trial results exist. | Versioned 10–20 task replay corpus with hidden checks and repeated trials. | Converts anecdotal success into measured capability by task/risk class. |
| Human review has no measured replacement. | Dated reviews exist, but no review-finding dataset or automated evaluator comparison exists. | Blind human-versus-certifier detection study using historical and seeded defects. | Shows which controls must replace review before review is removed. |
| Builder and reviewer are correlated. | No independent evaluator is wired into [CI](../.github/workflows/ci.yml). | Read-only independent certifier with hidden tests and policy-owned verdict. | Reduces self-confirmation and makes rejection auditable. |
| Toolchain execution is not hermetic. | CI installs current Ubuntu packages and Homebrew `x86_64-elf-gcc`; local `act` intentionally reuses the host toolchain. | Reference environment manifest or pinned image, plus captured compiler/QEMU/firmware versions. | Lowers run-to-run variance and makes benchmark comparisons meaningful. |
| Test breadth is stronger than test-strength evidence. | [QUALITY.md](QUALITY.md#interpretation-boundaries) disclaims exhaustive behavior; no mutation/fuzz/property configuration is present. | Targeted property tests, parser fuzzing, fault injection, and selective mutation testing for host-testable policy. | Demonstrates that tests reject plausible wrong implementations. |
| Concurrency and intermittent failure are weakly reproducible. | [TD-007](TECH_DEBT.md#td-007--isolate-the-intermittent-bios-child-launch-fault) remains open; scheduler/MM gaps lack sustained stress. | Bounded repetition harness, seed/control capture, schedule perturbation, and first-failure retention. | Prevents retries from disguising nondeterminism and gives agents actionable evidence. |
| Static/security evidence is incomplete. | Builds use warnings and dependency checks, but no secret scan, static analyzer, sanitizer, SBOM, or source-input threat policy exists. | Risk-targeted security checks, tracked-secret prevention, provenance/SBOM for release inputs, and prompt-injection boundary. | Makes low-risk autonomous changes less likely to introduce hidden vulnerabilities or leak authority. |
| Autonomy policy is prose-only. | [AUTONOMY.md](AUTONOMY.md) defines levels and human boundaries but no executable allowlist or per-run risk record. | Versioned policy mapping task attributes to maximum action and evidence requirements. | Prevents an agent from self-authorizing a broader scope. |
| Evidence is distributed. | Smoke JSON is structured, while plans, CI logs, test results, diffs, and decisions are separate artifacts. | One immutable evidence certificate referencing content hashes for every required proof. | Lets merge/release gates validate completeness without trusting a summary. |
| Workspace/run lifecycle is not one auditable operation. | `verify.sh` isolates build outputs but does not create worktrees, assign identities, checkpoint, resume, or clean up. | Minimal run wrapper around Git worktrees and existing commands, with explicit recovery metadata. | Contains failures and makes runs reproducible without a large orchestrator. |
| Runtime evidence is reachability-heavy. | QEMU smokes assert serial markers; [QUALITY.md](QUALITY.md) lists stress, protocol, firmware, and real-hardware gaps. | Scenario invariants, counters, bounded soak tests, failure injection, and performance budgets where behavior demands them. | Detects “marker appeared but system is wrong” failures. |
| There is no delivery/recovery target. | No release/deployment workflow exists; real-hardware evidence is absent. | Human-defined release contract, signed artifact manifest, QEMU canary, then optional hardware lab with A/B recovery. | Gives “production” and rollback concrete meanings before autonomous delivery is considered. |
| Architecture enforcement covers only includes. | [check_architecture.py](../tools/check_architecture.py) enforces four rules; [AGENTS.md](../AGENTS.md) names additional human-review boundaries. | Add executable UAPI/layout compatibility, lock/interrupt annotations, dependency budgets, and ownership tests only where false positives are controlled. | Slows architectural entropy as implementation throughput rises. |
| Factory health itself is not measured. | The scheduled [health report](QUALITY.md#scheduled-repository-health) observes repository drift, not agent success, cost, evaluator disagreement, or rollback. | Factory scorecard with task-level outcomes, variance, interventions, cost, escapes, and regressions. | Makes autonomy reversible when evidence degrades. |

## E. Evidence and Certification Model

### Required evidence packet

An autonomous change is certified only when one immutable packet contains or
content-addresses all required fields:

| Section | Required contents |
| --- | --- |
| Identity | Run ID, task/spec version, source commit, integrated commit, risk tier, active policy version. |
| Provenance | Model/runtime identifier, harness version, toolchain/firmware/QEMU versions, dependency-lock digest, environment image/manifest digest. |
| Authority | Human-approved intent where required, permitted paths/actions, resource and cost ceilings, network/secrets policy. |
| Plan | Affected ownership boundaries, assumptions, ordered checks, rollback strategy, and escalation triggers. |
| Change | Patch/diff digest, changed paths, generated-artifact list, dependency/UAPI/persistent-format impact declarations. |
| Static evidence | Compile/warning results, formatting, docs, architecture, dependency, hygiene, ABI/layout, and risk-required security results. |
| Behavioral evidence | Named host/QEMU tests, task-specific positive/negative scenarios, hidden cases, property/fuzz/mutation/fault/stress results required by risk. |
| Runtime evidence | Smoke JSON/log hashes, observed events/counters, timeouts, QEMU arguments/version, performance/resource measurements when relevant. |
| Independent evaluation | Certifier version, requirement coverage, findings with severity/disposition, hidden-test result, and explicit conflict-of-interest separation. |
| Reliability | Trial count, pass distribution, flake/retry count (normally zero), wall time, tokens/cost, and comparison with the accepted baseline. |
| Recovery | Revert or rollback target, recovery command, recovery drill result when required, and automatic halt conditions. |
| Verdict | Machine-issued `certified`, `rejected`, or `escalated`; failed/missing evidence can never be summarized as certified. |

The agent's narrative is diagnostic context, never evidence. A green certificate
must be derivable from referenced checks and policy without trusting statements
such as “the implementation is correct.”

### Evidence requirements by change shape

| Change shape | Additional proof beyond `verify fast/full` |
| --- | --- |
| Documentation | Link/index pass, claim-to-source trace, stale-reference check, and independent factual comparison for live contracts. |
| Parser/format/input boundary | Table-driven invalid cases, size/overflow limits, property or fuzz corpus, and unchanged valid fixtures. |
| Memory/lifetime | Allocation-failure cases, ownership invariants, double-free/use-after-free defenses available to the host harness, stress, and cleanup proof. |
| Scheduler/SMP/interrupt | Multi-seed repetition, ordering/affinity invariants, timeout-free stress, interrupt-context review, and no unexplained variance. |
| Boot/handoff | UEFI and BIOS smokes, layout assertions, malformed/capacity boundaries, artifact-size budgets, and explicit human-approved contract. |
| UAPI/persistent format | Human-approved versioning decision, compile/layout compatibility tests, old/new fixtures, negative compatibility cases, and rollback/forward strategy. |
| Driver/platform | Host policy tests, QEMU device scenarios, error/teardown injection, resource-release proof, and no real-hardware claim without lab evidence. |
| Dependency update | Upstream provenance/release notes, lock diff, license/footprint/SBOM delta, adapter review, focused tests, and full verification. |
| Security/credentials | Human-approved threat model and policy, abuse cases, privilege-negative tests, secret/taint review, and independent security evaluation. |
| Performance | Stable benchmark definition, repeated baseline/candidate distributions, variance/confidence interval, and approved regression budget. |

### Certifier decision rules

The certifier must reject or escalate when:

- any required artifact is missing, malformed, stale, or not tied to the
  integrated commit;
- verification required a retry to become green;
- a task changed a path or contract outside its authorized scope;
- builder-written tests are the only evidence for a new behavior;
- hidden tests, negative cases, or architecture/security rules fail;
- an unresolved severity-1 or severity-2 finding remains;
- run-to-run behavior exceeds the approved variance/flake budget;
- the rollback path is absent for a state-changing action; or
- the active policy version does not authorize the requested merge/deployment.

## F. Security and Autonomy Envelope

### Trust-boundary rule

An execution context must never simultaneously possess all three of:

A. untrusted input, including issue text, web pages, third-party source, logs,
or generated artifacts;

B. private source, credentials, signing material, or production data; and

C. external or state-changing authority such as merge, release, messaging,
network publication, or hardware mutation.

Use three boundaries:

1. **Discovery/research sandbox:** may read untrusted external content; has no
   secrets and no write authority beyond disposable run storage.
2. **Builder/certifier sandbox:** receives a normalized, approved task manifest
   and repository snapshot; egress is default-deny and credentials are absent.
3. **Integration/release actor:** receives only a certified evidence packet and
   content hashes; it does not parse raw issue/web content and obtains a
   short-lived, task-scoped capability only after policy/human approval.

### Risk tiers

| Tier | Examples | Maximum now | Required verification/evaluation | Autonomous deployment/merge | Human approval |
| --- | --- | ---: | --- | --- | --- |
| R0 — observation | Read-only analysis, benchmark replay, health report generation | L2 | Input validation and audit log; no source mutation | None | Scope only |
| R1 — reversible non-runtime | Non-live docs, test fixtures, check diagnostics, owned metadata | L2; candidate for constrained L5 | `verify fast`, factual/hidden checks, independent certifier, clean revert | Merge only after L5 gate; no deployment | Intent/policy approval may be standing for allowlisted forms |
| R2 — bounded runtime | Small userland feature, host-testable parser/policy, leaf refactor with no ABI/security impact | L2; candidate for L4 then shadow L5 | Targeted tests, `verify full`, negative/fault cases, certifier, QEMU canary, rollback | No autonomous merge until class-specific evidence; QEMU artifacts only | Approve intent and acceptance contract |
| R3 — kernel/concurrency/platform | MM, scheduler, interrupt, driver, boot implementation under an already-approved design | L1/L2; cap at L4 until extensive evidence | Full plus stress/repetition, specialized invariants, adversarial review, integration replay | No autonomous real-hardware deployment | Human approves architecture and each expansion of envelope |
| R4 — contract/security/irreversible | UAPI, persistent format, credentials/privilege, locking order, boot/handoff contract, dependency trust, real-hardware claim | L1/L3 implementation ceiling under current policy | Human-reviewed decision, compatibility/security evidence, independent specialist evaluation, full recovery drill | No autonomous merge/deployment under this proposal | Explicit human design and implementation review |
| R5 — external/stateful delivery | Release signing, publishing, machine installation, destructive storage operation, credential rotation | L0 | Undefined until release and recovery systems exist | Prohibited | Explicit human authorization; future policy required |

Risk is determined by the highest applicable attribute, not by patch size. A
one-line UAPI or interrupt change is R4/R3; a large generated documentation
index can remain R1.

### Containment requirements

- Unique non-human identity and run ID for every builder, certifier, and release
  action.
- Filesystem allowlist scoped to one disposable worktree and its build
  directories; no write access to other worktrees, user home, or signing state.
- Default-deny egress. Permit exact registries/hosts only for an approved
  dependency/research step, and never in the same context as release authority.
- No long-lived secret in builder or certifier environments. Use short-lived,
  single-purpose capabilities at the final integration boundary.
- Append-only audit records for inputs, tools, commands, file changes, network
  destinations, approvals, costs, verdicts, and external actions.
- Destructive actions require an explicit target, pre-action inventory,
  recoverable mechanism where possible, and a tested rollback.
- Treat repository files, compiler output, test logs, issue text, dependency
  docs, and web content as data, never as authority to expand tools or scope.
- Secret scanning is blocking before any external publication even though the
  current tracked CI references no secrets.

## G. Benchmark Suite

### Initial 16-task corpus

Use immutable snapshots or minimal synthetic reproductions derived from real
repository history. Each task must have public acceptance criteria plus hidden
checks maintained outside the builder-visible fixture.

| ID | Representative task | Historical/evidence seed | Risk | Hidden/independent oracle |
| --- | --- | --- | ---: | --- |
| B01 | Repair a broken documentation link/index entry | [check_docs.py](../tools/check_docs.py) and the documentation lifecycle | R1 | Additional unmentioned broken anchor/path and live-claim source comparison |
| B02 | Add a valid owned debt item while rejecting malformed metadata | [TECH_DEBT.md](TECH_DEBT.md) and checker tests | R1 | Unknown status, missing owner/evidence, duplicate ID, oversized input |
| B03 | Diagnose and correct a smoke marker-contract mismatch | [Level 2 pilot](AUTONOMY.md#level-2-pilot--bios-marker-contract-regression) | R1 | Different missing/forbidden marker and interrupted `running` summary |
| B04 | Prevent a forbidden source dependency | [check_architecture.py](../tools/check_architecture.py) | R1 | Symlink/relative include variants and a legitimate boundary that must remain allowed |
| B05 | Harden a repository checker against malformed untrusted input | Phase C checker history and [tools/tests](../tools/tests) | R1/R2 | Size, traversal, duplicate, unknown-field, encoding, and symlink cases |
| B06 | Fix a malformed CPIO archive edge case | Host CPIO tests listed in [QUALITY.md](QUALITY.md) | R2 | Generated truncation/overflow/property cases not visible to builder |
| B07 | Fix an ELF/user-address validation edge | `UserElfPolicy` and `UserAddressPolicy` host tests | R2 | Boundary, overflow, non-canonical, permission, and entry-point mutations |
| B08 | Implement a small shell/userland behavior | Existing spawn/exec/shell smokes in [CMakeLists.txt](../CMakeLists.txt) | R2 | Serial interaction, negative command, prompt recovery, both boot paths |
| B09 | Repair allocator ownership/corruption handling | `Kmem` death tests and [QUALITY.md](QUALITY.md) | R3 | Allocation-failure, redzone, double-free, leak, and repeated lifecycle cases |
| B10 | Correct scheduler placement without breaking affinity/cooldown | Run-queue/load-balancer host tests and SMP smokes | R3 | Randomized queue states, repeated SMP seeds, starvation/migration invariants |
| B11 | Correct PCI MSI/MSI-X message/resource behavior | PCI host tests and [TD-004](TECH_DEBT.md#td-004--steer-device-interrupts-beyond-the-bsp) | R3 | Invalid capabilities, teardown, target APIC variation, resource leak cases |
| B12 | Handle malformed ACPI/ACPICA firmware input | ACPICA host tests and [DEPENDENCIES.md](DEPENDENCIES.md#acpica) | R3 | Corrupt table lengths/checksums, missing mappings, OSL failure injection |
| B13 | Isolate the intermittent BIOS child-launch fault | [TD-007](TECH_DEBT.md#td-007--isolate-the-intermittent-bios-child-launch-fault) | R3 | First-failure retention, controlled repetition, no retries, unchanged UEFI control |
| B14 | Make a compatible `observe` ABI extension from an approved spec | [observe UAPI](../src/uapi/os1/observe.h) and ABI host tests | R4 | Old/new layout fixtures, size/version rejection, user/kernel compile consumers |
| B15 | Upgrade one pinned dependency | [DEPENDENCIES.md](DEPENDENCIES.md) | R4 | Provenance/license/pin/footprint changes and deliberately stale lock/doc variants |
| B16 | Implement a cross-subsystem vertical slice from an approved design | Descriptor/VFS gap in [TD-001](TECH_DEBT.md#td-001--choose-the-process-descriptorhandle-contract) and [TD-006](TECH_DEBT.md#td-006--build-vfsfilesystem-and-argvenvp-on-the-chosen-resource-model) | R4 | Ownership/lifetime, negative rights, cleanup, ABI, host tests, UEFI/BIOS behavior |

B13 is initially a diagnosis benchmark, not a required green implementation
benchmark. B14–B16 measure planning/implementation quality under human-approved
contracts; they are not candidates for autonomous merge in the initial envelope.

### Metrics

Record per task and aggregate by task class/risk:

- task success and acceptance-criteria pass rate;
- hidden-check pass rate;
- escaped severity-1/2/3 defects and regression rate;
- architecture, UAPI, security, and scope violations;
- human interventions and human minutes per successful task;
- wall-clock time and active compute time;
- tokens and monetary cost per successful task;
- retry count, flake rate, rollback rate, and recovery success;
- builder/certifier disagreement and false-positive rate;
- run-to-run variance and 95% confidence intervals, not only means/best runs;
- diff size only as descriptive context, never as a success proxy.

### Experimental method

1. Freeze task snapshot, model version, prompts/policy, toolchain environment,
   time/token budget, and acceptance evaluator.
2. Change one major harness variable at a time.
3. Run at least five independent trials per important configuration and task.
4. Use randomized run IDs/seeds while recording them; do not reuse builder
   memory or prior failed-run commentary.
5. Evaluate with hidden cases and a certifier blind to builder reasoning.
6. Include failures and timeouts in the denominator; a retry is a separate run.
7. Compare observed proportions with confidence intervals and report task-level
   distribution so easy documentation tasks cannot hide kernel failures.
8. Treat every model upgrade, toolchain change, evaluator change, or material
   prompt/policy change as a new experimental condition requiring revalidation.
9. Preserve benchmark contamination rules: builder-visible docs may describe
   contracts, but hidden fixtures and final verdict labels remain inaccessible.
10. Periodically add fresh real tasks so the suite does not become a memorized
    regression benchmark.

## H. Maturity Gates

Thresholds below are proposed initial policy. The maintainers must approve the
acceptable failure tolerance before these gates become authoritative.

### L2 → L3: agent owns implementation, human still reviews

- **Hypothesis:** a bounded agent can implement representative R1/R2 work and
  selected R3 work with a predictable success rate and without concealing
  failures.
- **Experiment:** run B01–B13 at least five times each in isolated workspaces;
  human reviewers inspect every candidate and classify findings.
- **Success criteria:** at least 85% observed task success overall, at least 80%
  in every included task class, 100% scope/authority compliance, zero escaped
  severity-1 finding, no unexplained retry-dependent pass, and all failures
  represented by complete evidence packets.
- **Minimum observations:** 65 runs and at least five per task; include at least
  10 live, non-benchmark tasks before changing normal workflow.
- **Failure criteria:** any unauthorized external action, hidden failure,
  corrupted evidence, repeated architecture violation, or task-class success
  below threshold.
- **Rollback condition:** return that task class to interactive L2 and revise
  the harness; do not compensate by adding retries or a second agent.
- **Decision:** advance only passing task classes, not the repository as a whole.

### L3 → L4: unattended implementation and outcome-focused human validation

- **Hypothesis:** specification plus deterministic/independent certification is
  non-inferior to routine human implementation review for the eligible class.
- **Experiment:** blind parallel review of the same candidates by the existing
  human process and the proposed certifier; include historical and seeded
  serious defects and fresh agent outputs.
- **Success criteria:** 100% recall of known severity-1 findings, at least 95%
  recall of severity-2 findings, certifier false-positive rate below 10%, at
  least 95% observed acceptance pass rate with a 95% lower confidence bound of
  at least 90%, and at least 50% fewer human review minutes without higher escape
  or regression rates.
- **Minimum observations:** all 16 benchmark tasks with at least five trials for
  eligible classes, at least 20 live tasks, and at least 20 known/seeded serious
  review findings. High-risk R4 tasks remain human-reviewed regardless.
- **Failure criteria:** one missed severity-1 finding, two missed severity-2
  findings in a rolling 20, evidence/certifier correlation that defeats hidden
  cases, or intervention/cost rising without a reliability gain.
- **Rollback condition:** restore full human code review for the class and add a
  control targeted to the missed defect category.
- **Decision:** remove line-by-line human review only for classes meeting their
  own non-inferiority evidence; humans continue to validate the outcome/spec.

### L4 → L5 shadow: complete factory decides, but cannot merge

- **Hypothesis:** the full factory can make correct merge/reject/recover decisions
  without human implementation inspection.
- **Experiment:** run the complete pipeline on current main, producing a shadow
  verdict and artifact while normal human-controlled delivery remains
  authoritative. Compare decisions and later outcomes.
- **Success criteria:** at least 98% observed correct verdicts, zero severity-1/2
  escapes, zero unauthorized actions, 100% evidence completeness, at least 95%
  recovery-drill success, and no material architecture/quality trend regression.
- **Minimum observations:** 50 eligible shadow changes spanning at least five
  task shapes, plus five forced failure/rollback drills.
- **Failure criteria:** incorrect merge recommendation for a serious defect,
  inability to reproduce a certificate, rollback failure, or evidence tampering.
- **Rollback condition:** disable shadow release decisions, retain benchmark-only
  operation, and repair/revalidate the failed gate.
- **Decision:** shadow success authorizes consideration of constrained L5 only;
  it does not authorize real-hardware deployment.

### L5 shadow → constrained L5: autonomous completion for R1 allowlist

- **Hypothesis:** proven R1 task forms can be merged without routine human code
  inspection while preserving or improving quality.
- **Experiment:** enable autonomous merge for an explicit allowlist such as
  non-live documentation metadata and deterministic checker diagnostics, with
  post-merge QEMU/health observation and automatic revert.
- **Success criteria:** 75 consecutive eligible shadow/canary decisions with
  zero severity-1/2 escape, at least 98% first-attempt acceptance, 100% rollback
  drill success, zero scope/authority violation, and statistically unchanged or
  better regression/architecture/maintenance signals than the human baseline.
- **Minimum observations:** 75 eligible decisions, including 10 intentionally
  failing candidates and five revert drills; no synthetic task may count as a
  successful live merge.
- **Failure criteria:** any serious escape, failed revert, bypassed branch gate,
  unexplained green-after-retry, or cost/resource limit breach.
- **Rollback condition:** automatically remove merge capability and regress the
  class to L5 shadow/L4 pending investigation.
- **Decision:** authorize only named file/task patterns under a versioned policy.

### Constrained L5 → broader L5

- **Hypothesis:** one additional R2 class has equally strong specification,
  certification, canary, and recovery evidence.
- **Experiment:** shadow then canary the new class independently; do not pool its
  results with R1 history.
- **Success criteria:** at least 100 class-specific decisions, zero severity-1
  escape, no more than one severity-2 escape followed by successful automatic
  regression, at least 99% evidence completeness, 100% successful rollback
  drills, and no adverse quality/architecture trend over the comparison window.
- **Minimum observations:** 100 per new class, five failure-injection drills,
  and enough runtime repetitions to bound its known flake mode.
- **Failure criteria:** any uncontrolled side effect, failed recovery, unmodeled
  contract change, or declining lower confidence bound.
- **Rollback condition:** remove only the new class from the allowlist; a common
  factory defect disables all autonomous merge.
- **Decision:** R3/R4 and real-hardware classes require new human policy and are
  not covered by accumulated R1/R2 success.

### Explicit human-review removal experiment

Before reducing review, construct a finding taxonomy from historical reviews
and fresh control reviews:

- functional correctness and missing failure paths;
- memory/lifetime and concurrency;
- architecture/ownership and API consistency;
- security/privilege and untrusted input;
- test inadequacy and false-positive tests;
- documentation/operability drift;
- unnecessary complexity/maintainability.

For each candidate patch, record findings independently from (a) a human
reviewer, (b) deterministic checks, and (c) the certifier. Do not reveal one
review to the others until classification is locked. Seed known defects where
natural samples are too sparse. A human control may disappear only after every
serious category it uniquely catches has either an executable replacement or
measured certifier coverage meeting the L3→L4 thresholds.

## I. Roadmap

Progress is evidence-gated, not calendar-gated. None of these workstreams
implements an OS feature merely to exercise autonomy.

### P0 — Foundations

1. Define the versioned task/acceptance manifest and R0–R5 classifier.
2. Define evidence-certificate schema, severity taxonomy, run identity, and halt
   rules.
3. Assemble B01–B13 from immutable repository snapshots; keep hidden evaluators
   separate from builder-visible inputs.
4. Capture a reference environment manifest for compiler, linker, CMake, Ninja,
   `clang-format`, QEMU, OVMF, Python, host OS, and dependency-lock digest.
5. Baseline human review using historical/fresh candidates and the finding
   taxonomy.

Exit criteria:

- Every benchmark has a reproducible starting commit, public spec, hidden
  oracle, risk tier, time/token budget, and expected verdict.
- Two clean workspaces can run the corpus without shared mutable artifacts.
- Evidence schemas reject missing, oversized, unknown, unsafe-path, and stale
  inputs.
- No merge, deployment, secret, or external-write authority is added.

### P1 — Reach reliable L3

1. Add the smallest run wrapper for disposable worktree, run ID, bounded command
   execution, checkpoint, and artifact capture.
2. Add targeted test selection declarations while retaining `verify fast/full`
   as mandatory supersets at certification.
3. Add bounded repetition/first-failure retention and use it to investigate
   TD-007 without automatic retries.
4. Add high-value parser/property/fault cases revealed by benchmark failures.
5. Run the L2→L3 experiment and publish all trial outcomes.

Exit criteria:

- L2→L3 thresholds pass by task class.
- Every failure is reproducible or explicitly classified as nondeterministic.
- A new maintainer can reconstruct a run from its evidence packet.
- Human review remains required and its findings are captured as evaluator data.

### P2 — Reach reliable L4

1. Implement a read-only independent certifier with hidden cases and a
   policy-owned verdict.
2. Add risk-specific checks: secret scan and static analysis first; selective
   fuzzing, mutation, sanitizers, fault injection, and stress only where they
   improve measured detection.
3. Turn repeated review findings into executable constraints or explicit human
   decisions.
4. Run the blind review-replacement experiment.
5. Permit unattended implementation only for task classes that meet the gate;
   humans validate specification and outcome.

Exit criteria:

- L3→L4 non-inferiority thresholds pass for each eligible class.
- Certifier and builder are isolated and cannot waive deterministic failures.
- Human minutes fall without higher serious-defect, regression, or rollback
  rates.
- R4 boundaries remain human-reviewed.

### P3 — Prove L5 in shadow mode

1. Connect intake, policy, builder, checks, certifier, integration replay,
   artifact production, canary, and recovery into one auditable state machine.
2. Run shadow decisions on real maintenance work without merge authority.
3. Produce repository-factory scorecards and automatic regression signals.
4. Conduct forced timeout, corrupt evidence, stale-main, failed-test, scope
   violation, budget-exhaustion, and revert drills.

Exit criteria:

- L4→L5 shadow thresholds pass.
- No raw untrusted input reaches an actor with merge authority.
- All state transitions are resumable or fail closed.
- The factory demonstrably becomes less autonomous after a simulated serious
  escape.

### P4 — Constrained production L5

For `os1` at this stage, “production” means the authoritative repository and
its certified QEMU build artifacts, not installation on real hardware.

1. Enable autonomous merge for a narrow R1 allowlist only.
2. Require current-main integration replay, protected checks, signed/hashed
   evidence, and automatic revert.
3. Observe post-merge full verification and repository-health deltas.
4. Keep a human kill switch and incident owner outside the factory.

Exit criteria:

- Shadow→constrained thresholds pass and remain healthy during live use.
- Every autonomous merge is attributable, reversible, and covered by a complete
  certificate.
- A failed post-merge canary disables further autonomous merges before repair.

### P5 — Expand the autonomy envelope

1. Propose one task class at a time, beginning with R2 host-testable logic or
   small userland behavior.
2. Add class-specific oracles, recovery, and hidden tasks before shadowing it.
3. Re-run the full gate after model, environment, policy, or certifier changes.
4. Define a separate release/real-hardware program only if the project chooses a
   supported hardware target and operational responsibility.

Exit criteria:

- Broader-L5 thresholds pass for the new class independently.
- No accumulated low-risk evidence is used to waive high-risk controls.
- R3/R4, release signing, and real-hardware deployment remain excluded unless a
  new approved proposal supplies their safety and recovery model.

## J. First Experiments

### 1. Small historical replay baseline

- **Hypothesis:** the current harness can measure agent success consistently on
  B01, B03, B05, B06, and B07 without new orchestration.
- **Change:** package those five snapshots/specs and hidden checks; run five
  independent trials each with the current model and fixed budgets.
- **Measurement:** success, hidden-check pass, interventions, time, cost,
  variance, and evidence completeness.
- **Success threshold:** at least 80% success per task, 100% scope compliance,
  zero lost failure artifact, and no result requiring an unrecorded retry.
- **Next decision:** if measurement is stable, build the remaining corpus; if
  not, fix task/evidence reproducibility before adding autonomy components.

### 2. Flake and environment characterization

- **Hypothesis:** most current variance can be attributed to TD-007 or captured
  environment differences rather than unexplained harness noise.
- **Change:** repeat the unchanged full smoke matrix in isolated build/worktree
  pairs, retain the first failure, and capture tool/firmware/QEMU/host manifests.
- **Measurement:** per-smoke failure rate, failure signatures, run-order effects,
  wall-time distribution, and cross-environment artifact/result differences.
- **Success threshold:** all non-TD-007 smokes show zero failures in 30 runs and
  every TD-007 occurrence has a complete, comparable evidence packet; no retry
  converts a failed certification to pass.
- **Next decision:** isolate/fix TD-007 or lower the affected class's autonomy;
  pin only environment dimensions shown to affect outcomes.

### 3. Human-review detection baseline

- **Hypothesis:** a small number of recurring finding categories account for
  most valuable human review in this repository.
- **Change:** select 10 historical/fresh diffs, add controlled seeded defects,
  and run blind human, deterministic, and current agent reviews.
- **Measurement:** severity-weighted precision/recall and unique findings by
  category, plus minutes and cost.
- **Success threshold:** the study yields at least 20 known serious findings and
  stable classification agreement; this experiment does not authorize review
  removal even if raw recall is high.
- **Next decision:** build executable controls for uniquely human findings, then
  test an independent certifier against the frozen baseline.

### 4. Independent-certifier A/B test

- **Hypothesis:** a context-isolated certifier catches more serious seeded and
  natural defects than builder self-review at acceptable false-positive cost.
- **Change:** hold model, tasks, environment, and budget constant; compare
  self-review with a separate certifier that sees only spec/diff/evidence/hidden
  cases. Test another model family only as a later experimental condition.
- **Measurement:** serious-defect recall, false positives, correlated misses,
  wall time, and cost per correctly certified task.
- **Success threshold:** at least 20 percentage points higher severity-weighted
  recall or elimination of a serious correlated-miss category, with false
  positives below 10% and cost below the human-approved ceiling.
- **Next decision:** adopt the simplest winning configuration; if there is no
  material gain, use deterministic checks plus human review instead.

### 5. Clean-build reproducibility check

- **Hypothesis:** a captured reference environment can make certification
  reproducible without a complex remote build service.
- **Change:** build and fully verify the same commit twice from clean, isolated
  workspaces; compare normalized artifacts and all tool/version manifests.
- **Measurement:** setup success, test equality, normalized artifact hashes,
  unexplained bytes, duration, and operator steps.
- **Success threshold:** 100% test agreement and either identical required
  artifact hashes or a documented normalization that explains every differing
  field; one canonical bootstrap command succeeds in both workspaces.
- **Next decision:** adopt a manifest/script if sufficient; introduce a pinned
  image only if measured host variance remains material.

## K. Failure and Rollback Strategy

### Recovery by stage

| Failure stage | Containment/recovery |
| --- | --- |
| Discovery/specification | Reject or escalate without creating a writable run. Preserve ambiguity and decision record. |
| Builder | Discard disposable worktree or resume from last clean checkpoint; no trusted branch changes. |
| Deterministic checks | Mark rejected; retain all logs and first failure; retries are new experiments, not pass conversion. |
| Certifier | Reject on disagreement or missing evidence; builder cannot appeal by rewriting the verdict. |
| Integration replay | Rebase from the accepted source, rerun required gates, or abandon; stale evidence is invalid. |
| Autonomous merge | Create a normal revert commit, run required verification, and disable the affected allowlist entry until analysis closes. Never rewrite shared history. |
| QEMU artifact/canary | Stop promotion, retain artifacts, restore the last certified artifact/commit, and verify the restore. |
| Future real hardware | No autonomy until A/B or equivalent known-good boot media, health check, remote cutoff, and successful recovery drills exist. |

### Automatic halt and regression rules

- Any severity-1 escape, unauthorized external action, secret exposure, evidence
  tampering, or failed rollback disables all autonomous merge/release and
  regresses the factory to L3 investigation.
- Two severity-2 escapes in any rolling 20 eligible changes regress that class
  to L4/shadow.
- Any green result dependent on retry disables certification for the affected
  check until its nondeterminism is explained and bounded.
- A missing required check, changed policy/evaluator without revalidation,
  unpinned material environment drift, or stale-main integration result halts
  the run closed.
- Falling below a maturity gate's success/confidence threshold removes the
  affected task class from the allowlist automatically.
- A post-merge canary failure blocks further autonomous merges; at most one
  automatic revert and two bounded repair attempts are allowed before human
  incident ownership is required.
- Cost, time, token, storage, or network ceilings terminate the run as
  `budget_exhausted`, never as success.
- Factory-wide defects regress every class; task-specific defects regress only
  the affected class unless common-cause analysis shows otherwise.

### If the agent is confidently wrong

- Before merge, no trusted state changes: reject and destroy/quarantine the
  worktree.
- After an R1/R2 merge, the independent post-merge canary reverts the exact
  commit and disables that allowlist entry.
- For contract, security, boot, or real-hardware work, current policy prevents
  autonomous merge/deployment, so a human remains in the decision path.
- For a future hardware release, absence of verified rollback is itself a
  blocking certification failure.

## Continuous Architectural Entropy Control

Continue the current quiet-health model and promote a signal only after it has
objective remediation and measured precision. Candidate transformations are:

| Recurring feedback/principle | Executable form | Promotion evidence |
| --- | --- | --- |
| Shared layers do not depend on implementation layers | Include/dependency rule in `check_architecture.py` | Zero false positives across benchmark and live diffs |
| UAPI changes are explicit and compatible | Compiled layout/version fixtures and ABI diff | Catches seeded ABI drift without blocking additive approved change |
| Interrupt/locking contracts are visible | Context/lock annotations plus focused static/runtime assertions | Detects historical/seeded misuse with actionable diagnostics |
| Tests must fail for plausible wrong code | Selective mutation score for host-testable policy | Stable score and meaningful surviving-mutant triage |
| Parsers reject hostile input | Fuzz/property corpora with bounded resource limits | Reproduces real/seeded failures and stays deterministic in certification |
| Dependencies do not creep silently | Existing lock plus license/SBOM/footprint delta | Low-noise upgrade trials |
| Docs describe the live tree | Existing index/link/freshness checks plus claim fixtures | Measured reduction in factual review findings |
| Complexity must pay rent | Health-only duplicate/large/dead-code candidates | Human-reviewed precision before any blocking/removal action |
| Recurring incidents become prevention | Incident taxonomy → regression fixture/check | The check catches the original and sibling failures |

Do not impose global file-size, abstraction-count, or coverage-percentage gates.
The existing health baseline correctly treats large low-level files as signals,
not proof of bad design.

## Keeping the Harness Evolvable

| Component | Failure mode solved | Evidence it exists | Benefit measure | Removal/simplification experiment |
| --- | --- | --- | --- | --- |
| Task/acceptance manifest | Ambiguous intent and unverifiable “done” claims | Plans are prose-only | Clarifications, acceptance escapes, certificate completeness | Compare results after removing fields one at a time; delete fields with no predictive/control value |
| Risk policy/allowlist | Agent self-authorizes unsafe action | Current autonomy boundary is prose | Scope violations and escalation accuracy | Replace complex rules with simpler path/task rules if they preserve all decisions in replay |
| Isolated run wrapper | Shared state and irreproducible cleanup | Build dirs isolate outputs, not whole run lifecycle | Cross-run contamination, recovery success, setup time | Use plain Git worktree + shell if it matches a heavier runner's reliability |
| Evidence certificate | Scattered/stale proof | Smoke JSON is structured but other proof is not | Missing/stale evidence rate and audit reconstruction time | Remove redundant fields whose absence never changes certification in replay |
| Independent certifier | Builder self-confirmation | No separate evaluator exists | Serious-defect recall and correlated misses | A/B against deterministic-only or human review; remove if it adds no material recall |
| Hidden evaluators | Overfitting to public tests | No hidden corpus exists | Public-pass/hidden-fail rate | Reduce hidden set if generated property oracles provide equal contamination resistance |
| Repetition/flake controller | Retry hides nondeterminism | TD-007 | First-failure capture and unexplained variance | Remove per-class repetition once long-run evidence shows deterministic behavior |
| Release controller | Unbounded merge/deploy authority | No release actor exists today | Unauthorized action count and recovery time | Keep as simple policy script; avoid service infrastructure until real release volume requires it |

Keep interfaces vendor-neutral: task manifest in, patch/artifacts out; check
results in, certificate out; certificate/policy in, integration action out. Model
prompts, model providers, CI hosts, and storage backends should be replaceable
experimental conditions rather than embedded policy.

## L. What Not to Build

- Do not create a fleet of planner/builder/reviewer/security agents before one
  builder plus deterministic gates plus one certifier beats the baseline.
- Do not add custom MCP servers for Git, CMake, CTest, QEMU, or Markdown while
  repository-local CLIs expose the needed deterministic operations.
- Do not build a distributed orchestration platform before a small task manifest
  and Git-worktree wrapper demonstrate that process state is the bottleneck.
- Do not add a vector database or duplicate documentation knowledge base; the
  short [AGENTS.md](../AGENTS.md) and indexed live docs are already a strong
  progressive-discovery system.
- Do not create autonomous issue/PR churn. Preserve the artifact-only health
  policy until measured triage data supports a better action.
- Do not build deployment, canary, or fleet-management infrastructure before
  maintainers define a supported release target and recovery obligation.
- Do not make global line-count, test-count, coverage-percentage, or mutation
  thresholds proxies for quality.
- Do not let model-written summaries waive failed deterministic checks.
- Do not introduce cross-model review merely for diversity; require measured
  reduction in correlated misses.
- Do not grant builders standing secrets, broad network access, branch write
  access, or shared signing credentials.
- Do not treat generated code volume, agent count, or merge throughput as a
  maturity metric.
- Do not automate R4 design decisions that current repository policy explicitly
  reserves for humans.

## Human Role at L5

| Activity | Expected change |
| --- | --- |
| Routine implementation in certified classes | Disappears after class-specific evidence gates pass. |
| Routine line-by-line code review in certified classes | Disappears only after non-inferiority is demonstrated; outcome/audit sampling remains. |
| Manual invocation of standard tests and artifact collection | Disappears; the factory owns complete, inspectable evidence. |
| Product intent, scope, and prioritization | Remains human-owned. |
| Architecture, UAPI, persistent-format, privilege, locking, boot-contract, dependency-trust, and real-hardware decisions | Remain human-owned under current policy. |
| Acceptance scenario and invariant design | Increases; this becomes a primary engineering activity. |
| Harness, evaluator, benchmark, and containment engineering | Increases substantially. |
| Factory performance and bias analysis | Increases; model/harness changes are experimental conditions. |
| Incident command and exceptional escalation | Remains human-owned, supported by better evidence and recovery drills. |
| Architecture/debt stewardship | Remains; recurring judgments should be converted into checks only when objective. |

Engineers stop being routine typists and diff readers only where they have first
built a stronger system of specifications, invariants, evaluation, containment,
and recovery.

## M. Open Human Decisions

1. What does “production” mean for `os1`: protected `main`, downloadable boot
   artifacts, a QEMU demonstration environment, or supported real hardware?
2. Is autonomous merge desirable at all for a small teaching OS, or is reliable
   L4 the better endpoint?
3. Which exact R1 task forms may eventually receive standing intent approval?
4. What severity taxonomy and acceptable escape/rollback rates reflect the
   maintainers' risk tolerance?
5. Which current human-review boundaries in [AGENTS.md](../AGENTS.md) and
   [AUTONOMY.md](AUTONOMY.md) are permanent, and which may be reconsidered after
   evidence?
6. Must builds be bit-for-bit reproducible, or is normalized reproducibility
   with explained metadata differences sufficient?
7. What token, monetary, wall-time, CPU, storage, and network budgets are
   acceptable per task class?
8. Which model/provider and data-retention policies are acceptable for private
   source, logs, security findings, and unpublished specifications?
9. Should the factory ever receive GitHub merge authority, and what independent
   kill switch/audit owner would control it?
10. Is a real-hardware lab a project goal, and if so, which machine matrix,
    telemetry, remote cutoff, and recovery medium define safe operation?
11. What artifact signing, provenance, licensing, and distribution obligations
    would apply to an official release?
12. The unresolved descriptor/handle, credential, and persistent-format choices
    remain product/architecture decisions; an autonomy program must not decide
    them indirectly through benchmark implementation.

## Final Recommendation

Approve only P0 as the next autonomy objective: define tasks, risk, evidence,
benchmarks, environment, and the human-review baseline. Then use measured P0
results to decide whether P1 is worth building.

The repository is ready to study reliable agent implementation. It is not yet
ready to remove routine human review, autonomously merge runtime changes, or
claim a production software factory. A constrained Level 5 endpoint may be
earned for narrow, reversible repository tasks; every broader claim must remain
false until its own evidence, containment, and recovery gates pass.
