#include <os1/observe.h>
#include <os1/syscall.hpp>

#include <stddef.h>
#include <stdint.h>

namespace
{
constexpr char kBusyYieldPath[] = "/disk/bin/busyyield";
constexpr uint32_t kMaxCpuRecords = 8;
constexpr uint32_t kObserveAttempts = 2048;

size_t string_length(const char* text)
{
    size_t length = 0;
    while(text[length] != '\0')
    {
        ++length;
    }
    return length;
}

void write_string(const char* text)
{
    os1::user::write(1, text, string_length(text));
}

void write_unsigned(uint64_t value)
{
    char digits[32];
    size_t count = 0;
    do
    {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while(value != 0);

    while(count > 0)
    {
        const char ch = digits[--count];
        os1::user::write(1, &ch, 1);
    }
}

void write_failure(const char* reason)
{
    write_string("[user/smpcheck] fail ");
    write_string(reason);
    write_string("\n");
}

void write_success(uint64_t first_pid, uint32_t first_cpu, uint64_t second_pid, uint32_t second_cpu)
{
    write_string("[user/smpcheck] observed pids ");
    write_unsigned(first_pid);
    write_string("@");
    write_unsigned(first_cpu);
    write_string(" and ");
    write_unsigned(second_pid);
    write_string("@");
    write_unsigned(second_cpu);
    write_string("\n");
}

bool observe_cpus(Os1ObserveCpuRecord*& records, uint32_t& record_count)
{
    static uint8_t buffer[sizeof(Os1ObserveHeader) +
                          kMaxCpuRecords * sizeof(Os1ObserveCpuRecord)] = {};

    const long observed = os1::user::observe(OS1_OBSERVE_CPUS, buffer, sizeof(buffer));
    if(observed < static_cast<long>(sizeof(Os1ObserveHeader)))
    {
        return false;
    }

    const auto* header = reinterpret_cast<const Os1ObserveHeader*>(buffer);
    if((header->kind != OS1_OBSERVE_CPUS) ||
       (header->record_size != sizeof(Os1ObserveCpuRecord)) ||
       (header->record_count > kMaxCpuRecords))
    {
        return false;
    }

    const size_t required = sizeof(Os1ObserveHeader) +
                            static_cast<size_t>(header->record_count) *
                                sizeof(Os1ObserveCpuRecord);
    if(observed < static_cast<long>(required))
    {
        return false;
    }

    records = reinterpret_cast<Os1ObserveCpuRecord*>(buffer + sizeof(Os1ObserveHeader));
    record_count = header->record_count;
    return true;
}

bool observed_child_pair(uint64_t first_pid,
                         uint64_t second_pid,
                         uint32_t& first_cpu,
                         uint32_t& second_cpu)
{
    Os1ObserveCpuRecord* records = nullptr;
    uint32_t record_count = 0;
    if(!observe_cpus(records, record_count))
    {
        return false;
    }

    bool saw_first = false;
    bool saw_second = false;
    for(uint32_t i = 0; i < record_count; ++i)
    {
        if(records[i].current_pid == first_pid)
        {
            saw_first = true;
            first_cpu = records[i].logical_index;
        }
        else if(records[i].current_pid == second_pid)
        {
            saw_second = true;
            second_cpu = records[i].logical_index;
        }
    }

    return saw_first && saw_second && (first_cpu != second_cpu);
}
}  // namespace

int main(int, char**)
{
    const long first_pid_long = os1::user::spawn(kBusyYieldPath);
    if(first_pid_long < 0)
    {
        write_failure("spawn-first");
        return 1;
    }

    const long second_pid_long = os1::user::spawn(kBusyYieldPath);
    if(second_pid_long < 0)
    {
        int first_status = 0;
        (void)os1::user::waitpid(static_cast<uint64_t>(first_pid_long), &first_status);
        write_failure("spawn-second");
        return 1;
    }

    const uint64_t first_pid = static_cast<uint64_t>(first_pid_long);
    const uint64_t second_pid = static_cast<uint64_t>(second_pid_long);
    uint32_t first_cpu = 0;
    uint32_t second_cpu = 0;
    bool observed = false;
    for(uint32_t attempt = 0; attempt < kObserveAttempts; ++attempt)
    {
        if(observed_child_pair(first_pid, second_pid, first_cpu, second_cpu))
        {
            observed = true;
            break;
        }
        os1::user::yield();
    }

    if(!observed)
    {
        int first_status = 0;
        int second_status = 0;
        (void)os1::user::waitpid(first_pid, &first_status);
        (void)os1::user::waitpid(second_pid, &second_status);
        write_failure("observe");
        return 1;
    }

    write_success(first_pid, first_cpu, second_pid, second_cpu);

    int first_status = 0;
    int second_status = 0;
    const long waited_first = os1::user::waitpid(first_pid, &first_status);
    const long waited_second = os1::user::waitpid(second_pid, &second_status);
    if((waited_first != static_cast<long>(first_pid)) || (first_status != 0) ||
       (waited_second != static_cast<long>(second_pid)) || (second_status != 0))
    {
        write_failure("wait");
        return 1;
    }
    return 0;
}
