#pragma once
#include <stddef.h>
#include <stdint.h>

namespace usb::hub
{
struct Descriptor
{
    uint8_t ports;
    uint16_t power_delay_ms;
};
bool parse_descriptor(const uint8_t*, size_t, Descriptor&);
bool configuration(const uint8_t*, size_t, uint8_t& value);
struct Transport
{
    void* context;
    bool (*control)(void*,
                    uint8_t type,
                    uint8_t request,
                    uint16_t value,
                    uint16_t index,
                    void* data,
                    uint16_t length);
    void (*delay_ms)(void*, unsigned);
    bool (*child)(void*, uint8_t port);
    // Called only for a stable, connected low/full-speed child after reset.
    void (*unsupported)(void*, uint8_t port, unsigned speed_mbps_x10) = nullptr;
    bool split_children = false;
};
bool port_status(const Transport&, uint8_t port, uint32_t&);
bool stable_high_speed(uint32_t status);
unsigned unsupported_speed(uint32_t status);
// Enumerate only high-speed children. Unsupported speeds remain unconfigured.
bool power_ports(const Transport&, const Descriptor&);
bool enumerate(const Transport&, const Descriptor&);
bool enumerate_port(const Transport&, uint8_t port, bool debounce = true);
}  // namespace usb::hub
