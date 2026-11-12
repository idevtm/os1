#pragma once
#include <os1/startup.h>

namespace os1::shell_startup
{
constexpr uint64_t schema_id = 0x3154524154534853ull;
constexpr unsigned field_id = 1;
constexpr char profile[] = "/etc/shell/init.sh";

// Rich parameter wire format. The record has one present optional String.
constexpr os1_constructor_args boot_arguments()
{
    constexpr unsigned length = sizeof(profile) - 1;
    os1_constructor_args args{};
    args.schema_id = schema_id;
    args.version = 1;
    args.size = 11 + length;
    args.payload[0] = 1;
    args.payload[2] = field_id;
    args.payload[6] = 3 + length;
    args.payload[8] = 1;
    args.payload[9] = length;
    for(unsigned i = 0; i < length; ++i)
        args.payload[11 + i] = profile[i];
    return args;
}
}  // namespace os1::shell_startup
