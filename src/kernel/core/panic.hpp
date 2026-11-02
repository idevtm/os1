// Final kernel stop path. Keeping this tiny boundary named makes later panic
// reporting easier without scattering halt loops across exception handlers.
#pragma once

// Disable interrupts and halt forever. While the real-hardware boot log is
// active, reset the machine instead so the next boot can save the log.
[[noreturn]] void halt_forever();
