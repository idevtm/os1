# Kernel contracts

These documents describe maintained kernel interfaces and integration boundaries.
Dated implementation evidence remains available in the archive.

| Document | Scope |
| --- | --- |
| [Architecture](../ARCHITECTURE.md) | Current boot, memory, scheduling, process, device, and observability structure |
| [Embedded Controller](acpi-embedded-controller.md) | Shutdown-time EC discovery, region access, and transport limits |
| [System lifecycle](system-lifecycle-plan.md) | Shell supervision, synchronization, and controlled power transitions |
| [SMP synchronization](smp-synchronization.md) | Locking vocabulary, ownership, wait queues, and synchronization obligations |
| [Measurement clock ABI](measurement-clock-abi.md) | High-resolution measurement and its separation from scheduling deadlines |
| [ACPICA integration](acpica-integration.md) | Implemented backend and explicit limits of the original integration design |
| [Native API](../native-api/README.md) | Resource ownership, calls, events, sessions, and Buffer contracts |
| [Kernel archive](../archive/kernel/README.md) | Completed milestones, allocator/SMP plans, and reliability evidence |

The synchronization and ACPICA documents retain their original creation dates and implementation boundaries.
Their topic placement does not imply that every historical requirement is implemented.
See [latest review](../latest-review.md) for dated findings and subsequent corrections.
