#include "platform/system_control.hpp"

#include <os1/system.h>
#include <os1/tmpfs.h>

#include "console/console.hpp"
#include "debug/log.hpp"
#include "debug/log_export.hpp"
#include "drivers/usb/stall_trace.hpp"
#include "freestanding/string.hpp"
#include "ipc/channel.hpp"
#include "platform/acpica_diagnostics.hpp"
#include "platform/system_power.hpp"
#include "proc/thread.hpp"
#include "sync/atomic.hpp"
#include "sync/smp.hpp"
#include "vfs/ext2.hpp"
#ifdef OS1_HOST_TEST
void (*system_transition_wait_hook)();
#endif

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
long sync_for_transition(PowerDiagnostic& diagnostic)
{
    constexpr uint64_t retry_ns = 1'000'000;
    constexpr uint64_t timeout_ns = 250'000'000;
    const uint64_t start = diagnostic_log::monotonic();
    log_export::FinalDrainGuard drain;
    bool acquired = drain.try_acquire();
    for(unsigned waits = 0; !acquired && waits < timeout_ns / retry_ns; ++waits)
    {
#ifdef OS1_HOST_TEST
        if(system_transition_wait_hook)
            system_transition_wait_hook();
#else
        kernel_sleep_for(current_thread(), retry_ns);
#endif
        if(diagnostic_log::monotonic() - start > timeout_ns)
            break;
        acquired = drain.try_acquire();
    }
    if(!acquired)
    {
        diagnostic = {"log-drain-admission", "busy"};
        return OS1_ERR_WOULD_BLOCK;
    }
    drain.drain();
    return ext2_system_sync(true);
}
}  // namespace
long system_control(unsigned member, unsigned argument)
{
    if(member == OS1_SYSTEM_EHCI_TRACE_START)
        return usb::stall_trace::start(argument);
    if(member == OS1_SYSTEM_EHCI_TRACE_STOP)
        return usb::stall_trace::stop(argument);
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
    long result = transition ? sync_for_transition(diagnostic) : ext2_system_sync(false);
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
