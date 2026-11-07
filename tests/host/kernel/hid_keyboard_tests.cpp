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
