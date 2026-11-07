#pragma once
#include "storage/block_device.hpp"

// The IRQ holds this lock through its last request access. Observing done alone
// does not retire that callback: completion_signal still accesses its wait queue.
template<class Drain>
bool poll_block_completion(BlockRequest& request, Spinlock& lock, Drain drain)
{
    IrqSpinGuard guard(lock);
    drain();
    return completion_done(request.completion);
}

// Caller owns the queue lock. A completion at the timeout boundary wins over
// device reset. Drain once more because an interrupt can still be pending.
template<class Drain, class Expire>
bool finish_block_poll(BlockRequest& request, Drain drain, Expire expire)
{
    drain();
    if(completion_done(request.completion))
        return true;
    expire();
    return false;
}
