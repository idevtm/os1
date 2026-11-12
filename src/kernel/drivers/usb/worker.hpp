#pragma once

#include <stdint.h>

namespace usb
{
struct ReconnectDelay
{
    uint64_t deadline_100ns = 0;

    void changed(bool connected, uint64_t now)
    {
        deadline_100ns = connected ? now + 1'000'000 : 0;
    }

    bool take_ready(uint64_t now)
    {
        if(!deadline_100ns || now < deadline_100ns)
            return false;
        deadline_100ns = 0;
        return true;
    }
};
}  // namespace usb

struct Process;
class PageFrameContainer;
bool usb_start_worker(Process*, PageFrameContainer&);

void usb_queue_console_char(char);
void usb_poll_console();
