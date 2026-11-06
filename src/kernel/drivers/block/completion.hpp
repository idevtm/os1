#pragma once
#include "storage/block_device.hpp"

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
