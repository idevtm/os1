// Local APIC interrupt-command helpers for kernel IPIs.
#include "arch/x86_64/apic/ipi.hpp"

#include "arch/x86_64/apic/lapic.hpp"
#include "arch/x86_64/cpu/cpu.hpp"
#include "arch/x86_64/interrupt/interrupt.hpp"
#include "arch/x86_64/interrupt/vector_allocator.hpp"
#include "core/kernel_state.hpp"
#include "debug/event_ring.hpp"
#include "drivers/timer/tsc_clock.hpp"
#include "sync/smp.hpp"

#if defined(OS1_HOST_TEST)
void (*host_tlb_retirement_wait)(cpu*) = nullptr;
void (*host_ipi_destination_written)() = nullptr;
void (*host_power_park_wait)() = nullptr;
#endif
namespace
{
constexpr uint32_t kIpiAllButSelf = 0x000C0000u;
static_assert(kPowerParkVector > kDynamicIrqVectorLimit && kPowerParkVector < kLapicSpuriousVector);
uint64_t g_power_generation;
uint64_t g_power_request;

bool power_target(const cpu* target, const cpu* owner)
{
    return target != owner &&
           (target == g_cpu_boot || cpu_ap_startup_state(target) == ApStartupState::Online);
}

volatile uint64_t g_tlb_requested = 0;
volatile uint64_t g_tlb_acknowledged[256]{};
uint8_t g_reschedule_vector = 0;
bool g_reschedule_vector_initialized = false;
uint8_t g_tlb_shootdown_vector = 0;
bool g_tlb_shootdown_vector_initialized = false;

void wait_for_icr_delivery()
{
    while((lapic[ICRLO] & DELIVS) != 0u)
    {
#if defined(OS1_HOST_TEST)
        __atomic_signal_fence(__ATOMIC_ACQ_REL);
#else
        asm volatile("pause" ::: "memory");
#endif
    }
}

void lapic_icr_write(uint32_t high, uint32_t low)
{
    // The destination and command are one per-CPU transaction. A nested timer
    // wakeup must not replace ICRHI before this caller publishes ICRLO.
    IrqGuard irq;
    wait_for_icr_delivery();
    lapic[ICRHI] = high;
#if defined(OS1_HOST_TEST)
    if(host_ipi_destination_written)
        host_ipi_destination_written();
#endif
    lapic[ICRLO] = low;
    (void)lapic[ID];
    wait_for_icr_delivery();
}

[[nodiscard]] bool ipi_vector_valid(uint8_t vector)
{
    return interrupt_vector_is_external(vector);
}

void reload_current_address_space()
{
#if defined(OS1_HOST_TEST)
    return;
#else
    uint64_t cr3;
    // Read hardware state: scheduler publication can lag a CR3 switch.
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
#endif
}
}  // namespace

bool ipi_initialize()
{
    KASSERT_ON_BSP();
    if(g_reschedule_vector_initialized && g_tlb_shootdown_vector_initialized)
    {
        const bool reschedule_ready =
            irq_vector_is_allocated(g_reschedule_vector) || irq_reserve_vector(g_reschedule_vector);
        const bool tlb_ready = irq_vector_is_allocated(g_tlb_shootdown_vector) ||
                               irq_reserve_vector(g_tlb_shootdown_vector);
        return reschedule_ready && tlb_ready;
    }

    uint8_t reschedule_vector = 0;
    if(!irq_allocate_vector(reschedule_vector))
    {
        return false;
    }

    uint8_t tlb_shootdown_vector = 0;
    if(!irq_allocate_vector(tlb_shootdown_vector))
    {
        return false;
    }

    g_reschedule_vector = reschedule_vector;
    g_reschedule_vector_initialized = true;
    g_tlb_shootdown_vector = tlb_shootdown_vector;
    g_tlb_shootdown_vector_initialized = true;
    return true;
}

bool ipi_send(uint8_t apic_id, uint8_t vector)
{
    if((nullptr == lapic) || !ipi_vector_valid(vector))
    {
        return false;
    }

    lapic_icr_write(static_cast<uint32_t>(apic_id) << 24, vector);
    return true;
}

bool ipi_send_all_but_self(uint8_t vector)
{
    if((nullptr == lapic) || !ipi_vector_valid(vector))
    {
        return false;
    }

    lapic_icr_write(0, kIpiAllButSelf | vector);
    return true;
}

bool ipi_send_reschedule(const cpu* target)
{
    if((nullptr == target) || !g_reschedule_vector_initialized)
    {
        return false;
    }

    kernel_event::record(OS1_KERNEL_EVENT_IPI_RESCHED,
                         OS1_KERNEL_EVENT_FLAG_BEGIN,
                         cpu_cur()->id,
                         target->id,
                         g_reschedule_vector,
                         0);
    const bool sent = ipi_send(target->id, g_reschedule_vector);
#if defined(OS1_HOST_TEST)
    // Hardware-boundary seam: host tests may model an AP consuming publication.
    extern void host_reschedule_delivered(const cpu*);
    if(sent)
        host_reschedule_delivered(target);
#endif
    if(!sent)
    {
        kernel_event::record(OS1_KERNEL_EVENT_IPI_RESCHED,
                             OS1_KERNEL_EVENT_FLAG_FAILURE,
                             cpu_cur()->id,
                             target->id,
                             g_reschedule_vector,
                             0);
    }
    return sent;
}

