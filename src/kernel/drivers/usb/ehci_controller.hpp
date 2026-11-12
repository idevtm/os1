#pragma once
#include <stddef.h>
#include <stdint.h>

class Spinlock;

namespace ehci
{
constexpr uint32_t terminate = 1;
constexpr uint32_t active = 1u << 7;
constexpr uint32_t halted = 1u << 6;
constexpr uint32_t errors = 0x7cu;
struct alignas(32) Qtd
{
    uint32_t next, alternate, token, buffer[5], upper[5];
};
struct alignas(32) QueueHead
{
    uint32_t next, endpoint, capabilities, current;
    uint32_t qtd_next, alternate, token, buffer[5], upper[5];
};
static_assert(sizeof(Qtd) == 64);
static_assert(sizeof(QueueHead) == 96);

bool prepare_qtd(Qtd&,
                 uint64_t buffer,
                 uint32_t bytes,
                 uint8_t pid,
                 bool toggle,
                 uint32_t next = terminate,
                 uint32_t alternate = terminate);
bool prepare_head(QueueHead&,
                  uint64_t self,
                  uint64_t first,
                  uint8_t address,
                  uint8_t endpoint,
                  uint16_t packet,
                  unsigned speed = 2,
                  unsigned hub = 0,
                  unsigned port = 0,
                  bool periodic = false);
uint32_t remaining(uint32_t token);
enum class RootAbortStep
{
    Wait,
    Disable,
    Complete
};
constexpr RootAbortStep root_abort_step(uint32_t status, bool disable_sent)
{
    if(status & (1u << 8))
        return RootAbortStep::Wait;
    if(!disable_sent)
        return RootAbortStep::Disable;
    return (status & 4u) ? RootAbortStep::Wait : RootAbortStep::Complete;
}
bool root_usable(uint32_t status);
enum class AsyncTransfer
{
    Control,
    Bulk
};
constexpr unsigned async_poll_delay_us = 10;
constexpr uint64_t async_deadline_100ns(AsyncTransfer transfer)
{
    return transfer == AsyncTransfer::Bulk ? 20'000'000 : 10'000'000;
}
// A failed stop quarantines controller DMA. A stopped transfer can retire one device.
template<typename Stop, typename Connected, typename RetireController, typename RetireDevice>
bool finish_async_transfer(AsyncTransfer transfer,
                           bool completed,
                           Stop stop,
                           Connected connected,
                           RetireController retire_controller,
                           RetireDevice retire_device)
{
    if(!stop())
        return false;  // The schedule stop path retires the controller.
    if(completed)
        return true;
    const bool root_connected = connected();
    // A connected control transfer can leave the device at an unknown address.
    if(transfer == AsyncTransfer::Control && root_connected)
        retire_controller();
    else
        retire_device(root_connected);
    return false;
}
enum class Schedule
{
    Asynchronous,
    Periodic
};

// Neither gate blocks. Each schedule retains its own descriptors until stopped.
class ScheduleLease
{
public:
    explicit ScheduleLease(uint8_t& gate)
        : gate_(gate), owned_(!__atomic_exchange_n(&gate_, 1, __ATOMIC_ACQUIRE))
    {
    }
    ~ScheduleLease()
    {
        if(owned_)
            __atomic_store_n(&gate_, 0, __ATOMIC_RELEASE);
    }
    ScheduleLease(const ScheduleLease&) = delete;
    ScheduleLease& operator=(const ScheduleLease&) = delete;
    explicit operator bool() const
    {
        return owned_;
    }

private:
    uint8_t& gate_;
    bool owned_;
};

// Change only the owned schedule bit, with no register lock held during waits.
bool set_schedule(volatile uint32_t& command,
                  volatile uint32_t& status,
                  Spinlock& command_lock,
                  const bool& failed,
                  Schedule schedule,
                  bool enabled,
                  void (*poll)(void*),
                  void* context,
                  unsigned limit,
                  uint64_t (*clock_100ns)() = nullptr);
}  // namespace ehci

namespace ehci
{
// Isolate a failed address-zero root child before another port can reset.
bool isolate_root(volatile uint32_t& port, void (*poll)(void*), void* context, unsigned limit);
// Include previously failed members: partial enumeration can publish siblings
// before the root is isolated, and a transfer can mark the parent failed first.
template<typename Range, typename Retire>
void retire_root_devices(Range& devices, unsigned root, Retire retire)
{
    for(auto& device : devices)
        if(device.root == root)
            retire(device);
}
}  // namespace ehci

namespace ehci
{
// Software recovery supplements CERR without resetting the endpoint data toggle.
struct InputRecovery
{
    uint64_t window_ns = 0, due_ns = 0;
    unsigned attempts = 0;
    bool pending = false, retried = false;
    bool schedule(uint32_t token, unsigned bytes, unsigned speed, uint64_t now);
};
}  // namespace ehci
