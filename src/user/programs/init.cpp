#include <os1/file_tools.hpp>
#include <os1/ipc.hpp>

#include "interface/shell_startup.hpp"

namespace
{
namespace u = os1::user;
namespace files = os1::user::files;
constexpr uint64_t healthy_shell_ns = 1'000'000'000;
constexpr uint64_t shell_retry_ns = 100'000'000;
constexpr uint64_t rescue_retry_ns = 250'000'000;
constexpr unsigned rapid_exit_limit = 3;

class RetryTimer
{
public:
    RetryTimer()
    {
        open();
    }

    void delay(uint64_t duration)
    {
        long now = u::ipc_clock();
        const bool clock_valid = now >= 0 && uint64_t(now) <= INT64_MAX - duration;
        const uint64_t deadline = clock_valid ? uint64_t(now) + duration : 0;
        if(clock_valid)
        {
            if(client.fd < 0)
                open();
            if(client.fd >= 0 && u::ipc_wait_until(client.fd, deadline) == OS1_ERR_TIMED_OUT)
                return;
        }
        // Pace recovery without a timer pair. Bound yields when the clock stalls.
        for(unsigned unchanged = 0; unchanged < 10000;)
        {
            u::yield();
            const long sample = clock_valid ? u::ipc_clock() : -1;
            if(sample >= 0 && uint64_t(sample) >= deadline)
                return;
            if(sample > now && clock_valid)
            {
                now = sample;
                unchanged = 0;
            }
            else
                ++unchanged;
        }
    }

private:
    void open()
    {
        os1_ipc_pair timer{};
        if(u::ipc(OS1_IPC_CREATE, 0, &timer) == 0)
        {
            client.reset(timer.client);
            provider.reset(timer.provider);
        }
    }

    // Both descriptors are CLOEXEC. Neither end sends a notification.
    files::Handle client, provider;
};

void report_status(const char* prefix, long status)
{
    files::message(prefix);
    if(status < 0)
        files::message("-");
    files::number(2, status < 0 ? uint64_t(0) - uint64_t(status) : uint64_t(status));
    files::message("\n");
}
}  // namespace

int main(int, char**)
{
    bool first = true;
    unsigned rapid_exits = 0;
    RetryTimer timer;
    for(;;)
    {
        const char* arguments[] = {"sh", "--boot"};
        constexpr auto constructor = os1::shell_startup::boot_arguments();
        long child = u::run_program("/bin/sh",
                                    first ? 2 : 1,
                                    arguments,
                                    false,
                                    0,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    -1,
                                    &constructor);
        first = false;
        const long started = child >= 0 ? u::ipc_clock() : -1;
        int status = 0;
        const long result = child < 0 ? child : u::waitpid(child, &status);
        if(child >= 0 && result == child)
        {
            const long ended = u::ipc_clock();
            if(started >= 0 && ended >= started && uint64_t(ended - started) >= healthy_shell_ns)
            {
                rapid_exits = 0;
                continue;
            }
            if(++rapid_exits < rapid_exit_limit)
            {
                timer.delay(shell_retry_ns);
                continue;
            }
            files::message("init: disk shell exited repeatedly; starting rescue\n");
            report_status("init: shell exit status=", status);
        }
        else
        {
            files::message("init: cannot start /bin/sh; starting rescue\n");
            report_status("init: launch/wait status=", result);
        }
        files::message("init: deploy programs to ext2 with os1.sh, then boot again\n");
        rapid_exits = 0;
        child = u::spawn("/sbin/rescue");
        if(child < 0 || u::waitpid(child, &status) != child)
            files::message("init: rescue unavailable; retrying\n");
        timer.delay(rescue_retry_ns);
    }
}
