# Capability attachments and shared activation — 2026-11-05

Status: implemented and validated. Authorized scope: bounded capability
attachments, an activation service, and a shared Counter with independent clients.

## Contract

- Preserve existing copied IPC operations. Add extended submit/receive/reply/result
  records carrying at most four copied references. Require DELEGATE, attenuate
  rights and directory descendant ceilings, and install receiver-local CLOEXEC
  descriptors. Sender descriptors remain intact. Failed delivery rolls back all
  installed descriptors and preserves the message for retry. Result attachments
  are claimed once across Operation aliases; retained result bytes remain readable.
- Initially transfer console/file/directory objects and IPC client endpoints.
  Exclude providers, Operations, and subscriptions. Request attachments may only
  refer to newer connections than the carrying connection; this prevents cycles
  through pending requests. Reply attachments have no generation restriction:
  completion removes the pending reference before publishing the returned client in the completed Operation.
- An explicitly granted activation endpoint authorizes access to one registered
  class. The first service supports Counter; its executable is an explicit grant,
  not an ambient pathname lookup. The manager serializes activation and supervises
  its child with deadlines. It does not select fresh/shared policy or replay an
  uncertain construction automatically.
- Counter's service-mode construction hook runs on each acquisition. First valid
  arguments create the singleton; matching arguments return the same object,
  preserving its value; conflicting arguments fail. The class returns a copied
  client capability through IPC. Ordinary callers have no policy override.
- A supervised provider may receive a second, matching client endpoint at fd 4
  solely to export itself. Its parent owns stop authority. The service retains
  state until manager shutdown; clients cannot stop it by disconnecting.
- Two independent clients acquire references, share mutations, receive separate
  volatile broadcast subscriptions, and disconnect independently. Existing private
  Counter/TextProcessor examples and all prior checks remain valid.

## Validation

- All **234 host tests pass**, including eight new attachment/launch contract
  tests: rights and directory ceilings, shared file cursors, capture/delivery
  rollback, four-reference bounds, one-time result claims, cycle rejection,
  unread/unclaimed cleanup, and matching supervised client grants.
- Both UEFI and BIOS images build. The full **14 integration checks pass**;
  both affected spawn smokes were rerun after the final reply-validation fix.
- `activationcheck` proves queued construction, identical object generations,
  independent client processes, shared mutations/events, unsubscribe/disconnect,
  conflicting arguments, scope shutdown, and rejection of incorrect interfaces
  and positive application statuses without replay.
- The source-only adversarial review found a positive application status could be
  mistaken for a descriptor in the acquire helper. The manager and client now
  reject it; the real provider fixture exercises this path on both boot modes.
  Final review found no remaining blockers. No existing checks were weakened.
- The first image build exceeded the 96 KiB initrd slot (104,448 bytes). Compiling
  bundled user programs with `-Os` now produces an 88,576-byte initrd, leaving
  9,728 bytes. BIOS kernel ELF is 463,360 bytes, leaving 3,584 bytes of staging
  space. Memory layout and size/overlap assertions remain unchanged.

Reproduction uses the existing host and image configurations:

```sh
cmake --build build-host-tests -j8
ctest --test-dir build-host-tests --output-on-failure -j8
cmake --build build --target os1_image os1_bios_image -j8
ctest --test-dir build --output-on-failure
```

See the [implemented ABI and C++ flow](shared-activation-abi.md).

Shared memory, general registry discovery, credential policy, provider restart,
per-client quotas, move transfer, and generated bindings remain later work.
