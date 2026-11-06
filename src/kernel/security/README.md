# Kernel security model

This directory reserves the boundary for credentials, permissions, and privilege policy.
Process credentials, user/group identity, and filesystem ownership checks are not implemented yet.
Existing capability rights and explicit launch grants already restrict resource access.

| Current behavior | Remaining policy |
| --- | --- |
| ACPI observation reads cached information without firmware execution | Explicit power control needs authorization, driver quiescence, and recovery |
| Descriptors and native sessions enforce granted rights | Credentials and ownership must govern multiuser service access |

The [passive observation record](../../../docs/archive/kernel/2026-11-05-passive-acpi-observation.md) closes ordinary observation's power side effect.
The [historical R2 finding](../../../docs/archive/reviews/2026-11-03-review.md#r2--p1-acpi-observation-performs-ungated-power-transitions) preserves the original defect.
Future ownership includes credentials, file permission checks, process-control policy, and privilege boundaries needed for remote login.
