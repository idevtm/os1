// Local APIC interrupt-command helpers for kernel IPIs.
#pragma once

#include <stdint.h>

struct cpu;

bool ipi_initialize();
[[nodiscard]] bool ipi_send(uint8_t apic_id, uint8_t vector);
[[nodiscard]] bool ipi_send_all_but_self(uint8_t vector);
[[nodiscard]] bool ipi_send_reschedule(const cpu* target);
[[nodiscard]] bool ipi_send_tlb_shootdown();
[[nodiscard]] uint8_t ipi_reschedule_vector();
[[nodiscard]] uint8_t ipi_tlb_shootdown_vector();
[[nodiscard]] bool ipi_is_reschedule_vector(uint8_t vector);
[[nodiscard]] bool ipi_is_tlb_shootdown_vector(uint8_t vector);
void ipi_handle_tlb_shootdown();

void ipi_poll_tlb_shootdown();
void ipi_retire_tlb();

// Power entry holds interrupts disabled from quiescence through resume.
constexpr uint8_t kPowerParkVector = 0xF0;
[[nodiscard]] bool ipi_quiesce_for_power();
void ipi_resume_after_power();
void ipi_handle_power_park();
