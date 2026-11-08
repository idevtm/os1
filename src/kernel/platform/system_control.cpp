#include "platform/system_control.hpp"

#include <os1/system.h>
#include <os1/tmpfs.h>

#include "platform/system_power.hpp"
#include "sync/atomic.hpp"
#include "sync/smp.hpp"
#include "vfs/ext2.hpp"
namespace
{
volatile bool transitioning;
}
long system_control(unsigned member)
{
    if(member < OS1_SYSTEM_SYNC || member > OS1_SYSTEM_REBOOT)
        return OS1_ERR_UNSUPPORTED;
    bool expected = false;
    if(!atomic_compare_exchange_strong(&transitioning, &expected, true))
        return OS1_ERR_WOULD_BLOCK;
    const bool transition = member != OS1_SYSTEM_SYNC;
    long result = ext2_system_sync(transition);
    if(result == 0 && transition)
    {
        if(member == OS1_SYSTEM_SHUTDOWN && !acpica_prepare_shutdown())
            result = OS1_ERR_UNSUPPORTED;
        else
        {
            IrqGuard interrupts;
            if(member == OS1_SYSTEM_SHUTDOWN)
                acpica_enter_shutdown();
            else
                acpica_reset_system();
            // A successful platform transition does not return to this kernel.
            result = OS1_ERR_IO;
        }
        ext2_system_thaw();
    }
    atomic_store_release(&transitioning, false);
    return result;
}
