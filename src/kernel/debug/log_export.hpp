#pragma once
#include <os1/log.h>
namespace log_export
{
// Short shared admission for foreground filesystem entry points. No I/O is done here.
struct ForegroundGuard
{
    bool held;
    bool acquired;
    explicit ForegroundGuard(bool required = true);
    ~ForegroundGuard();
};
// Only calls that can enter a filesystem backend need export admission.
bool syscall_needs_filesystem(uint64_t number, uint64_t operation);
void initialize();
void poll();
long run(unsigned target);
// False means admission deferred before any export effect. Other results are final.
bool try_run(unsigned target, long& result);
void final_drain();
os1_log_export_info info();
#ifdef OS1_HOST_TEST
void test_reset();
#endif
}  // namespace log_export
