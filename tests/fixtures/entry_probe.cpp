#include <os1/observe.h>

#include <os1/syscall.hpp>

#include "hardware/presentation.hpp"

namespace
{
using namespace os1::user;

struct Output
{
    void text(const char* message)
    {
        size_t bytes = 0;
        while(message[bytes])
            ++bytes;
        write(1, message, bytes);
    }
};

uint64_t timestamp()
{
    uint32_t low, high;
    asm volatile("lfence; rdtsc; lfence" : "=a"(low), "=d"(high) : : "memory");
    return (static_cast<uint64_t>(high) << 32) | low;
}

bool flat_segments()
{
    const uint64_t expected = 0x891a73be;
    uint64_t fs, gs;
    asm volatile("movq %%fs:(%2), %0; movq %%gs:(%2), %1"
                 : "=&r"(fs), "=&r"(gs)
                 : "r"(&expected), "m"(expected));
    return fs == expected && gs == expected;
}

uint64_t ticks()
{
    struct
    {
        Os1ObserveHeader header;
        Os1ObserveSystemRecord system;
    } snapshot{};
    return observe(OS1_OBSERVE_SYSTEM, &snapshot, sizeof(snapshot)) == sizeof(snapshot)
               ? snapshot.system.tick_count
               : 0;
}
}  // namespace

int main(int argc, char**)
{
    // An argument selects the common workload for baseline measurements.
    const bool check_segments = argc == 1;
    if(check_segments && !flat_segments())
        return 1;

    constexpr unsigned calls = 50000;
    const long pid = getpid();
    if(pid <= 0)
        return 2;

    const uint64_t begin = timestamp();
    for(unsigned i = 0; i < calls; ++i)
    {
        if(getpid() != pid)
            return 2;
    }
    const uint64_t syscall_cycles = timestamp() - begin;

    const uint64_t before = ticks();
    const uint64_t spin_start = timestamp();
    while(timestamp() - spin_start < 50000000)
        asm volatile("pause");
    const uint64_t after = ticks();
    if(before == 0 || after <= before)
        return 3;

    if(check_segments && !flat_segments())
        return 4;

    Output out;
    out.text("entry probe ok: getpid_tsc_cycles=");
    os1::hardware::presentation::number(out, syscall_cycles);
    out.text(" timer_ticks=");
    os1::hardware::presentation::number(out, after - before);
    out.text("\n");
    return 0;
}
