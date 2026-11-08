#pragma once

// Bounded capture for an explicit power request. Ordinary boot output stays quiet.
// One scope owns capture at a time. Nested scopes do not replace the owner.
class AcpicaDiagnosticCapture
{
public:
    explicit AcpicaDiagnosticCapture(bool enabled = true);
    ~AcpicaDiagnosticCapture();
    AcpicaDiagnosticCapture(const AcpicaDiagnosticCapture&) = delete;
    AcpicaDiagnosticCapture& operator=(const AcpicaDiagnosticCapture&) = delete;
    // Detach before console output. Call after the failed transition thaws storage.
    void report();

private:
    bool attached_ = false;
};
