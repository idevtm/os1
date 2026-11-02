#include "arch/x86_64/interrupt/interrupt.hpp"

#include "arch/x86_64/apic/lapic.hpp"
#include "arch/x86_64/cpu/cpu.hpp"
#include "arch/x86_64/cpu/io_port.hpp"
#include "arch/x86_64/interrupt/vector_allocator.hpp"
#include "util/memory.h"

extern "C"
{
    int int_00h();
    int int_01h();
    int int_02h();
    int int_03h();
    int int_04h();
    int int_05h();
    int int_06h();
    int int_07h();
    int int_08h();
    int int_09h();
    int int_0Ah();
    int int_0Bh();
    int int_0Ch();
    int int_0Dh();
    int int_0Eh();
    int int_10h();
    int int_11h();
    int int_12h();
    int int_13h();
    int int_1Dh();
    int int_1Eh();

    extern uint64_t interrupt_vector_stub_table[256];
}

namespace
{
InterruptHandler irq_handler_function[256] = {};
void* irq_handler_data[256] = {};
ExceptionHandler exception_handler_function[256] = {};

static inline void lidt(void* base, uint16_t size)
{
    struct
    {
        uint16_t length;
        void* base;
    } __attribute__((packed)) idtr = {size, base};

    asm volatile("lidt %0" : : "m"(idtr));
}

void lapic_error_vector_handler(void*)
{
    lapic_err_intr();
}
}  // namespace

void dispatch_interrupt_vector(uint8_t vector)
{
    if(irq_handler_function[vector])
    {
        irq_handler_function[vector](irq_handler_data[vector]);
    }
}

bool interrupt_vector_has_handler(uint8_t vector)
{
    return nullptr != irq_handler_function[vector];
}

void dispatch_irq_hook(int number)
{
    if((number >= 0) && (number < 16))
    {
        dispatch_interrupt_vector(static_cast<uint8_t>(T_IRQ0 + number));
    }
}

void dispatch_exception_handler(int number, TrapFrame* frame)
{
    if((number >= 0) && (number < 256) && exception_handler_function[number])
    {
        exception_handler_function[number](frame);
    }
}

void Interrupts::set_idt(int index, uint64_t address, uint8_t type_attr)
{
    IDT[index].offset_1 = address & 0xffff;
    IDT[index].selector = CPU_GDT_KCODE;
    IDT[index].ist = 0;
    IDT[index].type_attr = type_attr;
    IDT[index].offset_2 = (address >> 16) & 0xffff;
    IDT[index].offset_3 = (address >> 32) & 0xffffffff;
    IDT[index].zero = 0;
}

void Interrupts::clear_idt(int index)
{
    IDT[index].offset_1 = 0;
    IDT[index].selector = 0;
    IDT[index].ist = 0;
    IDT[index].type_attr = 0;
    IDT[index].offset_2 = 0;
    IDT[index].offset_3 = 0;
    IDT[index].zero = 0;
}

bool Interrupts::initialize()
{
    asm volatile("cli");
    for(int i = 0; i < 256; i++)
    {
        clear_idt(i);
    }

    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 40);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    // Keep every 8259 line masked: external IRQs are routed through the IOAPIC,
    // and an unmasked PIC can deliver the same lines on its own vectors.
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    for(uint16_t vector = T_IRQ0; vector <= 0xFFu; ++vector)
    {
        if(vector == T_SYSCALL)
        {
            continue;
        }
        if(0 != interrupt_vector_stub_table[vector])
        {
            set_idt(static_cast<int>(vector), interrupt_vector_stub_table[vector]);
        }
    }

    set_idt(0x00, (uint64_t)int_00h);
    set_idt(0x01, (uint64_t)int_01h);
    set_idt(0x02, (uint64_t)int_02h);
    set_idt(0x03, (uint64_t)int_03h);
    set_idt(0x04, (uint64_t)int_04h);
    set_idt(0x05, (uint64_t)int_05h);
    set_idt(0x06, (uint64_t)int_06h);
    set_idt(0x07, (uint64_t)int_07h);
    set_idt(0x08, (uint64_t)int_08h);
    set_idt(0x09, (uint64_t)int_09h);
    set_idt(0x0A, (uint64_t)int_0Ah);
    set_idt(0x0B, (uint64_t)int_0Bh);
    set_idt(0x0C, (uint64_t)int_0Ch);
    set_idt(0x0D, (uint64_t)int_0Dh);
    set_idt(0x0E, (uint64_t)int_0Eh);
    set_idt(0x10, (uint64_t)int_10h);
    set_idt(0x11, (uint64_t)int_11h);
    set_idt(0x12, (uint64_t)int_12h);
    set_idt(0x13, (uint64_t)int_13h);
    set_idt(0x1D, (uint64_t)int_1Dh);
    set_idt(0x1E, (uint64_t)int_1Eh);
    // NMI, double fault, and machine check run on each CPU's IST1 stack so a
    // fault on a broken kernel stack still reaches a handler instead of
    // escalating to a triple fault (an instant, silent reset).
    IDT[0x02].ist = 1;
    IDT[0x08].ist = 1;
    IDT[0x12].ist = 1;
    for(int i = 0; i < 256; ++i)
    {
        irq_handler_function[i] = nullptr;
        irq_handler_data[i] = nullptr;
        exception_handler_function[i] = nullptr;
    }
    set_vector_handler(T_LERROR, lapic_error_vector_handler, nullptr);
    irq_vector_allocator_reset();

    load();
    return true;
}

void Interrupts::load()
{
    lidt(IDT, 256 * sizeof(IDTDescriptor));
}

void Interrupts::set_vector_handler(uint8_t vector, InterruptHandler pFunction, void* data)
{
    irq_handler_function[vector] = pFunction;
    irq_handler_data[vector] = data;
}

void Interrupts::set_irq_handler(int number, InterruptHandler pFunction, void* data)
{
    if((number >= 0) && (number < 16))
    {
        set_vector_handler(static_cast<uint8_t>(T_IRQ0 + number), pFunction, data);
    }
}

void Interrupts::set_exception_handler(int number, ExceptionHandler handler)
{
    if((number >= 0) && (number < 256))
    {
        exception_handler_function[number] = handler;
    }
}
