#include "arch/x86_64/cpu/cpu.hpp"

#include "arch/x86_64/apic/ioapic.hpp"
#include "arch/x86_64/apic/lapic.hpp"
#include "arch/x86_64/cpu/exception_stack.hpp"
#include "arch/x86_64/cpu/syscall.hpp"
#include "arch/x86_64/cpu/x86.hpp"
#include "core/kernel_state.hpp"
#include "core/timer_source.hpp"
#include "debug/event_ring.hpp"
#include "handoff/memory_layout.h"
#include "mm/page_frame.hpp"
#include "util/memory.h"  // IWYU pragma: keep

namespace
{
constexpr size_t k_cpu_record_prefix_bytes = offsetof(cpu, kstacklo);
constexpr uint64_t kApBootWaitSpinLimit = 100000000;

[[noreturn]] void cpu_idle_loop()
{
    // Fallback only: Phase 2 APs should enter their pre-created idle thread.
    for(;;)
    {
        asm volatile("cli");
        asm volatile("hlt");
    }
}

bool wait_for_ap_boot(cpu* c)
{
    for(uint64_t spins = 0; spins < kApBootWaitSpinLimit; ++spins)
    {
        if(c->booted != 0)
        {
            return true;
        }

        asm volatile("pause");
    }

    return c->booted != 0;
}

void clear_ap_startup_idt()
{
    volatile uint8_t* const idt = kernel_physical_pointer<volatile uint8_t>(kApStartupIdtAddress);
    for(uint64_t index = 0; index < kApStartupIdtSizeBytes; ++index)
    {
        idt[index] = 0;
    }
}

[[noreturn]] void enter_ap_idle_thread()
{
    Thread* idle = cpu_cur()->idle_thread;
    if(nullptr == idle)
    {
        debug("cpu ")(cpu_cur()->id)(" missing idle thread")();
        cpu_idle_loop();
    }

    idle->state = ThreadState::Running;
    if((nullptr != idle->process) && (ProcessState::Dying != idle->process->state))
    {
        idle->process->state = ProcessState::Running;
    }
    kernel_event::record(OS1_KERNEL_EVENT_AP_ONLINE,
                         OS1_KERNEL_EVENT_FLAG_SUCCESS,
                         cpu_cur()->id,
                         reinterpret_cast<uint64_t>(cpu_cur()),
                         reinterpret_cast<uint64_t>(idle),
                         0);

    if(timer_source_ap_timer_enabled())
    {
        if(!cpu_start_local_apic_timer())
        {
            debug("cpu ")(cpu_cur()->id)(" lapic timer start failed")();
        }
    }

    xchg(&cpu_cur()->booted, 1);
    enter_first_thread(idle, idle->kernel_stack_top);
    cpu_idle_loop();
}

void set_tss_descriptor(cpu* c)
{
    const uint64_t base = (uint64_t)&c->tss;
    const uint64_t limit = sizeof(Tss64) - 1;

    const uint64_t low = (limit & 0xFFFFull) | ((base & 0xFFFFFFull) << 16) | (0x89ull << 40) |
                         (((limit >> 16) & 0xFull) << 48) | (((base >> 24) & 0xFFull) << 56);
    const uint64_t high = (base >> 32) & 0xFFFFFFFFull;

    c->gdt[CPU_GDT_TSS >> 3] = low;
    c->gdt[(CPU_GDT_TSS >> 3) + 1] = high;
}

void initialize_cpu_record_prefix(cpu* c)
{
    memset(c, 0, k_cpu_record_prefix_bytes);
    c->gdt[0] = SEGDESC_NULL;
    c->gdt[CPU_GDT_KCODE >> 3] = SEGDESC64_CODE(0);
    c->gdt[CPU_GDT_KDATA >> 3] = SEGDESC64_DATA(0);
    c->gdt[CPU_GDT_UDATA >> 3] = SEGDESC64_DATA(3);
    c->gdt[CPU_GDT_UCODE >> 3] = SEGDESC64_CODE(3);
    c->gdt[CPU_GDT_TSS >> 3] = 0;
    c->gdt[(CPU_GDT_TSS >> 3) + 1] = 0;
    c->runq.lock.reset("cpu-runq");
    c->runq.head = nullptr;
    c->runq.tail = nullptr;
    c->runq.length = 0;
    c->last_recorded_runq_depth = static_cast<size_t>(-1);
    c->magic = CPU_MAGIC;
}
}  // namespace

cpu* g_cpu_boot = nullptr;

void cpu_initialize_record(cpu* c)
{
    if(nullptr == c)
    {
        return;
    }

    initialize_cpu_record_prefix(c);
}

void cpu_set_kernel_stack(uint64_t stack_top)
{
    cpu_cur()->tss.rsp0 = stack_top;
}

