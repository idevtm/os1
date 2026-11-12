#pragma once
#include "sync/smp.hpp"

struct Process;

// A user copy holds the address-space gate through physical access. A Buffer
// alias change holds the same gate through retirement. Same-gate copies can nest.
class UserCopyGuard
{
public:
    explicit UserCopyGuard(Process* process);
    ~UserCopyGuard();
    UserCopyGuard(const UserCopyGuard&) = delete;
    UserCopyGuard& operator=(const UserCopyGuard&) = delete;

private:
    IrqGuard irq_;
    Spinlock* gate_;
    Spinlock* previous_;
};
