#pragma once
#include <stddef.h>
#include <stdint.h>

namespace usb::storage
{
struct Interface
{
    uint8_t configuration{}, number{}, in{}, out{}, in_burst{}, out_burst{};
    uint16_t in_packet{}, out_packet{};
    bool in_companion{}, out_companion{};
};
bool parse_configuration(const void*, size_t, Interface&);

enum class TransferStatus : uint8_t
{
    Ok,
    Stall,
    Failed
};
struct Transfer
{
    TransferStatus status;
    uint32_t bytes;
};
struct Transport
{
    void* context;
    Transfer (*bulk)(void*, bool in, void*, uint32_t bytes);
    bool (*clear_halt)(void*, bool in);
    bool (*reset)(void*);
};
enum class Result : uint8_t
{
    Ok,
    CommandFailed,
    TransportFailed
};
struct Device
{
    Transport transport{};
    uint32_t tag{};
    uint64_t sectors{};
    bool alive{};
    // Boot probe evidence; runtime I/O does not modify these fields.
    const char* probe_stage{};
    Result probe_result = Result::TransportFailed;
    uint8_t probe_opcode{}, probe_mode{};
    uint32_t probe_sense = 0xffffffff;
    bool write_through{};
    Result command(const uint8_t* cdb,
                   uint8_t cdb_bytes,
                   bool in,
                   void*,
                   uint32_t bytes,
                   uint32_t* actual = nullptr);
    bool initialize(Transport);
    bool read_write(bool write, uint64_t sector, void*, uint32_t count);
    bool flush();
    void invalidate();

private:
    bool probe_write_through();
};
}  // namespace usb::storage
