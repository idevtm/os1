#include "debug/debug.hpp"

#include <stdlib.h>

#include "arch/x86_64/cpu/io_port.hpp"
#include "debug/bootlog.hpp"
#include "handoff/memory_layout.h"
#include "util/memory.h"

Debug debug;

Debug::Debug()
{
    serial_state_ = kSerialStateUninitialized;
    vga_mirror_enabled_ = false;
    vga_cursor_initialized_ = false;
    vga_cursor_ = 0;
    init_serial();
}

void Debug::set_vga_mirror_enabled(bool enabled)
{
    vga_mirror_enabled_ = enabled;
    if(enabled && !vga_cursor_initialized_)
    {
        initialize_vga_cursor();
    }
}

void Debug::init_serial()
{
    outb(PORT + 1, 0x00);  // Disable all interrupts
    outb(PORT + 3, 0x80);  // Enable DLAB (set baud rate divisor)
    outb(PORT + 0, 0x01);  // Set divisor to 1 (lo byte) 115200 baud
    outb(PORT + 1, 0x00);  //                  (hi byte)
    outb(PORT + 3, 0x03);  // 8 bits, no parity, one stop bit
    outb(PORT + 2, 0xC7);  // Enable FIFO, clear them, with 14-byte threshold
    outb(PORT + 4, 0x0B);  // IRQs enabled, RTS/DSR set
    serial_state_ = kSerialStateReady;
}

int Debug::busy()
{
    return !(inb(PORT + 5) & 0x20);
}

bool Debug::wait_until_ready()
{
    if(serial_state_ == kSerialStateUnavailable)
    {
        return false;
    }

    if(serial_state_ == kSerialStateUninitialized)
    {
        init_serial();
    }

    for(uint32_t spins = 0; spins < kTransmitReadySpinLimit; ++spins)
    {
        if(!busy())
        {
            return true;
        }
    }

    serial_state_ = kSerialStateUnavailable;
    return false;
}

void Debug::initialize_vga_cursor()
{
    outb(kVgaPortIndex, 0x0F);
    const uint16_t low = inb(kVgaPortData);
    outb(kVgaPortIndex, 0x0E);
    const uint16_t high = inb(kVgaPortData);
    vga_cursor_ = static_cast<uint16_t>((high << 8) | low);
    if(vga_cursor_ >= (kVgaTextColumns * kVgaTextRows))
    {
        vga_cursor_ = 0;
    }
    vga_cursor_initialized_ = true;
}

void Debug::scroll_vga()
{
    volatile uint16_t* const vga =
        kernel_physical_pointer<volatile uint16_t>(kVgaTextBufferPhysicalAddress);
    for(uint16_t row = 1; row < kVgaTextRows; ++row)
    {
        for(uint16_t column = 0; column < kVgaTextColumns; ++column)
        {
            vga[(row - 1) * kVgaTextColumns + column] = vga[row * kVgaTextColumns + column];
        }
    }

    for(uint16_t column = 0; column < kVgaTextColumns; ++column)
    {
        vga[(kVgaTextRows - 1) * kVgaTextColumns + column] =
            (static_cast<uint16_t>(kVgaTextAttribute) << 8) | ' ';
    }

    vga_cursor_ = (kVgaTextRows - 1) * kVgaTextColumns;
}

void Debug::update_vga_cursor()
{
    outb(kVgaPortIndex, 0x0F);
    outb(kVgaPortData, static_cast<uint8_t>(vga_cursor_ & 0xFF));
    outb(kVgaPortIndex, 0x0E);
    outb(kVgaPortData, static_cast<uint8_t>((vga_cursor_ >> 8) & 0xFF));
}

void Debug::write_vga(const char c)
{
    if(!vga_mirror_enabled_)
    {
        return;
    }

    if(!vga_cursor_initialized_)
    {
        initialize_vga_cursor();
    }

    if('\r' == c)
    {
        vga_cursor_ = static_cast<uint16_t>(vga_cursor_ - (vga_cursor_ % kVgaTextColumns));
        update_vga_cursor();
        return;
    }

    if('\n' == c)
    {
        vga_cursor_ = static_cast<uint16_t>(vga_cursor_ +
                                            (kVgaTextColumns - (vga_cursor_ % kVgaTextColumns)));
        if(vga_cursor_ >= (kVgaTextColumns * kVgaTextRows))
        {
            scroll_vga();
        }
        update_vga_cursor();
        return;
    }

    volatile uint16_t* const vga =
        kernel_physical_pointer<volatile uint16_t>(kVgaTextBufferPhysicalAddress);
    vga[vga_cursor_] = (static_cast<uint16_t>(kVgaTextAttribute) << 8) |
                       static_cast<uint8_t>(c);
    ++vga_cursor_;
    if(vga_cursor_ >= (kVgaTextColumns * kVgaTextRows))
    {
        scroll_vga();
    }
    update_vga_cursor();
}

void Debug::write(const char c)
{
    bootlog_write(c);
    write_vga(c);
    if(!wait_until_ready())
    {
        return;
    }
    outb(PORT, c);
}

void Debug::write(const char* str)
{
    while(*str)
    {
        write(*str++);
    }
}

void Debug::write_line(const char* str)
{
    write(str);
    write('\n');
}

void Debug::write_int(uint64_t value, int base, int minimum_digits)
{
    char temp[256];
    utoa(value, temp, base, minimum_digits);
    write(temp);
}

void Debug::write_int_line(uint64_t value, int base, int minimum_digits)
{
    write_int(value, base, minimum_digits);
    write('\n');
}

Debug& Debug::operator()()
{
    write('\n');
    return *this;
}

Debug& Debug::operator()(const char* str)
{
    write(str);
    return *this;
}

Debug& Debug::operator()(uint64_t value, int base, int minimum_digits)
{
    char temp[256];
    utoa(value, temp, base, minimum_digits);
    return (*this)(temp);
}

Debug& Debug::s(const char* str)
{
    write(str);
    return *this;
}

Debug& Debug::u(uint64_t value, int base, int minimum_digits)
{
    char temp[256];
    utoa(value, temp, base, minimum_digits);
    return (*this)(temp);
}

Debug& Debug::nl()
{
    write('\n');
    return *this;
}

void debug_memory(uint64_t begin, uint64_t end)
{
    uint8_t* p = (uint8_t*)begin;
    uint8_t* e = (uint8_t*)end;
    // debug memory zone
    while(p < e)
    {
        debug((uint64_t)p, 16, 16)(":");
        for(int i = 0; i < 32; ++i)
        {
            if(0 == i % 8)
                debug(" ");
            debug(p[i], 16, 2);
        }
        debug(" ");
        char* s = (char*)p;
        for(int i = 0; i < 32; ++i)
        {
            if(s[i] >= 32 && s[i] < 0x7F)
                debug.write(s[i]);
            else
                debug.write('.');
        }
        debug.nl();
        p += 32;
    }
}
