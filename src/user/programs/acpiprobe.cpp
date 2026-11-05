#include <os1/observe.h>
#include <os1/syscall.hpp>

#include <stddef.h>
#include <stdint.h>

namespace
{
constexpr char kPrefix[] = "[user/acpiprobe] ";

size_t string_length(const char* text)
{
    size_t length = 0;
    while((nullptr != text) && (text[length] != '\0'))
    {
        ++length;
    }
    return length;
}

void write_bytes(const char* data, size_t length)
{
    if((nullptr == data) || (0u == length))
    {
        return;
    }

    (void)os1::user::write(1, data, length);
}

void write_string(const char* text)
{
    write_bytes(text, string_length(text));
}

void write_unsigned(uint64_t value)
{
    char digits[32];
    size_t count = 0;
    do
    {
        digits[count++] = static_cast<char>('0' + (value % 10u));
        value /= 10u;
    } while(value != 0u);

    while(count > 0)
    {
        const char ch = digits[--count];
        (void)os1::user::write(1, &ch, 1);
    }
}

void write_yes_no(bool value)
{
    write_string(value ? "yes" : "no");
}

void write_failure(const char* category,
                   uint32_t probe_count,
                   uint32_t success_count)
{
    write_string(kPrefix);
    write_string("fail ");
    write_string(category);
    write_string(" probes=");
    write_unsigned(probe_count);
    write_string(" success=");
    write_unsigned(success_count);
    write_string("\n");
}

bool observe_acpi_probe(Os1ObserveAcpiRecord& record)
{
    alignas(16) uint8_t buffer[sizeof(Os1ObserveHeader) + sizeof(Os1ObserveAcpiRecord)] = {};

    const long observed = os1::user::observe(OS1_OBSERVE_ACPI, buffer, sizeof(buffer));
    if(observed < static_cast<long>(sizeof(buffer)))
    {
        return false;
    }

    const auto* header = reinterpret_cast<const Os1ObserveHeader*>(buffer);
    if((header->abi_version != OS1_OBSERVE_ABI_VERSION) || (header->kind != OS1_OBSERVE_ACPI) ||
       (header->record_size != sizeof(Os1ObserveAcpiRecord)) || (header->record_count != 1u))
    {
        return false;
    }

    record = *reinterpret_cast<const Os1ObserveAcpiRecord*>(buffer + sizeof(Os1ObserveHeader));
    return true;
}
}  // namespace

int main(int, char**)
{
    Os1ObserveAcpiRecord record{};
    if(!observe_acpi_probe(record))
    {
        write_failure("observe", 0, 0);
        return 1;
    }

    write_string(kPrefix);
    write_string(record.route_success_count ? "route ok cached probes="
                                            : "route unavailable cached probes=");
    write_unsigned(record.route_probe_count);
    write_string(" resolved=");
    write_unsigned(record.route_success_count);
    if(record.route_success_count)
    {
        write_string(" irq=");
        write_unsigned(record.route_irq);
        write_string(" gsi=");
        write_yes_no(0u != record.route_source_is_gsi);
    }
    write_string("\n");

    if(record.power_probe_count || record.power_success_count)
    {
        write_failure("passive contract", record.power_probe_count, record.power_success_count);
        return 1;
    }
    write_string(kPrefix);
    write_string("passive snapshot power control disabled\n");
    return 0;
}
