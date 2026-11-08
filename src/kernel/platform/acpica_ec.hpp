#pragma once
#include <stdint.h>

// Called only after explicit namespace activation, under system-request admission.
uint32_t acpica_ec_initialize();
const char* acpica_ec_last_error();
// Namespace termination must finish before controller contexts are reset.
void acpica_ec_reset();

#if defined(OS1_HOST_TEST)
struct AcpicaEcTestIo
{
    void* context;
    uint32_t (*read)(void*, uint16_t, uint8_t&);
    uint32_t (*write)(void*, uint16_t, uint8_t);
    void (*stall)(void*, uint32_t);
};
void acpica_ec_set_test_io(AcpicaEcTestIo io);
uint32_t acpica_ec_test_access(
    unsigned index, unsigned function, uint64_t address, unsigned width, uint64_t& value);
#endif
