#include "drivers/usb/stall_trace.hpp"

#include <os1/input.h>
#include <os1/tmpfs.h>

#include "debug/log.hpp"
#include "platform/platform.hpp"
#include "sync/smp.hpp"

namespace usb::stall_trace
{
namespace
{
Spinlock lock{"ehci-stall-trace"};
Os1ObserveEhciStallTrace trace{};
uint64_t worker_last, poll_last, report_last, publish_last, last_time, start_tick;
unsigned last_ticket;
bool active;
unsigned clock_source;

bool read_time(unsigned source, uint64_t& time)
{
    uint64_t resolution = 0;
    if(source == OS1_EHCI_TRACE_CLOCK_HPET)
        return platform_measurement_sample(time, resolution);
    time = diagnostic_log::monotonic();
    return true;
}

void finish(unsigned flags)
{
    trace.flags |= flags;
    trace.end_ns = last_time;
    __atomic_store_n(&active, false, __ATOMIC_RELEASE);
}

bool refresh()
{
    if(!enabled())
        return false;
    if(diagnostic_log::monotonic() - start_tick >= 120'000'000'000ull)
    {
        finish(OS1_EHCI_TRACE_EXPIRED);
        return false;
    }
    uint64_t time = 0;
    if(!read_time(trace.clock, time) || time < last_time)
    {
        finish(OS1_EHCI_TRACE_CLOCK_LOST);
        return false;
    }
    last_time = time;
    return true;
}

void record(
    unsigned kind, unsigned pci, unsigned input, uint64_t time, uint64_t began, unsigned token)
{
    ++trace.samples[kind];
    const uint64_t elapsed = began >= trace.start_ns && began && time >= began ? time - began : 0;
    if(elapsed > trace.max_ns[kind])
        trace.max_ns[kind] = elapsed;
    if(elapsed < 50'000'000 && kind != OS1_EHCI_TRACE_ERROR && kind != OS1_EHCI_TRACE_RETRY)
        return;
    if(trace.count == OS1_EHCI_TRACE_CAPACITY)
    {
        ++trace.dropped;
        return;
    }
    trace.events[trace.count++] = {time,
                                   elapsed,
                                   kind,
                                   pci,
                                   input,
                                   token,
                                   trace.samples[OS1_EHCI_TRACE_BUSY],
                                   trace.samples[OS1_EHCI_TRACE_ERROR],
                                   trace.samples[OS1_EHCI_TRACE_RETRY]};
}
}  // namespace

bool enabled()
{
    return __atomic_load_n(&active, __ATOMIC_ACQUIRE);
}

uint64_t now()
{
    uint64_t time = 0;
    return read_time(__atomic_load_n(&clock_source, __ATOMIC_RELAXED), time) ? time : 0;
}

long start(unsigned input)
{
    if(!input || input > OS1_INPUT_DEVICE_LIMIT)
        return OS1_ERR_INVALID;
    IrqSpinGuard guard(lock);
    if(refresh())
        return OS1_ERR_WOULD_BLOCK;
    if(last_ticket == INT32_MAX)
        return OS1_ERR_LIMIT;
    trace = {};
    worker_last = poll_last = report_last = publish_last = 0;
    uint64_t time = 0;
    start_tick = diagnostic_log::monotonic();
    trace.clock = read_time(OS1_EHCI_TRACE_CLOCK_HPET, time) ? OS1_EHCI_TRACE_CLOCK_HPET
                                                             : OS1_EHCI_TRACE_CLOCK_TICK;
    trace.start_ns = last_time = trace.clock == OS1_EHCI_TRACE_CLOCK_HPET ? time : start_tick;
    trace.input = input;
    trace.ticket = ++last_ticket;
    __atomic_store_n(&clock_source, trace.clock, __ATOMIC_RELAXED);
    __atomic_store_n(&active, true, __ATOMIC_RELEASE);
    return trace.ticket;
}

long stop(unsigned ticket)
{
    IrqSpinGuard guard(lock);
    if(!ticket || trace.ticket != ticket)
        return OS1_ERR_NOT_FOUND;
    if(refresh())
        finish(0);
    return 0;
}

bool snapshot(Os1ObserveEhciStallTrace& out)
{
    IrqSpinGuard guard(lock);
    if(refresh() || !trace.ticket)
        return false;
    out = trace;
    return true;
}

void sample(unsigned kind, unsigned pci, unsigned input, uint64_t began, unsigned token)
{
    if(!enabled() || kind >= OS1_EHCI_TRACE_KIND_COUNT || input > OS1_INPUT_DEVICE_LIMIT)
        return;
    IrqSpinGuard guard(lock);
    if(!refresh() || (input && input != trace.input))
        return;
    uint64_t time = last_time;
    if(kind == OS1_EHCI_TRACE_REPORT && began >= trace.start_ns && began <= time)
        time = began;
    uint64_t* last = kind == OS1_EHCI_TRACE_WORKER            ? &worker_last
                     : input && kind == OS1_EHCI_TRACE_POLL   ? &poll_last
                     : input && kind == OS1_EHCI_TRACE_REPORT ? &report_last
                                                              : nullptr;
    if(last)
    {
        began = *last;
        *last = time;
    }
    record(kind, pci, input, time, began, token);
    if(input && kind == OS1_EHCI_TRACE_PUBLISH)
    {
        record(OS1_EHCI_TRACE_PUBLISH_GAP, pci, input, time, publish_last, 0);
        publish_last = time;
    }
}
}  // namespace usb::stall_trace
