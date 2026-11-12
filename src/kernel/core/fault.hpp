// Fault and exception handling. User faults terminate the current user thread;
// kernel faults are reported through serial/terminal output and halt the CPU.
#pragma once

#include <stdint.h>

#include "arch/x86_64/interrupt/interrupt.hpp"

struct Thread;

enum class ExceptionOwner
{
    UserProcess,
    Kernel,
    Platform,
};

[[nodiscard]] inline ExceptionOwner exception_owner(const TrapFrame& frame)
{
    if(frame.vector == T_NMI || frame.vector == T_MCHK)
        return ExceptionOwner::Platform;
    return trap_frame_is_user(frame) ? ExceptionOwner::UserProcess : ExceptionOwner::Kernel;
}

// Return a human-readable architectural name for an exception vector.
const char* kernel_fault_name(uint64_t vector);

// Dump a trap frame to the serial debug channel.
void dump_trap_frame(const TrapFrame& frame);

// Exception callback registered for kernel-mode exception vectors.
void on_kernel_exception(TrapFrame* frame);

// Main exception path called by trap dispatch.
Thread* handle_exception(TrapFrame* frame);
