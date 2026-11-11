/*
 * x86 processor-specific definitions and utility functions.
 *
 * Copyright (C) 1997 Massachusetts Institute of Technology
 * See section "MIT License" in the file LICENSES for licensing terms.
 *
 * Derived from the MIT Exokernel and JOS.
 * Adapted for PIOS by Bryan Ford at Yale University.
 */

#pragma once

#include "arch/x86_64/cpu/io_port.hpp"
#include "stddef.h"
#include "stdint.h"

// EFLAGS register
#define FL_CF 0x00000001         // Carry Flag
#define FL_PF 0x00000004         // Parity Flag
#define FL_AF 0x00000010         // Auxiliary carry Flag
#define FL_ZF 0x00000040         // Zero Flag
#define FL_SF 0x00000080         // Sign Flag
#define FL_TF 0x00000100         // Trap Flag
#define FL_IF 0x00000200         // Interrupt Flag
#define FL_DF 0x00000400         // Direction Flag
#define FL_OF 0x00000800         // Overflow Flag
#define FL_IOPL_MASK 0x00003000  // I/O Privilege Level bitmask
#define FL_IOPL_0 0x00000000     //   IOPL == 0
#define FL_IOPL_1 0x00001000     //   IOPL == 1
#define FL_IOPL_2 0x00002000     //   IOPL == 2
#define FL_IOPL_3 0x00003000     //   IOPL == 3
#define FL_NT 0x00004000         // Nested Task
#define FL_RF 0x00010000         // Resume Flag
#define FL_VM 0x00020000         // Virtual 8086 mode
#define FL_AC 0x00040000         // Alignment Check
#define FL_VIF 0x00080000        // Virtual Interrupt Flag
#define FL_VIP 0x00100000        // Virtual Interrupt Pending
#define FL_ID 0x00200000         // ID flag

// Struct containing information returned by the CPUID instruction
typedef struct cpuinfo
{
    uint32_t eax;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
} cpuinfo;

static inline void breakpoint(void)
{
    __asm __volatile("int3");
}

static inline void invlpg(void* addr)
{
    __asm __volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

static inline void lidt(void* p)
{
    __asm __volatile("lidt (%0)" : : "r"(p));
}

static inline void lldt(uint16_t sel)
{
    __asm __volatile("lldt %0" : : "r"(sel));
}

static inline void ltr(uint16_t sel)
{
    __asm __volatile("ltr %0" : : "r"(sel));
}

static inline void lcr0(uint32_t val)
{
    __asm __volatile("movl %0,%%cr0" : : "r"(val));
}

static inline uint32_t rcr0(void)
{
    uint32_t val;
    __asm __volatile("movl %%cr0,%0" : "=r"(val));
    return val;
}

static inline uint32_t rcr2(void)
{
    uint32_t val;
    __asm __volatile("movl %%cr2,%0" : "=r"(val));
    return val;
}

static inline void lcr4(uint32_t val)
{
    __asm __volatile("movl %0,%%cr4" : : "r"(val));
}

static inline uint32_t rcr4(void)
{
    uint32_t cr4;
    __asm __volatile("movl %%cr4,%0" : "=r"(cr4));
    return cr4;
}

static inline uint32_t read_eflags(void)
{
    uint32_t eflags;
    __asm __volatile("pushfl; popl %0" : "=rm"(eflags));
    return eflags;
}

static inline void write_eflags(uint32_t eflags)
{
    __asm __volatile("pushl %0; popfl" : : "rm"(eflags));
}

static inline uint32_t read_ebp(void)
{
    uint32_t ebp;
    __asm __volatile("movl %%ebp,%0" : "=rm"(ebp));
    return ebp;
}

static inline uint32_t read_esp(void)
{
    uint32_t esp;
    __asm __volatile("movl %%esp,%0" : "=rm"(esp));
    return esp;
}

static inline uint16_t read_cs(void)
{
    uint16_t cs;
    __asm __volatile("movw %%cs,%0" : "=rm"(cs));
    return cs;
}

// Atomically set *addr to newval and return the old value of *addr.
static inline uint32_t xchg(volatile uint32_t* addr, uint32_t newval)
{
    uint32_t result;

    // The + in "+m" denotes a read-modify-write operand.
    asm volatile("lock; xchgl %0, %1" : "+m"(*addr), "=a"(result) : "1"(newval) : "cc");
    return result;
}

// Atomically add incr to *addr.
static inline void lock_add(volatile int32_t* addr, int32_t incr)
{
    asm volatile("lock; addl %1,%0" : "+m"(*addr) : "r"(incr) : "cc");
}

// Atomically add incr to *addr and return true if the result is zero.
static inline uint8_t lock_add_zero(volatile int32_t* addr, int32_t incr)
{
    uint8_t zero;
    asm volatile("lock; addl %2,%0; setzb %1" : "+m"(*addr), "=rm"(zero) : "r"(incr) : "cc");
    return zero;
}

// Atomically add incr to *addr and return the old value of *addr.
static inline int32_t xadd(volatile uint32_t* addr, int32_t incr)
{
    int32_t result;

    // The + in "+m" denotes a read-modify-write operand.
    asm volatile("lock; xaddl %0, %1" : "+m"(*addr), "=a"(result) : "1"(incr) : "cc");
    return result;
}

static inline void pause(void)
{
    asm volatile("pause" : : : "memory");
}

static inline void die(void)
{
    do
    {
        asm volatile("cli");
        asm volatile("hlt");
    } while(1);
}

static inline void cpuid(uint32_t idx, cpuinfo* info)
{
    asm volatile("cpuid"
                 : "=a"(info->eax), "=b"(info->ebx), "=c"(info->ecx), "=d"(info->edx)
                 : "a"(idx));
}

static inline uint64_t rdtsc(void)
{
    uint32_t low;
    uint32_t high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return (static_cast<uint64_t>(high) << 32) | low;
}

inline constexpr uint32_t kIa32FsBase = 0xC0000100;
inline constexpr uint32_t kIa32GsBase = 0xC0000101;
inline constexpr uint32_t kIa32KernelGsBase = 0xC0000102;

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = value & 0xffffffffu;
    uint32_t hi = value >> 32;
    asm volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// Enable external device interrupts.
static inline void sti(void)
{
    asm volatile("sti");
}

// Disable external device interrupts.
static inline void cli(void)
{
    asm volatile("cli");
}
