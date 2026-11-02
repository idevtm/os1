#include "serial.hpp"

#include "arch/x86_64/cpu/io_port.hpp"
#include "bootlog.hpp"

namespace limine_shim
{
void init_serial()
{
    constexpr uint16_t kSerialPort = 0x3F8;
    outb(kSerialPort + 1, 0x00);
    outb(kSerialPort + 3, 0x80);
    outb(kSerialPort + 0, 0x01);
    outb(kSerialPort + 1, 0x00);
    outb(kSerialPort + 3, 0x03);
    outb(kSerialPort + 2, 0xC7);
    outb(kSerialPort + 4, 0x0B);
}

void write_serial_char(char value)
{
    shim_log_write_char(value);
    constexpr uint16_t kSerialPort = 0x3F8;
    while(0 == (inb(kSerialPort + 5) & 0x20))
    {
    }
    outb(kSerialPort, static_cast<uint8_t>(value));
}

void write_serial(const char* text)
{
    if(nullptr == text)
    {
        return;
    }
    while(*text)
    {
        write_serial_char(*text++);
    }
}

void write_serial_ln(const char* text)
{
    write_serial(text);
    write_serial("\n");
}

void write_serial_hex(uint64_t value)
{
    static const char hexdigits[] = "0123456789ABCDEF";
    for(int nibble = 15; nibble >= 0; --nibble)
    {
        const uint8_t digit = (value >> (nibble * 4)) & 0xFu;
        write_serial_char(hexdigits[digit]);
    }
}
}  // namespace limine_shim