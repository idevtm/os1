#pragma once

// Per-request diagnostics. Strings refer to static storage; no global last-error slot.
struct PowerDiagnostic
{
    const char* stage = "none";
    const char* detail = "none";
};
bool acpica_prepare_shutdown(PowerDiagnostic& diagnostic);
bool acpica_enter_shutdown(PowerDiagnostic& diagnostic);
bool acpica_reset_system(PowerDiagnostic& diagnostic);
