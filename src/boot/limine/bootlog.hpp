#pragma once

#include <stdint.h>

#include "limine.h"

namespace limine_shim
{
// Find the RAM boot log window armed by the os1 EFI pre-loader (a reserved
// kBootLogUefiCandidates entry with a valid header). Requires the HHDM; maps
// the low identity window up to kBootLogUefiCandidatesEnd when a window exists.
bool attach_bootlog(const limine_memmap_response* memory_map);
// Append one byte to the attached window; no-op when none was found.
void bootlog_write_char(char value);
}  // namespace limine_shim
