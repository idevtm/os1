#pragma once

#include <stdint.h>

void lapic_stub_reset();
uint32_t lapic_stub_icr_send_count();
uint32_t lapic_stub_last_icr_high();
uint32_t lapic_stub_last_icr_low();
struct cpu;
void lapic_stub_on_reschedule(void (*callback)(const cpu*));
