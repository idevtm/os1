#include "arch/x86_64/cpu/cpu.hpp"

#include "arch/x86_64/apic/ioapic.hpp"
#include "arch/x86_64/apic/lapic.hpp"
#include "arch/x86_64/cpu/exception_stack.hpp"
#include "arch/x86_64/cpu/syscall.hpp"
#include "arch/x86_64/cpu/x86.hpp"
#include "core/kernel_state.hpp"
#include "core/timer_source.hpp"
#include "debug/event_ring.hpp"
#include "drivers/timer/tsc_clock.hpp"
#include "handoff/memory_layout.h"
#include "mm/page_frame.hpp"
#include "util/memory.h"  // IWYU pragma: keep

namespace
{
constexpr size_t k_cpu_record_prefix_bytes = offsetof(cpu, kstacklo);
constexpr uint64_t kApBootTimeout100ns = 5000000;  // 500 ms.
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
    const bool calibrated = tsc_clock_calibrate();
    const uint64_t start = calibrated ? tsc_clock_now_100ns() : 0;
    for(uint64_t spins = 0; spins < kApBootWaitSpinLimit; ++spins)
    {
        if(ApStartupState::Online == cpu_ap_startup_state(c))
            return true;
        if(calibrated && tsc_clock_now_100ns() - start >= kApBootTimeout100ns)
            break;
        pause();
    }

    (void)cpu_ap_finish_startup(c, ApStartupState::TimedOut);
    return ApStartupState::Online == cpu_ap_startup_state(c);
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
    cpu* current = cpu_cur();
    Thread* idle = current->idle_thread;
    if(nullptr == idle)
        cpu_idle_loop();

    const bool timer_failed = timer_source_ap_timer_enabled() && !cpu_start_local_apic_timer();

    idle->state = ThreadState::Running;
    if(!cpu_ap_finish_startup(current, ApStartupState::Online))
    {
        idle->state = ThreadState::Blocked;
        lapic_timer_mask();
        cpu_idle_loop();
    }

    debug("CPU ")(current->id)(" is alive at 0x")(reinterpret_cast<uint64_t>(current), 16)();
    debug("cpu init worked!")();
    if(timer_failed)
        debug("cpu ")(current->id)(" lapic timer start failed")();

    if((nullptr != idle->process) && (ProcessState::Dying != idle->process->state))
        idle->process->state = ProcessState::Running;
    kernel_event::record(OS1_KERNEL_EVENT_AP_ONLINE,
                         OS1_KERNEL_EVENT_FLAG_SUCCESS,
                         current->id,
                         reinterpret_cast<uint64_t>(current),
                         reinterpret_cast<uint64_t>(idle),
                         0);
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

    // Each CPU needs the same page attribute table before a write-combining page is in use.
    wrmsr(kIa32Pat, kPatWithWriteCombining);
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
    cpu* current = cpu_cur();
    if(current != g_cpu_boot && ApStartupState::Pending == cpu_ap_startup_state(current))
    {
        cpu_init();
        if(lapic_x2apic_active())
        {
            debug("CPU ")(current->id)(": x2APIC mode active; xAPIC MMIO required")();
            (void)cpu_ap_finish_startup(current, ApStartupState::TimedOut);
            cpu_idle_loop();
        }
        if(nullptr != lapic)
        {
            assert(current->id == static_cast<uint8_t>(lapic[ID] >> 24));
        }
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
        return;

    extern uint8_t cpu_start_begin[];
    extern uint8_t cpu_start_end[];
    uint8_t* code = kernel_physical_pointer<uint8_t>(kApTrampolineAddress);
    memcpy(code, cpu_start_begin, (cpu_start_end - cpu_start_begin));

    auto* cpu_pages = kernel_physical_pointer<uint64_t>(kApStartupCpuPagesAddress);
    memset(cpu_pages, 0, kApStartupCpuPageCount * sizeof(*cpu_pages));
    *kernel_physical_pointer<uint64_t>(kApStartupRipAddress) = reinterpret_cast<uint64_t>(init);
    *kernel_physical_pointer<uint64_t>(kApStartupCr3Address) = cr3;
    clear_ap_startup_idt();
    for(cpu* c = g_cpu_boot; c; c = c->next)
    {
        if(c == cpu_cur())
        {
            continue;
        }

        // Prepare the exception stack here so a pending AP does not take the allocator lock.
        if(!cpu_prepare_exception_stack(*c, page_frames))
        {
            debug("cpu ")(c->id)(" exception stack allocation failed; startup skipped")();
            continue;
        }
        __atomic_store_n(&cpu_pages[c->id], reinterpret_cast<uint64_t>(c), __ATOMIC_RELEASE);

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
