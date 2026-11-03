// In-memory copy of the shim's serial output for the kernel's boot log.
#include "bootlog.hpp"

#include "paging.hpp"

namespace limine_shim
{
namespace
{
constexpr size_t kShimLogBytes = 8192;
char g_shim_log[kShimLogBytes];
size_t g_shim_log_length = 0;
}  // namespace

void shim_log_write_char(char value)
{
    // Keep the final byte as the terminator the kernel scans for.
    if(g_shim_log_length + 1 < kShimLogBytes)
    {
        g_shim_log[g_shim_log_length++] = value;
    }
}

bool shim_log_physical(uint64_t& physical_address, uint64_t& capacity)
{
    capacity = kShimLogBytes;
    return translate_shim_pointer(g_shim_log, physical_address);
}
}  // namespace limine_shim
