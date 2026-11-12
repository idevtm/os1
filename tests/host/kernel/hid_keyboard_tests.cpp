#include <gtest/gtest.h>

#include <array>

#include "drivers/usb/hid_keyboard.hpp"

TEST(HidKeyboard, ConvertsSingleKeyPressToAscii)
{
    usb::hid::BootKeyboardState state{};
    std::array<uint8_t, usb::hid::kBootKeyboardReportBytes> report{};
    report[2] = 0x04u;

    std::array<char, 6> output{};
    size_t output_count = 0;
    ASSERT_TRUE(usb::hid::boot_keyboard_apply_report(
        state, report.data(), report.size(), output.data(), output.size(), output_count));
    ASSERT_EQ(1u, output_count);
    EXPECT_EQ('a', output[0]);
}

TEST(HidKeyboard, SuppressesRepeatedHeldKeys)
{
    usb::hid::BootKeyboardState state{};
    std::array<uint8_t, usb::hid::kBootKeyboardReportBytes> report{};
    report[2] = 0x04u;

    std::array<char, 6> output{};
    size_t output_count = 0;
    ASSERT_TRUE(usb::hid::boot_keyboard_apply_report(
        state, report.data(), report.size(), output.data(), output.size(), output_count));
    ASSERT_EQ(1u, output_count);

    output.fill(0);
    ASSERT_TRUE(usb::hid::boot_keyboard_apply_report(
        state, report.data(), report.size(), output.data(), output.size(), output_count));
    EXPECT_EQ(0u, output_count);
}

TEST(HidKeyboard, TracksShiftedCharacters)
{
    usb::hid::BootKeyboardState state{};
    std::array<uint8_t, usb::hid::kBootKeyboardReportBytes> report{};
    report[0] = usb::hid::kModifierLeftShift;
    report[2] = 0x04u;

    std::array<char, 6> output{};
    size_t output_count = 0;
    ASSERT_TRUE(usb::hid::boot_keyboard_apply_report(
        state, report.data(), report.size(), output.data(), output.size(), output_count));
    ASSERT_EQ(1u, output_count);
    EXPECT_EQ('A', output[0]);
}

TEST(HidKeyboard, ConvertsPunctuationAndControlKeys)
{
    usb::hid::BootKeyboardState state{};
    std::array<uint8_t, usb::hid::kBootKeyboardReportBytes> report{};
    report[0] = usb::hid::kModifierRightShift;
    report[2] = 0x1eu;
    report[3] = 0x28u;
    report[4] = 0x2au;

    std::array<char, 6> output{};
    size_t output_count = 0;
    ASSERT_TRUE(usb::hid::boot_keyboard_apply_report(
        state, report.data(), report.size(), output.data(), output.size(), output_count));
    ASSERT_EQ(3u, output_count);
    EXPECT_EQ('!', output[0]);
    EXPECT_EQ('\n', output[1]);
    EXPECT_EQ('\b', output[2]);
}

TEST(HidKeyboard, RejectsTruncatedReports)
{
    usb::hid::BootKeyboardState state{};
    std::array<uint8_t, 4> report{};
    std::array<char, 6> output{};
    size_t output_count = 123u;

    EXPECT_FALSE(usb::hid::boot_keyboard_apply_report(
        state, report.data(), report.size(), output.data(), output.size(), output_count));
    EXPECT_EQ(0u, output_count);
}
TEST(HidKeyboard, LeftAndRightControlDProduceEof)
{
    for(uint8_t control : {0x01, 0x10})
    {
        usb::hid::BootKeyboardState state{};
        uint8_t report[8]{control, 0, 0x07};
        char out[6]{};
        size_t count = 0;
        ASSERT_TRUE(usb::hid::boot_keyboard_apply_report(state, report, 8, out, 6, count));
        ASSERT_EQ(1u, count);
        EXPECT_EQ(4, out[0]);
        ASSERT_TRUE(usb::hid::boot_keyboard_apply_report(state, report, 8, out, 6, count));
        EXPECT_EQ(0u, count);
    }
}

TEST(HidKeyboard, PagerQuitControlsReachConsole)
{
    for(uint8_t modifier : {0x01, 0x10, 0x03, 0x30})
    {
        usb::hid::BootKeyboardState state{};
        uint8_t report[8]{modifier, 0, 0x06};
        char out[6]{};
        size_t count = 0;
        ASSERT_TRUE(usb::hid::boot_keyboard_apply_report(state, report, 8, out, 6, count));
        ASSERT_EQ(1u, count);
        EXPECT_EQ(3, out[0]);
    }
    EXPECT_EQ(27, usb::hid::boot_key_usage_to_ascii(0x29, false));
    EXPECT_EQ(27, usb::hid::boot_key_usage_to_ascii(0x29, true));
}

TEST(HidKeyboard, NavigationKeysHaveDistinctSessionCodes)
{
    EXPECT_EQ(0x80, static_cast<unsigned char>(usb::hid::boot_key_usage_to_ascii(0x52, false)));
    EXPECT_EQ(0x81, static_cast<unsigned char>(usb::hid::boot_key_usage_to_ascii(0x51, false)));
    EXPECT_EQ('\t', usb::hid::boot_key_usage_to_ascii(0x2b, false));
}

TEST(HidKeyboard, GuidedCompletionKeysRemainDistinct)
{
    EXPECT_EQ(0x87, static_cast<unsigned char>(usb::hid::boot_key_usage_to_ascii(0x2b, true)));
    for(uint8_t control : {0x01, 0x10})
    {
        usb::hid::BootKeyboardState state{};
        uint8_t report[8]{control, 0, 0x2c};
        char out[6]{};
        size_t count = 0;
        ASSERT_TRUE(usb::hid::boot_keyboard_apply_report(state, report, 8, out, 6, count));
        ASSERT_EQ(1u, count);
        EXPECT_EQ(0x88, static_cast<unsigned char>(out[0]));
    }
}

TEST(HidKeyboard, ErrorReportsPreserveHeldKeysAndModifiers)
{
    for(uint8_t error : {1, 2, 3})
    {
        usb::hid::BootKeyboardState state{};
        constexpr uint8_t shift = usb::hid::kModifierLeftShift;
        uint8_t report[8]{shift, 0, 4};
        char output[6]{};
        size_t count = 0;
        ASSERT_TRUE(usb::hid::boot_keyboard_apply_report(state, report, 8, output, 6, count));
        ASSERT_EQ(1u, count);
        EXPECT_EQ('A', output[0]);

        // A valid key before the error must not produce input either.
        uint8_t invalid[8]{0, 0, 5, error, error, error, error, error};
        ASSERT_TRUE(usb::hid::boot_keyboard_apply_report(state, invalid, 8, output, 6, count));
        EXPECT_EQ(0u, count);
        EXPECT_EQ(shift, state.modifiers);
        EXPECT_EQ(4u, state.keys[0]);

        report[3] = 5;
        ASSERT_TRUE(usb::hid::boot_keyboard_apply_report(state, report, 8, output, 6, count));
        ASSERT_EQ(1u, count);
        EXPECT_EQ('B', output[0]);
    }
}
