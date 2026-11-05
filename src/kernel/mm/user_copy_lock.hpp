#pragma once
#include "sync/smp.hpp"
// The gate covers translation and direct-map access. Retirement holds the same
// gate until every old hardware translation has been acknowledged.
void user_copy_lock();
void user_copy_unlock();
class UserCopyGuard
{
public:
    UserCopyGuard()
    {
        user_copy_lock();
    }
    ~UserCopyGuard()
    {
        user_copy_unlock();
    }
    UserCopyGuard(const UserCopyGuard&) = delete;
    UserCopyGuard& operator=(const UserCopyGuard&) = delete;

private:
    IrqGuard irq_;
};
