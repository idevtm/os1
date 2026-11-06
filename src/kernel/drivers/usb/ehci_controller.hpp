#pragma once
#include <stddef.h>
#include <stdint.h>

namespace ehci
{
constexpr uint32_t terminate = 1;
constexpr uint32_t active = 1u << 7;
constexpr uint32_t halted = 1u << 6;
constexpr uint32_t errors = 0x7cu;
struct alignas(32) Qtd
{
    uint32_t next, alternate, token, buffer[5], upper[5];
};
struct alignas(32) QueueHead
{
    uint32_t next, endpoint, capabilities, current;
    uint32_t qtd_next, alternate, token, buffer[5], upper[5];
};
static_assert(sizeof(Qtd) == 64);
static_assert(sizeof(QueueHead) == 96);

bool prepare_qtd(Qtd&,
                 uint64_t buffer,
                 uint32_t bytes,
                 uint8_t pid,
                 bool toggle,
                 uint32_t next = terminate,
                 uint32_t alternate = terminate);
bool prepare_head(
    QueueHead&, uint64_t self, uint64_t first, uint8_t address, uint8_t endpoint, uint16_t packet);
uint32_t remaining(uint32_t token);
bool root_usable(uint32_t status);
// Never change ASE while its command and status bits disagree (EHCI 2.3.1).
bool set_async(volatile uint32_t& command,
               volatile uint32_t& status,
               bool enabled,
               void (*poll)(void*),
               void* context,
               unsigned limit);
}  // namespace ehci
