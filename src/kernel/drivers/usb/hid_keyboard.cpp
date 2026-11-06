// USB HID boot-keyboard report decoder used by xHCI keyboard input.
#include "drivers/usb/hid_keyboard.hpp"

namespace usb::hid
{
namespace
{
[[nodiscard]] bool has_usage(const uint8_t* usages, uint8_t usage)
{
    for(size_t index = 0; index < 6u; ++index)
    {
        if(usages[index] == usage)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool shift_active(uint8_t modifiers)
{
    return 0 != (modifiers & (kModifierLeftShift | kModifierRightShift));
}
}  // namespace

char boot_key_usage_to_ascii(uint8_t usage, bool shift)
{
    if((usage >= 0x04u) && (usage <= 0x1du))
    {
        const char base = shift ? 'A' : 'a';
        return static_cast<char>(base + (usage - 0x04u));
    }

    switch(usage)
    {
    case 0x1eu:
        return shift ? '!' : '1';
    case 0x1fu:
        return shift ? '@' : '2';
    case 0x20u:
        return shift ? '#' : '3';
    case 0x21u:
        return shift ? '$' : '4';
    case 0x22u:
        return shift ? '%' : '5';
    case 0x23u:
        return shift ? '^' : '6';
    case 0x24u:
        return shift ? '&' : '7';
    case 0x25u:
        return shift ? '*' : '8';
    case 0x26u:
        return shift ? '(' : '9';
    case 0x27u:
        return shift ? ')' : '0';
    case 0x28u:
        return '\n';
    case 0x2au:
        return '\b';
    case 0x2bu:
        return '\t';
    case 0x2cu:
        return ' ';
    case 0x2du:
        return shift ? '_' : '-';
    case 0x2eu:
        return shift ? '+' : '=';
    case 0x2fu:
        return shift ? '{' : '[';
    case 0x30u:
        return shift ? '}' : ']';
    case 0x31u:
        return shift ? '|' : '\\';
    case 0x33u:
        return shift ? ':' : ';';
    case 0x34u:
        return shift ? '"' : '\'';
    case 0x35u:
        return shift ? '~' : '`';
    case 0x36u:
        return shift ? '<' : ',';
    case 0x37u:
        return shift ? '>' : '.';
    case 0x38u:
        return shift ? '?' : '/';
    default:
        return 0;
    }
}

bool boot_keyboard_apply_report(BootKeyboardState& state,
                                const uint8_t* report,
                                size_t report_bytes,
                                char* output,
                                size_t output_capacity,
                                size_t& output_count)
{
    output_count = 0;
    if((nullptr == report) || (report_bytes < kBootKeyboardReportBytes) || (nullptr == output))
    {
        return false;
    }

    const uint8_t modifiers = report[0];
    const uint8_t* usages = report + 2u;
    const bool shift = shift_active(modifiers);

    for(size_t index = 0; index < 6u; ++index)
    {
        const uint8_t usage = usages[index];
        if((0u == usage) || (1u == usage) || has_usage(state.keys, usage))
        {
            continue;
        }

        if(output_count == output_capacity)
        {
            return false;
        }

        const char ascii =
            (modifiers & 0x11u) && usage == 0x07u ? 4 : boot_key_usage_to_ascii(usage, shift);
        if(0 != ascii)
        {
            output[output_count++] = ascii;
        }
    }

    state.modifiers = modifiers;
    for(size_t index = 0; index < 6u; ++index)
    {
        state.keys[index] = usages[index];
    }
    return true;
}
}  // namespace usb::hid