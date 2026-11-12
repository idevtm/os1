#include <os1/observe.h>
#include <stddef.h>
#include <stdint.h>

#include <os1/syscall.hpp>

namespace
{
constexpr char kWorkerPath[] = "/bin/balanceworker";
constexpr uint32_t kWorkerCount = 16;
constexpr uint32_t kMaxCpuRecords = 8;
constexpr uint32_t kObserveAttempts = 4096;

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
    write_string("[user/balancecheck] fail ");
    write_string(reason);
    write_string("\n");
}

void write_success(uint32_t delta, uint64_t total_runq, uint64_t migrate_total)
{
    write_string("[user/balancecheck] runq delta=");
    write_unsigned(delta);
    write_string(" total=");
    write_unsigned(total_runq);
    write_string(" migrate=");
    write_unsigned(migrate_total);
    write_string("\n");
}

bool observe_cpus(Os1ObserveCpuRecord*& records, uint32_t& record_count)
{
    static uint8_t buffer[sizeof(Os1ObserveHeader) + kMaxCpuRecords * sizeof(Os1ObserveCpuRecord)] =
        {};

    const long observed = os1::user::observe(OS1_OBSERVE_CPUS, buffer, sizeof(buffer));
    if(observed < static_cast<long>(sizeof(Os1ObserveHeader)))
    {
        return false;
    }

    const auto* header = reinterpret_cast<const Os1ObserveHeader*>(buffer);
    if((header->kind != OS1_OBSERVE_CPUS) || (header->record_size != sizeof(Os1ObserveCpuRecord)) ||
       (header->record_count > kMaxCpuRecords))
    {
        return false;
    }

    const size_t required = sizeof(Os1ObserveHeader) +
                            static_cast<size_t>(header->record_count) * sizeof(Os1ObserveCpuRecord);
    if(observed < static_cast<long>(required))
    {
        return false;
    }

    records = reinterpret_cast<Os1ObserveCpuRecord*>(buffer + sizeof(Os1ObserveHeader));
    record_count = header->record_count;
    return true;
}

bool observe_balanced_runqs(uint32_t& delta, uint64_t& total_runq, uint64_t& migrate_total)
{
    Os1ObserveCpuRecord* records = nullptr;
    uint32_t record_count = 0;
    if(!observe_cpus(records, record_count))
    {
        return false;
    }

    uint32_t min_depth = 0xFFFFFFFFu;
    uint32_t max_depth = 0;
    uint32_t booted_cpu_count = 0;
    total_runq = 0;
    migrate_total = 0;
    for(uint32_t i = 0; i < record_count; ++i)
    {
        if(0u == (records[i].flags & OS1_OBSERVE_CPU_FLAG_BOOTED))
        {
            continue;
        }

        ++booted_cpu_count;
        if(records[i].runq_depth < min_depth)
        {
            min_depth = records[i].runq_depth;
        }
        if(records[i].runq_depth > max_depth)
        {
            max_depth = records[i].runq_depth;
        }
        total_runq += records[i].runq_depth;
        migrate_total += records[i].migrate_in;
        migrate_total += records[i].migrate_out;
    }

    if((booted_cpu_count < 2u) || (total_runq < booted_cpu_count))
    {
        return false;
    }

    delta = max_depth - min_depth;
    return delta <= 1u;
}
}  // namespace

int main(int, char**)
{
    uint64_t pids[kWorkerCount] = {};
    uint32_t started = 0;
    for(; started < kWorkerCount; ++started)
    {
        const long pid = os1::user::spawn(kWorkerPath);
        if(pid < 0)
        {
            for(uint32_t i = 0; i < started; ++i)
            {
                int status = 0;
                (void)os1::user::waitpid(pids[i], &status);
            }
            write_failure("spawn");
            return 1;
        }
        pids[started] = static_cast<uint64_t>(pid);
    }

    uint32_t delta = 0;
    uint64_t total_runq = 0;
    uint64_t migrate_total = 0;
    bool observed = false;
    for(uint32_t attempt = 0; attempt < kObserveAttempts; ++attempt)
    {
        if(observe_balanced_runqs(delta, total_runq, migrate_total))
        {
            observed = true;
            break;
        }
        os1::user::yield();
    }

    if(!observed)
    {
        for(uint32_t i = 0; i < started; ++i)
        {
            int status = 0;
            (void)os1::user::waitpid(pids[i], &status);
        }
        write_failure("observe");
        return 1;
    }

    write_success(delta, total_runq, migrate_total);

    bool waited_ok = true;
    for(uint32_t i = 0; i < started; ++i)
    {
        int status = 0;
        const long waited = os1::user::waitpid(pids[i], &status);
        if((waited != static_cast<long>(pids[i])) || (status != 0))
        {
            waited_ok = false;
        }
    }

    if(!waited_ok)
    {
        write_failure("wait");
        return 1;
    }
    return 0;
}
