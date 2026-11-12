#include "drivers/usb/ehci_controller.hpp"

#include "sync/smp.hpp"
namespace ehci
{
bool prepare_qtd(Qtd& td,
                 uint64_t address,
                 uint32_t bytes,
                 uint8_t pid,
                 bool toggle,
                 uint32_t next,
                 uint32_t alternate)
{
    td = {};
    if(bytes > 4096 || pid > 2 || address > 0xffffffffu ||
       (bytes && bytes - 1 > 0xffffffffu - address) || (next != terminate && (next & 31)) ||
       (alternate != terminate && (alternate & 31)))
        return false;
    td.next = next;
    td.alternate = alternate;
    td.token =
        (uint32_t(toggle) << 31) | (bytes << 16) | (3u << 10) | (uint32_t(pid) << 8) | active;
    td.buffer[0] = uint32_t(address);
    for(unsigned i = 1; i < 5; ++i)
        td.buffer[i] = uint32_t((address & ~uint64_t(4095)) + i * 4096);
    return true;
}
bool prepare_head(QueueHead& qh,
                  uint64_t self,
                  uint64_t first,
                  uint8_t address,
                  uint8_t endpoint,
                  uint16_t packet,
                  unsigned speed,
                  unsigned hub,
                  unsigned port,
                  bool periodic)
{
    qh = {};
    if(self > 0xffffffffu || first > 0xffffffffu || (self & 31) || (first & 31) || address > 127 ||
       endpoint > 15 || speed > 2 || !packet || packet > (speed == 2 ? 512u : 64u) ||
       (speed == 1 && packet > 8) || (speed != 2 && (!hub || hub > 127 || !port || port > 127)) ||
       (speed == 2 && !periodic && packet != 64 && packet != 512))
        return false;
    qh.next = uint32_t(self) | 2;  // Circular asynchronous QH list.
    qh.endpoint = address | (uint32_t(endpoint) << 8) | (speed << 12) | (1u << 14) |
                  (periodic ? 0 : (1u << 15) | (4u << 28)) | (uint32_t(packet) << 16);
    if(speed != 2 && !endpoint)
        qh.endpoint |= 1u << 27;
    qh.capabilities =
        (1u << 30) | (hub << 16) | (port << 23) |
        (periodic ? (speed == 2 ? 1u : 0x1c01u) : 0u);  // One transaction per microframe.
    qh.qtd_next = uint32_t(first);
    qh.alternate = terminate;
    return true;
}
uint32_t remaining(uint32_t token)
{
    return (token >> 16) & 0x7fff;
}
bool root_usable(uint32_t status)
{
    // Connection/enable changes also reject an unplug/replug between requests.
    return (status & 5u) == 5u && !(status & (0x3au | (1u << 7) | (1u << 8) | (1u << 13)));
}
bool set_schedule(volatile uint32_t& command,
                  volatile uint32_t& status,
                  Spinlock& command_lock,
                  const bool& failed,
                  Schedule schedule,
                  bool enabled,
                  void (*poll)(void*),
                  void* context,
                  unsigned limit,
                  uint64_t (*clock_100ns)())
{
    const unsigned bit = schedule == Schedule::Asynchronous ? 5 : 4;
    const uint32_t enable = 1u << bit, running = 1u << (bit + 10);
    auto wait = [&](bool expected) {
        const uint64_t start = clock_100ns ? clock_100ns() : 0;
        for(unsigned i = 0; i < limit; ++i)
        {
            uint32_t snapshot = status;
            if(__atomic_load_n(&failed, __ATOMIC_ACQUIRE) || (snapshot & (1u << 4)) ||
               (clock_100ns && clock_100ns() - start >= 10'000'000))
                return false;
            if(bool(snapshot & running) == expected)
                return true;
            poll(context);
        }
        return false;
    };
    bool previous;
    {
        IrqSpinGuard guard(command_lock);
        previous = command & enable;
    }
    if(!wait(previous))
        return false;
    {
        IrqSpinGuard guard(command_lock);
        if(__atomic_load_n(&failed, __ATOMIC_ACQUIRE))
            return false;
        auto value = command;
        command = enabled ? value | enable : value & ~enable;
    }
    return wait(enabled);
}
}  // namespace ehci

namespace ehci
{
bool isolate_root(volatile uint32_t& port, void (*poll)(void*), void* context, unsigned limit)
{
    port = port & ~(0x2au | 4u);
    for(unsigned i = 0; i < limit; ++i)
    {
        if(!(port & (4u | (1u << 8))))
            return true;
        poll(context);
    }
    return !(port & (4u | (1u << 8)));
}
}  // namespace ehci

namespace ehci
{
bool InputRecovery::schedule(uint32_t token, unsigned bytes, unsigned speed, uint64_t now)
{
    // Only an exhausted, untransferred interrupt IN split ERR is recoverable.
    // STALL, babble, buffer errors, active work, and malformed lengths retire normally.
    if(pending || speed > 1 || !bytes || bytes > 8 || (token & 0xff) != 0x41 ||
       ((token >> 8) & 3) != 1 || (token & (3u << 10)) || remaining(token) != bytes)
        return false;
    if(!attempts || now - window_ns >= 10'000'000'000ull)
    {
        window_ns = now;
        attempts = 0;
    }
    if(attempts == 3)
        return false;
    due_ns = now + (20'000'000ull << attempts++);
    pending = retried = true;
    return true;
}
}  // namespace ehci
