#include "arch/x86_64/cpu/cpu.hpp"
#include "platform/topology.hpp"

int ncpu = 1;

namespace
{
cpu g_host_cpu{
    .self = &g_host_cpu,
    .next = nullptr,
    .id = 0,
    .booted = 1,
    .magic = CPU_MAGIC,
};
}  // namespace

cpu* g_cpu_boot = &g_host_cpu;
cpu* g_cpu_host_current = &g_host_cpu;

void cpu_set_kernel_stack(uint64_t stack_top)
{
    if(nullptr != g_cpu_host_current)
    {
        g_cpu_host_current->tss.rsp0 = stack_top;
    }
}

extern "C" void kernel_thread_start() {}
