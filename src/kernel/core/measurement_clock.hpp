#pragma once
#include <stdint.h>

// Arithmetic and validity rules for one system-wide 64-bit HPET counter.
// The platform serializes calls. Neither this clock nor its epoch defines IPC deadlines.
class MeasurementClock
{
public:
    bool initialize(bool wide, uint32_t period_fs, uint64_t first, uint64_t last)
    {
        valid_ = wide && period_fs && period_fs <= 100000000u && last > first;
        period_ = period_fs;
        epoch_ = previous_ = last;
        return valid_;
    }
    bool sample(uint64_t counter, uint64_t& ns, uint64_t& resolution)
    {
        ns = resolution = 0;
        if(!valid_)
            return false;
        if(counter < previous_)
        {
            valid_ = false;
            return false;
        }
        const uint64_t delta = counter - epoch_;
        const uint64_t whole = delta / 1000000;
        const uint64_t fraction = (delta % 1000000) * period_ / 1000000;
        if(whole > (UINT64_MAX - fraction) / period_)
        {
            valid_ = false;
            return false;
        }
        previous_ = counter;
        ns = whole * period_ + fraction;
        resolution = (period_ + 999999ull) / 1000000;
        return true;
    }

private:
    uint64_t epoch_ = 0, previous_ = 0;
    uint32_t period_ = 0;
    bool valid_ = false;
};
