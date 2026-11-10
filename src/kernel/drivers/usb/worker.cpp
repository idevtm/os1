#include "drivers/usb/worker.hpp"

#include "console/console_input.hpp"
#include "drivers/usb/ehci.hpp"
#include "drivers/usb/xhci.hpp"
#include "ipc/channel.hpp"
namespace
{
PageFrameContainer* allocator;
void worker()
{
    unsigned ticks = 0;
    for(;;)
    {
        ehci_poll_devices(*allocator, ticks % 50 == 0);
        if(ticks % 50 == 0)
            xhci_poll_devices(*allocator);
        ipc_refresh_input_waiters();
        ++ticks;
        kernel_sleep_for(current_thread(), 5'000'000);
    }
}
}  // namespace
bool usb_start_worker(Process* process, PageFrameContainer& frames)
{
    allocator = &frames;
    return create_kernel_thread(process, worker, frames) != nullptr;
}

namespace
{
Spinlock console_lock{"usb-console"};
char console_bytes[64]{};
unsigned console_head{}, console_count{};
}  // namespace
void usb_queue_console_char(char byte)
{
    IrqSpinGuard guard(console_lock);
    if(console_count < sizeof(console_bytes))
        console_bytes[(console_head + console_count++) % sizeof(console_bytes)] = byte;
}
void usb_poll_console()
{
    char bytes[64];
    unsigned count = 0;
    {
        IrqSpinGuard guard(console_lock);
        while(console_count)
        {
            bytes[count++] = console_bytes[console_head];
            console_head = (console_head + 1) % sizeof(console_bytes);
            --console_count;
        }
    }
    for(unsigned i = 0; i < count; ++i)
        console_input_on_keyboard_char(bytes[i]);
}