bool ipi_send_tlb_shootdown()
{
    if(!g_tlb_shootdown_vector_initialized)
    {
        return false;
    }

    reload_current_address_space();
    return ipi_send_all_but_self(g_tlb_shootdown_vector);
}

uint8_t ipi_reschedule_vector()
{
    return g_reschedule_vector_initialized ? g_reschedule_vector : 0;
}

uint8_t ipi_tlb_shootdown_vector()
{
    return g_tlb_shootdown_vector_initialized ? g_tlb_shootdown_vector : 0;
}

bool ipi_is_reschedule_vector(uint8_t vector)
{
    return g_reschedule_vector_initialized && (g_reschedule_vector == vector);
}

bool ipi_is_tlb_shootdown_vector(uint8_t vector)
{
    return g_tlb_shootdown_vector_initialized && (g_tlb_shootdown_vector == vector);
}

void ipi_poll_tlb_shootdown()
{
    if(!g_tlb_shootdown_vector_initialized)
        return;
    cpu* current = cpu_cur();
    if(!current)
        return;
    const auto request = __atomic_load_n(&g_tlb_requested, __ATOMIC_ACQUIRE);
    if(__atomic_load_n(&g_tlb_acknowledged[current->id], __ATOMIC_RELAXED) >= request)
        return;
    reload_current_address_space();
    __atomic_store_n(&g_tlb_acknowledged[current->id], request, __ATOMIC_RELEASE);
}
void ipi_handle_tlb_shootdown()
{
    // Asynchronous callers also require a flush when no retirement is pending.
    reload_current_address_space();
    ipi_poll_tlb_shootdown();
}
void ipi_retire_tlb()
{
    IrqGuard irq;
    if(!g_tlb_shootdown_vector_initialized)
    {
        reload_current_address_space();
        return;
    }
    const auto request = __atomic_add_fetch(&g_tlb_requested, 1ull, __ATOMIC_ACQ_REL);
    (void)ipi_send_all_but_self(g_tlb_shootdown_vector);
    for(cpu* target = g_cpu_boot; target; target = target->next)
    {
        if(ApStartupState::Online != cpu_ap_startup_state(target) && target != cpu_cur())
            continue;
        while(__atomic_load_n(&g_tlb_acknowledged[target->id], __ATOMIC_ACQUIRE) < request)
        {
#if defined(OS1_HOST_TEST)
            if(host_tlb_retirement_wait)
                host_tlb_retirement_wait(target);
#endif
            ipi_poll_tlb_shootdown();
#if !defined(OS1_HOST_TEST)
            asm volatile("pause" ::: "memory");
#endif
        }
    }
}

bool ipi_quiesce_for_power()
{
    auto* owner = cpu_cur();
    if(!owner)
        return true;
    bool has_target = false;
    for(cpu* target = g_cpu_boot; target; target = target->next)
        has_target |= power_target(target, owner);
    if(!has_target)
        return true;
    // This IPI cannot use the ordinary unbounded delivery-status wait.
    if(!lapic || (lapic[ICRLO] & DELIVS))
        return false;
    const uint64_t request = (++g_power_generation << 8) | owner->id;
    __atomic_store_n(&g_power_request, request, __ATOMIC_RELEASE);
    lapic[ICRHI] = 0;
    lapic[ICRLO] = kIpiAllButSelf | kPowerParkVector;
    (void)lapic[ID];
    for(unsigned attempt = 0; attempt < 1000; ++attempt)
    {
        bool all_parked = true;
        for(cpu* target = g_cpu_boot; target; target = target->next)
            if(power_target(target, owner) &&
               __atomic_load_n(&target->power_park_ack, __ATOMIC_ACQUIRE) != request)
                all_parked = false;
        if(all_parked)
            return true;
#if defined(OS1_HOST_TEST)
        if(host_power_park_wait)
            host_power_park_wait();
#else
        tsc_clock_delay_us(100);
#endif
    }
    ipi_resume_after_power();
    return false;
}

void ipi_resume_after_power()
{
    __atomic_store_n(&g_power_request, 0, __ATOMIC_RELEASE);
}

void ipi_handle_power_park()
{
    const uint64_t request = __atomic_load_n(&g_power_request, __ATOMIC_ACQUIRE);
    auto* target = cpu_cur();
    if(!request || !target || target->id == static_cast<uint8_t>(request))
        return;
    __atomic_store_n(&target->power_park_ack, request, __ATOMIC_RELEASE);
#if !defined(OS1_HOST_TEST)
    while(__atomic_load_n(&g_power_request, __ATOMIC_ACQUIRE) == request)
    {
        ipi_poll_tlb_shootdown();
        asm volatile("pause" ::: "memory");
    }
#endif
}
