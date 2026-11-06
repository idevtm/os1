#include "drivers/usb/ehci_controller.hpp"
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
                  uint16_t packet)
{
    qh = {};
    if(self > 0xffffffffu || first > 0xffffffffu || (self & 31) || (first & 31) || address > 127 ||
       endpoint > 15 || (packet != 64 && packet != 512))
        return false;
    qh.next = uint32_t(self) | 2;  // Circular asynchronous QH list.
    qh.endpoint = address | (uint32_t(endpoint) << 8) | (2u << 12) | (1u << 14) | (1u << 15) |
                  (uint32_t(packet) << 16) | (4u << 28);
    qh.capabilities = 1u << 30;  // One transaction per microframe.
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
bool set_async(volatile uint32_t& command,
               volatile uint32_t& status,
               bool enabled,
               void (*poll)(void*),
               void* context,
               unsigned limit)
{
    constexpr uint32_t enable = 1u << 5, running = 1u << 15;
    auto wait = [&](bool expected) {
        for(unsigned i = 0; i < limit; ++i)
        {
            uint32_t snapshot = status;
            if(snapshot & (1u << 4))
                return false;
            if(bool(snapshot & running) == expected)
                return true;
            poll(context);
        }
        return false;
    };
    if(!wait(bool(command & enable)))
        return false;
    command = enabled ? command | enable : command & ~enable;
    return wait(enabled);
}
}  // namespace ehci
