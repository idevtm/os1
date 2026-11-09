#include "platform/system_control.hpp"

#include <os1/system.h>
#include <os1/tmpfs.h>

#include "console/console.hpp"
#include "debug/log_export.hpp"
#include "freestanding/string.hpp"
#include "platform/acpica_diagnostics.hpp"
#include "platform/system_power.hpp"
#include "sync/atomic.hpp"
#include "sync/smp.hpp"
#include "vfs/ext2.hpp"
namespace
{
volatile bool transitioning;
void report_failure(unsigned member, const PowerDiagnostic& diagnostic)
{
    char line[192] = "system: ";
    const char* method = member == OS1_SYSTEM_SHUTDOWN ? "shutdown" : "reboot";
    freestanding::append_string(line, sizeof(line), method);
    freestanding::append_string(line, sizeof(line), " stage=");
    freestanding::append_string(line, sizeof(line), diagnostic.stage);
    freestanding::append_string(line, sizeof(line), " detail=");
    freestanding::append_string(line, sizeof(line), diagnostic.detail);
    freestanding::append_string(line, sizeof(line), "\n");
    write_console_bytes(line, freestanding::string_length(line));
}
}  // namespace
long system_control(unsigned member)
{
    if(member < OS1_SYSTEM_SYNC || member > OS1_SYSTEM_REBOOT)
        return OS1_ERR_UNSUPPORTED;
    const bool transition = member != OS1_SYSTEM_SYNC;
    bool expected = false;
    if(!atomic_compare_exchange_strong(&transitioning, &expected, true))
    {
        if(transition)
            report_failure(member, {"transition-admission", "busy"});
        return OS1_ERR_WOULD_BLOCK;
    }
    AcpicaDiagnosticCapture acpi_output(member == OS1_SYSTEM_SHUTDOWN);
    PowerDiagnostic diagnostic{"storage-sync", "failed"};
    if(transition)
        log_export::final_drain();
    long result = ext2_system_sync(transition);
    if(result == 0 && transition)
    {
        if(member == OS1_SYSTEM_SHUTDOWN && !acpica_prepare_shutdown(diagnostic))
            result = OS1_ERR_UNSUPPORTED;
        else
        {
            IrqGuard interrupts;
            const bool accepted = member == OS1_SYSTEM_SHUTDOWN ? acpica_enter_shutdown(diagnostic)
                                                                : acpica_reset_system(diagnostic);
            if(accepted)
                diagnostic = {"power-entry", "returned-without-transition"};
            // A successful platform transition does not return to this kernel.
            result = OS1_ERR_IO;
        }
        ext2_system_thaw();
    }
    if(transition && result < 0)
    {
        acpi_output.report();
        report_failure(member, diagnostic);
    }
    atomic_store_release(&transitioning, false);
    return result;
}
