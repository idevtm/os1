# Staged Agent Autonomy

> Status: active
> Owner: repository maintainers
> Last verified: 2026-08-19 at `ffe258c`

This is the live capability and evidence boundary for agent-driven work in
`os1`. A level describes the maximum workflow an agent may perform after the
user has placed that work in scope; it never grants authority to create external
state or make a sensitive design decision on its own.

## Capability Ladder

| Level | Agent capability | Required evidence | Human role |
| --- | --- | --- | --- |
| 0 | Orient and propose | Repository map and cited source evidence | Approve scope |
| 1 | Implement and run fast checks | Clean diff plus `tools/verify.sh fast` | Review behavior and design |
| 2 | Reproduce, fix, and run full validation | Before/after test or smoke evidence plus `tools/verify.sh full` | Review high-risk areas |
| 3 | Prepare a PR and respond to CI/review | Passing required checks and resolved feedback | Authorize publication and approve merge |
| 4 | Merge low-risk maintenance | Proven low-risk class, audit trail, and rollback path | Explicitly authorize merge and monitor exceptions |

Level 2 is implemented and piloted. Levels 3 and 4 are definitions only; they
are not enabled by Phase C and still require explicit user authority for every
external write, publication, or merge.

## Always-Human Decisions

Regardless of level, human judgment is required for:

- user/kernel ABI and persistent on-disk formats;
- memory-safety, privilege, credential, and isolation policy;
- locking order and interrupt-context behavior;
- boot/handoff contracts;
- new third-party runtime dependencies; and
- claims of real-hardware support.

An agent may gather evidence or implement an already-approved design in these
areas, but it must not silently choose the contract.

## Level 2 Pilot — BIOS Marker-Contract Regression

Date: 2026-08-19  
Scope: smoke-harness configuration only  
QEMU: 11.1.0, q35, four vCPUs, BIOS image

The known BIOS child-launch fault is intermittent, so it could not provide a
bounded pilot. Instead, the exercise modeled an accidental smoke-contract drift:
an impossible expected marker was supplied to the real BIOS image, reproduced as
a structured timeout, and then corrected to the boot marker already emitted by
the guest. QEMU command, timeout, boot image, and forbidden markers were held
constant.

Before evidence:

- [schema-v1 JSON](evidence/phase-c-level2-before.json)
- `failed / timeout` after 2.032 seconds
- expected and missing: `phase-c: impossible marker`
- the retained serial output contained `[kernel64] hello!`, proving the booted
  guest and the marker contract disagreed

After evidence:

- [schema-v1 JSON](evidence/phase-c-level2-after.json)
- `passed / all_markers_seen` after 0.259 seconds
- expected and seen: `[kernel64] hello!`
- no missing or rejected markers

Exact reproduction, with only output paths, test name, and the marked value
changing between the two invocations:

```sh
python3 cmake/scripts/run_smoke.py \
  --log /tmp/os1-phase-c-pilot-before.log \
  --summary /tmp/os1-phase-c-pilot-before.json \
  --test-name phase_c_pilot_before --boot-path bios --timeout 2 \
  --marker 'phase-c: impossible marker' \
  --reject-marker '#GP general protection' \
  --reject-marker '#UD invalid opcode' -- \
  qemu-system-x86_64 -machine q35 -smp 4 -serial stdio -display none \
  -no-reboot -no-shutdown -drive format=raw,file=build/artifacts/os1.raw

python3 cmake/scripts/run_smoke.py \
  --log /tmp/os1-phase-c-pilot-after.log \
  --summary /tmp/os1-phase-c-pilot-after.json \
  --test-name phase_c_pilot_after --boot-path bios --timeout 2 \
  --marker '[kernel64] hello!' \
  --reject-marker '#GP general protection' \
  --reject-marker '#UD invalid opcode' -- \
  qemu-system-x86_64 -machine q35 -smp 4 -serial stdio -display none \
  -no-reboot -no-shutdown -drive format=raw,file=build/artifacts/os1.raw
```

The pilot proves the Level 2 loop—reproduce, classify, correct, retain
before/after evidence, and run full validation. It does not prove a kernel bug
was fixed, authorize retrying flaky tests, or expand authority to PR/merge work.

## Maintenance

Every future Level 2 exercise must retain a failing before case and passing
after case, name the unchanged controls, run risk-appropriate regression checks,
and record any human decision. Update this document only when capability or
review boundaries change; individual work remains in its execution plan.
