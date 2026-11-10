#pragma once
#include <stdint.h>
namespace driver
{
enum class State : uint32_t
{
    discovered,
    starting,
    ready,
    quiescing,
    removed,
    failed
};
// Admission uses atomic transitions. No lock remains held across driver callbacks.
class Lifecycle
{
    uint32_t value_ = uint32_t(State::discovered);
    bool change(State from, State to)
    {
        auto expected = uint32_t(from);
        return __atomic_compare_exchange_n(
            &value_, &expected, uint32_t(to), false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }

public:
    State state() const
    {
        return State(__atomic_load_n(&value_, __ATOMIC_ACQUIRE));
    }
    bool start()
    {
        return change(State::discovered, State::starting);
    }
    bool started(bool ok)
    {
        return change(State::starting, ok ? State::ready : State::failed);
    }
    bool stop()
    {
        return change(State::ready, State::quiescing);
    }
    bool stopped(bool retired)
    {
        return change(State::quiescing, retired ? State::removed : State::failed);
    }
};
}  // namespace driver
