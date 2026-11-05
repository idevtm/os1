// SMP synchronization contract. The annotations below document remaining
// ownership constraints; they do not replace locks for shared runtime state.
#pragma once

#include <stdint.h>

// Marker for global state that is intentionally mutated only on the bootstrap
// CPU for now. This is documentation with compiler-visible placement; it is not
// a lock and must be replaced or removed when the state becomes SMP-safe.
#define OS1_BSP_ONLY [[maybe_unused]]

// Marker for globals whose mutation is serialized by `name`. This is
// documentation-only and intentionally easy to grep during SMP conversions.
#define OS1_LOCKED_BY(name) [[maybe_unused]]

#if defined(OS1_HOST_TEST)
#define KASSERT_ON_BSP() \
    do                   \
    {                    \
    } while(false)
#else
// Return true while executing on the bootstrap CPU. Before g_cpu_boot is
// published, early boot is still single-CPU and is treated as BSP context.
[[nodiscard]] bool kernel_on_bsp();

// Serial-log and halt after a BSP-only invariant violation.
[[noreturn]] void kassert_on_bsp_failed(const char* file, int line);

#ifndef NDEBUG
#define KASSERT_ON_BSP()                         \
    do                                           \
    {                                            \
        if(!kernel_on_bsp())                     \
        {                                        \
            kassert_on_bsp_failed(__FILE__, __LINE__); \
        }                                        \
    } while(false)
#else
#define KASSERT_ON_BSP() \
    do                   \
    {                    \
    } while(false)
#endif
#endif

// Lock waiters can run with IF=0. Service TLB requests without taking locks.
#if !defined(OS1_HOST_TEST)
void ipi_poll_tlb_shootdown();
#endif

class Spinlock
{
public:
    constexpr explicit Spinlock(const char* name) : name_(name) {}

    Spinlock(const Spinlock&) = delete;
    Spinlock& operator=(const Spinlock&) = delete;

    void lock()
    {
        while(__atomic_exchange_n(&locked_, 1u, __ATOMIC_ACQUIRE) != 0u)
        {
            while(__atomic_load_n(&locked_, __ATOMIC_RELAXED) != 0u)
            {
#if defined(OS1_HOST_TEST)
                __atomic_signal_fence(__ATOMIC_ACQ_REL);
#else
                ipi_poll_tlb_shootdown();
                asm volatile("pause" ::: "memory");
#endif
            }
        }
    }

    [[nodiscard]] bool try_lock()
    {
        return __atomic_exchange_n(&locked_, 1u, __ATOMIC_ACQUIRE) == 0u;
    }

    void unlock()
    {
        __atomic_store_n(&locked_, 0u, __ATOMIC_RELEASE);
    }

    void reset(const char* name)
    {
        __atomic_store_n(&locked_, 0u, __ATOMIC_RELAXED);
        name_ = name;
    }

    [[nodiscard]] bool locked() const
    {
        return __atomic_load_n(&locked_, __ATOMIC_RELAXED) != 0u;
    }

    [[nodiscard]] const char* name() const
    {
        return name_;
    }

private:
    volatile uint32_t locked_ = 0;
    const char* name_;
};

class IrqGuard
{
public:
    IrqGuard() : saved_rflags_(read_rflags()), restore_interrupts_((saved_rflags_ & kInterruptFlag) != 0)
    {
        disable_interrupts();
    }

    IrqGuard(const IrqGuard&) = delete;
    IrqGuard& operator=(const IrqGuard&) = delete;

    ~IrqGuard()
    {
        if(restore_interrupts_)
        {
            enable_interrupts();
        }
    }

    [[nodiscard]] bool interrupts_were_enabled() const
    {
        return restore_interrupts_;
    }

private:
    static constexpr uint64_t kInterruptFlag = 1ull << 9;

    [[nodiscard]] static uint64_t read_rflags()
    {
#if defined(OS1_HOST_TEST)
        return kInterruptFlag;
#else
        uint64_t value = 0;
        asm volatile("pushfq; popq %0" : "=r"(value) : : "memory");
        return value;
#endif
    }

    static void disable_interrupts()
    {
#if !defined(OS1_HOST_TEST)
        asm volatile("cli" ::: "memory");
#endif
    }

    static void enable_interrupts()
    {
#if !defined(OS1_HOST_TEST)
        asm volatile("sti" ::: "memory");
#endif
    }

    uint64_t saved_rflags_;
    bool restore_interrupts_;
};

template <typename Lock = Spinlock>
class IrqSpinGuard
{
public:
    explicit IrqSpinGuard(Lock& lock) : irq_guard_(), lock_(lock)
    {
        lock_.lock();
    }

    IrqSpinGuard(const IrqSpinGuard&) = delete;
    IrqSpinGuard& operator=(const IrqSpinGuard&) = delete;

    ~IrqSpinGuard()
    {
        if(locked_)
        {
            lock_.unlock();
        }
    }

    void unlock()
    {
        if(locked_)
        {
            lock_.unlock();
            locked_ = false;
        }
    }

private:
    IrqGuard irq_guard_;
    Lock& lock_;
    bool locked_ = true;
};
