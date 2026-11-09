#pragma once
#include <os1/log.h>
namespace log_export
{
// Short shared admission for foreground filesystem entry points. No I/O is done here.
struct ForegroundGuard
{
    bool held;
    ForegroundGuard();
    ~ForegroundGuard();
};
void initialize();
void poll();
long run(unsigned target);
void final_drain();
os1_log_export_info info();
#ifdef OS1_HOST_TEST
void test_reset();
#endif
}  // namespace log_export