extern PageFrameContainer page_frames;

void cpu_init()
{
    cpu* c = cpu_cur();
    c->self = c;
    c->tss.io_bitmap_base = sizeof(Tss64);
    if(0 == c->tss.rsp0)
    {
        c->tss.rsp0 = (uint64_t)c->kstackhi;
    }
    // Dedicated IST1 stack for NMI, double fault, and machine check. The BSP
    // gets it on its second cpu_init, once the page allocator and direct map
    // exist and before the IDT is loaded.
    if(g_kernel_direct_map_ready && !cpu_prepare_exception_stack(*c, page_frames))
    {
        debug("cpu ")(c->id)(" exception stack allocation failed; halted before IDT load")();
        cpu_idle_loop();
    }
    set_tss_descriptor(c);

    struct
    {
        uint16_t length;
        void* base;
    } __attribute__((packed)) gdtr = {sizeof(c->gdt) - 1, c->gdt};

    asm volatile("lgdt %0" : : "m"(gdtr));

    asm volatile("movw %%ax,%%gs" ::"a"(CPU_GDT_KDATA));
    asm volatile("movw %%ax,%%fs" ::"a"(CPU_GDT_KDATA));
    asm volatile("movw %%ax,%%es" ::"a"(CPU_GDT_KDATA));
    asm volatile("movw %%ax,%%ds" ::"a"(CPU_GDT_KDATA));
    asm volatile("movw %%ax,%%ss" ::"a"(CPU_GDT_KDATA));
    asm volatile(
        "pushq %[code]\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        :
        : [code] "i"(CPU_GDT_KCODE)
        : "rax", "memory");

    // User GS can only come from the flat GDT while FSGSBASE is disabled.
    constexpr uint64_t kCr4Fsgsbase = 1ull << 16;
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    if(cr4 & kCr4Fsgsbase)
    {
        cr4 &= ~kCr4Fsgsbase;
        asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    }

    wrmsr(kIa32FsBase, 0);
    wrmsr(kIa32GsBase, reinterpret_cast<uint64_t>(c));
    wrmsr(kIa32KernelGsBase, 0);

    asm volatile("lldt %%ax" ::"a"(0));
    ltr(CPU_GDT_TSS);
    cpu_enable_syscall_entry();
}

void cpu_load_idt()
{
    interrupts.load();
}

cpu* cpu_alloc(void)
{
    cpu* last_cpu = g_cpu_boot;
    while(last_cpu->next)
    {
        last_cpu = last_cpu->next;
    }

    uint64_t page = 0;
    if(!page_frames.allocate(page, 1))
    {
        debug("alloc cpu failed")();
        return nullptr;
    }

    cpu* c = kernel_physical_pointer<cpu>(page);
    memset(c, 0, sizeof(cpu));
    cpu_initialize_record(c);
    last_cpu->next = c;
    debug("alloc cpu at 0x")(page, 16)();
    return c;
}

void init()
{
    if(!cpu_on_boot())
    {
        debug("CPU ")(cpu_cur()->id)(" is alive at 0x")((uint64_t)cpu_cur(), 16)();
        cpu_init();
        if(nullptr != lapic)
        {
            cpu_cur()->id = static_cast<uint8_t>(lapic[ID] >> 24);
        }
        debug("cpu init worked!")();
        lapic_init();
        cpu_load_idt();
        enter_ap_idle_thread();
    }

    cpu_idle_loop();
}

void cpu_boot_others(uint64_t cr3)
{
    debug("booting other CPUs cr3 = 0x")(cr3, 16)();
    debug("current CPU = ")(cpu_cur()->id)();
    if(!cpu_on_boot())
    {
        xchg(&cpu_cur()->booted, 1);
        return;
    }

    extern uint8_t cpu_start_begin[];
    extern uint8_t cpu_start_end[];
    uint8_t* code = kernel_physical_pointer<uint8_t>(kApTrampolineAddress);
    memcpy(code, cpu_start_begin, (cpu_start_end - cpu_start_begin));

    for(cpu* c = g_cpu_boot; c; c = c->next)
    {
        if(c == cpu_cur())
        {
            continue;
        }

        *kernel_physical_pointer<uint64_t>(kApStartupCpuPageAddress) = (uint64_t)c;
        *kernel_physical_pointer<uint64_t>(kApStartupRipAddress) = (uint64_t)init;
        *kernel_physical_pointer<uint64_t>(kApStartupCr3Address) = cr3;
        clear_ap_startup_idt();

        debug("Starting CPU ")(c->id)();
        lapic_start_cpu(c->id, (uint64_t)code);
        if(!wait_for_ap_boot(c))
        {
            debug("cpu ")(c->id)(" failed to come up")();
            continue;
        }
        debug("done waiting")();
    }
}
