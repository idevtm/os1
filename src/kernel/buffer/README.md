# Exclusive Buffer memory

This module manages dedicated Buffer pages and exclusive process access.
It retains original mappings until an Operation finishes or the owner removes those mappings.

```text
create -> owner mappings -> request escrow -> borrower mappings
              ^                                  |
              +--------- acknowledged retirement -+

provider output -> reply escrow -> caller claim
```

The [authority rules](#authority-rules) define descriptor access.
The [mapping rules](#mapping-rules) define page lifetime.
The [retirement rules](#retirement-rules) define CPU and kernel-copy synchronization.

## Authority rules

A retained budget record identifies each process incarnation.
A separate address-space generation identifies its current mapping lifetime.
Exec removes reservations and advances that generation. Process cleanup clears the process pointer before record reuse.

Each transfer advances the Buffer generation.
Buffer instances use `OpenInstance::cursor` for their borrowed authority generation.
Files continue to use this field as a byte cursor. The instance allocation size does not change.
A zero generation identifies a persistent owner alias. A borrowed descriptor must match the current Buffer generation.
During a loan, persistent owner aliases cannot map the Buffer, including loans within one process.
Byte I/O, seek, and directory operations reject Buffer instances and cannot change this generation.

## Mapping rules

| Resource | Bound |
| --- | ---: |
| Bytes per object | 65536 |
| Objects per originating process | 8 |
| Pages across all origins | 128 |
| Mapping records per object | 4 |
| Mapping records per process | 32 |
| Outbound loans per process | 8 |
| Borrowed objects per process | 8 |

Origin reservations consume mapping records. Four origin mappings leave no mapping slot for a borrower.
Explicit unmap can release a reservation during a loan.
The mapping address range starts at `kUserSpaceBase + 0x20000000`.
It contains 32 slots of 65536 bytes. Mapping skips slots that contain existing user pages.
Each mapping is non-executable. New backing pages are zero, including padding after the requested byte count.

Mapping records retain object and budget references. Descriptor closure does not unmap pages.
The original budget retains allocation charges through loans, output moves, and origin exit.
The final object release removes these charges and releases the pages.
`PageFlags::SharedBacking` excludes Buffer pages from anonymous-page destruction.
Process cleanup detaches Buffer leaves before address-space destruction.

Borrowed mappings have a separate role from origin reservations.
This distinction permits same-process loans without removing original reservations during borrower retirement.

## Retirement rules

The user-copy gate serializes ownership changes with translation and direct-map access.
Copies hold the gate until physical access finishes. Buffer transitions hold it through acknowledged TLB retirement.
The gate supports nested use on one CPU. Its interrupt guard prevents scheduler migration during nested use.

Retirement clears revoked leaves before publishing a global request sequence.
Every online CPU reloads its actual hardware CR3 before acknowledging that sequence.
Reading hardware CR3 avoids dependence on scheduler thread publication order.
The protocol assumes the current kernel configuration without PCID or global user mappings.

A TLB interrupt acknowledges before IPC wake processing or general IRQ work.
Lock waiters and concurrent retirement initiators poll pending requests while interrupts are disabled.
Acknowledgement processing takes no lock. Retirement has no timeout that permits early ownership publication.

## Validation limits

Host tests cover ownership, quotas, rollback, stale descriptors, shared backing, and simulated delayed acknowledgements.
Host acknowledgement tests do not execute hardware invalidation instructions.
Integrated UEFI, BIOS, SMP, and runtime measurements remain separate validation requirements.
The complete ASD-STE100 Issue 9 standard and dictionary were unavailable for this document review.
