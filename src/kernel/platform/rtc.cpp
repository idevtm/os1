#include "arch/x86_64/cpu/io_port.hpp"
#include "core/wall_clock.hpp"
#include "platform/state.hpp"
#include "sync/smp.hpp"

namespace
{
uint8_t read_cmos(uint8_t index)
{
    // The kernel enables NMI after AP startup. Never write RTC data registers.
    outb(0x70, index);
    return inb(0x71);
}
bool snapshot(RtcCalendar& value, uint8_t century)
{
    IrqGuard guard;
    if(read_cmos(0x0a) & 0x80)
        return false;
    value = {read_cmos(0),
             read_cmos(2),
             read_cmos(4),
             read_cmos(7),
             read_cmos(8),
             read_cmos(9),
             century ? read_cmos(century) : uint8_t(0),
             read_cmos(0x0b),
             read_cmos(0x0d)};
    return !(read_cmos(0x0a) & 0x80);
}
bool equal(const RtcCalendar& a, const RtcCalendar& b)
{
    return a.second == b.second && a.minute == b.minute && a.hour == b.hour && a.day == b.day &&
           a.month == b.month && a.year == b.year && a.century == b.century && a.mode == b.mode &&
           a.status == b.status;
}
}  // namespace
void rtc_initialize_wall_clock()
{
    wall_clock_initialize(0);
    const auto& fixed = g_platform.acpi_fixed;
    if(fixed.present && (fixed.boot_architecture_flags & (1u << 5)))
        return;
    const uint8_t century = fixed.present ? fixed.century : 0;
    if(century && (century < 0x0e || century > 0x7f))
        return;
    // Bounded retries also cover the RTC update interval without an unbounded boot wait.
    for(unsigned attempt = 0; attempt < 4096; ++attempt)
    {
        RtcCalendar first{}, second{};
        uint64_t now = 0;
        if(snapshot(first, century) && snapshot(second, century) && equal(first, second) &&
           rtc_decode(first, century != 0, now))
        {
            wall_clock_initialize(now);
            return;
        }
    }
}
