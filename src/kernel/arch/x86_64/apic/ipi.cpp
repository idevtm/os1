// Local APIC interrupt-command helpers for kernel IPIs.
#include "arch/x86_64/apic/ipi.hpp"

#include "arch/x86_64/apic/lapic.hpp"
#include "arch/x86_64/cpu/cpu.hpp"
#include "arch/x86_64/interrupt/interrupt.hpp"
#include "arch/x86_64/interrupt/vector_allocator.hpp"
#include "core/kernel_state.hpp"
#include "debug/event_ring.hpp"
#include "sync/smp.hpp"

namespace
{
constexpr uint32_t kIpiAllButSelf = 0x000C0000u;

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
    wait_for_icr_delivery();
    lapic[ICRHI] = high;
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
    uint64_t cr3 = g_kernel_root_cr3;
    cpu* current = cpu_cur();
    if((nullptr != current) && (nullptr != current->current_thread) &&
       (0 != current->current_thread->address_space_cr3))
    {
        cr3 = current->current_thread->address_space_cr3;
    }
    else if((nullptr != current) && (nullptr != current->idle_thread) &&
            (0 != current->idle_thread->address_space_cr3))
    {
        cr3 = current->idle_thread->address_space_cr3;
    }

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
    if(sent) host_reschedule_delivered(target);
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

void ipi_handle_tlb_shootdown()
{
    reload_current_address_space();
}
