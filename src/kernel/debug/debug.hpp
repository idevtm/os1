// Serial debug logger. This remains the most reliable output path across boot,
// framebuffer, scheduler, and fault-handling failures.
#pragma once

#include <stddef.h>
#include <stdint.h>

class Debug
{
public:
    // initialize COM1 for polling serial output.
#if defined(OS1_HOST_TEST)
    constexpr Debug() = default;
#else
    Debug();
#endif
    // Enable or disable BIOS text-mode VGA mirroring for all debug output.
    void set_vga_mirror_enabled(bool enabled);
    // Mirror all debug output through `sink`, e.g. to the framebuffer terminal
    // on machines without a serial port. nullptr disables the mirror.
    void set_mirror(void (*sink)(char));
    // write one byte to COM1 after waiting for the transmit FIFO.
    void write(const char c);
    // write one byte everywhere except the mirror; console code that already
    // draws the byte on the terminal uses this to avoid duplicates.
    void write_unmirrored(const char c);
    // write a nul-terminated string to COM1.
    void write(const char* str);
    // write a string followed by a newline.
    void write_line(const char* str);
    // Format and write an integer.
    void write_int(uint64_t value, int base = 10, int minimum_digits = 1);
    // Format and write an integer followed by a newline.
    void write_int_line(uint64_t value, int base = 10, int minimum_digits = 1);

    // Finish a chained debug expression with a newline.
    Debug& operator()();
    // Append a string to a chained debug expression.
    Debug& operator()(const char* str);
    // Append a formatted integer to a chained debug expression.
    Debug& operator()(uint64_t value, int base = 10, int minimum_digits = 1);

    // Named string append helper for older call sites.
    Debug& s(const char* str);
    // Named integer append helper for older call sites.
    Debug& u(uint64_t value, int base = 10, int minimum_digits = 1);
    // Named newline helper for older call sites.
    Debug& nl();

private:
    static constexpr uint16_t kVgaTextColumns = 80;
    static constexpr uint16_t kVgaTextRows = 25;
    static constexpr uint16_t kVgaPortIndex = 0x3D4;
    static constexpr uint16_t kVgaPortData = 0x3D5;
    static constexpr uint64_t kVgaTextBufferPhysicalAddress = 0xB8000;
    static constexpr uint8_t kVgaTextAttribute = 0x07;
    static constexpr uint8_t kSerialStateUninitialized = 0;
    static constexpr uint8_t kSerialStateReady = 1;
    static constexpr uint8_t kSerialStateUnavailable = 2;
    static constexpr uint32_t kTransmitReadySpinLimit = 1u << 20;
    static const uint16_t PORT = 0x3F8;
    // Program COM1 to 115200 8N1 and enable FIFO mode.
    void init_serial();
    // Return non-zero while the serial transmitter is busy.
    int busy();
    // Wait briefly for the transmitter to become ready without hanging boot.
    bool wait_until_ready();
    void initialize_vga_cursor();
    void scroll_vga();
    void update_vga_cursor();
    void write_vga(const char c);

    uint8_t serial_state_ = kSerialStateUninitialized;
    bool vga_mirror_enabled_ = false;
    bool vga_cursor_initialized_ = false;
    uint16_t vga_cursor_ = 0;
    void (*mirror_)(char) = nullptr;
};

// Dump a memory range in hex plus printable ASCII to the serial logger.
void debug_memory(uint64_t begin, uint64_t end);
// Global serial logger, constructed before kernel_main runs.
extern Debug debug;
