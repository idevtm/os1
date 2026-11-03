# Execution Plans

> Status: live process
> Owner: repository maintainers
> Last verified: 2026-08-19 at `d255142`

Use a checked-in execution plan when work spans multiple subsystems, has
important ordering constraints, changes a public contract, or is likely to
continue across sessions. A small local fix does not need one.

## Lifecycle

1. Copy [_template.md](_template.md) into `active/` using a descriptive,
   date-prefixed filename.
2. Set status to `active`, name an owner, and record the starting commit.
3. Keep progress, decisions, validation evidence, and follow-ups current while
   implementing.
4. When all acceptance criteria pass, set status to `implemented` and move the
   file to `completed/` in the same change.
5. If the direction is abandoned, set status to `superseded`, link the replacing
   plan or decision, and move it to `completed/`.
6. Update [the documentation index](../README.md) whenever a plan is added,
   moved, or superseded.

The source tree and live architecture document remain authoritative. Plans
capture intent and decisions; they do not override implemented behavior.

## Required Plan Content

- goal and non-goals;
- current evidence and constraints;
- ordered implementation phases;
- observable acceptance criteria;
- progress and decision logs;
- exact validation commands and results; and
- deferred follow-ups with a discoverable owner or destination.

The documentation checker requires active/completed plan files to carry valid
status, owner, and last-verified metadata.
