# Kernel Security Model

This directory is reserved for credentials, permissions, and privilege checks;
those facilities are not implemented yet. Authority boundaries are already
relevant to device operations: the current ACPI observe path can attempt power
transitions without a credential or diagnostic-mode gate. See
[review finding R2](../../../doc/2026-11-03-review.md#r2--p1-acpi-observation-performs-ungated-power-transitions).

Expected future ownership includes uid/gid credentials, file permission checks, process-control policy, and privilege boundaries needed for remote login.
