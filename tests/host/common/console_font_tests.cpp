// Host tests of the console font checks, the cell grid change, and the settings files of `sh`
// and `gsh`.
#include <gtest/gtest.h>
#include <os1/ipc.h>

#include <string>
#include <vector>

#include "terminal/cells.hpp"
#include "text/settings.hpp"
namespace
{
using os1::terminal::blank_cell;

std::vector<uint8_t> glyph_payload(unsigned first, unsigned count, unsigned width, unsigned height)
{
    std::vector<uint8_t> payload{uint8_t(first), uint8_t(count), uint8_t(width), uint8_t(height)};
    payload.resize(4 + count * os1::terminal::glyph_bytes(width, height));
    return payload;
}

TEST(ConsoleFont, AcceptsEachSpleenCellForTheConsole)
{
    const unsigned cells[][2] = {{5, 8}, {6, 12}, {8, 16}, {12, 24}, {16, 32}};
    for(const auto& cell : cells)
    {
        const unsigned limit =
            (OS1_IPC_PAYLOAD_MAX - 4) / os1::terminal::glyph_bytes(cell[0], cell[1]);
        auto payload = glyph_payload(0, limit, cell[0], cell[1]);
        EXPECT_LE(payload.size(), size_t(OS1_IPC_PAYLOAD_MAX));
        EXPECT_TRUE(os1::terminal::valid_glyphs(payload.data(), payload.size(), 8, 16))
            << cell[0] << "x" << cell[1];
    }
}

TEST(ConsoleFont, RefusesBadHeaders)
{
    // A cell outside the limits.
    for(auto cell : {std::pair{4u, 8u}, {5u, 7u}, {17u, 32u}, {16u, 33u}, {32u, 64u}})
    {
        auto payload = glyph_payload(0, 1, cell.first, cell.second);
        EXPECT_FALSE(os1::terminal::valid_glyphs(payload.data(), payload.size(), 8, 16));
    }
    // No glyph, a range past 128, and a wrong size.
    auto empty = glyph_payload(0, 0, 8, 16);
    EXPECT_FALSE(os1::terminal::valid_glyphs(empty.data(), empty.size(), 8, 16));
    auto past = glyph_payload(120, 9, 8, 16);
    EXPECT_FALSE(os1::terminal::valid_glyphs(past.data(), past.size(), 8, 16));
    auto shorter = glyph_payload(0, 2, 8, 16);
    EXPECT_FALSE(os1::terminal::valid_glyphs(shorter.data(), shorter.size() - 1, 8, 16));
    EXPECT_FALSE(os1::terminal::valid_glyphs(shorter.data(), 3, 8, 16));
    EXPECT_FALSE(os1::terminal::valid_glyphs(nullptr, 0, 8, 16));
}

TEST(ConsoleFont, NewCellSizeMustStartAtCharacterZero)
{
    auto later = glyph_payload(14, 1, 6, 12);
    EXPECT_FALSE(os1::terminal::valid_glyphs(later.data(), later.size(), 8, 16));
    // With the same cell size, a later range is correct.
    EXPECT_TRUE(os1::terminal::valid_glyphs(later.data(), later.size(), 6, 12));
}

// A cell buffer with space for `capacity` cells. A cell outside the grid has the value 0xFFFF.
struct Grid
{
    unsigned columns;
    std::vector<uint16_t> cells;
    Grid(unsigned c, size_t capacity) : columns(c), cells(capacity, 0xFFFF) {}
    uint16_t at(unsigned x, unsigned y) const
    {
        return cells[y * columns + x];
    }
};

TEST(ConsoleFont, LargerGridKeepsEachRowAndBlanksNewCells)
{
    // 8 x 16 cells to 6 x 12 cells on a 640 x 480 screen.
    Grid grid(80, 106 * 40);
    for(unsigned y = 0; y < 30; ++y)
        for(unsigned x = 0; x < 80; ++x)
            grid.cells[y * 80 + x] = uint16_t(y * 80 + x + 1);
    int row = 29, column = 79;
    ASSERT_TRUE(os1::terminal::resize_cells(
        grid.cells.data(), grid.cells.size(), 80, 30, 106, 40, row, column));
    grid.columns = 106;
    for(unsigned y = 0; y < 40; ++y)
        for(unsigned x = 0; x < 106; ++x)
        {
            const uint16_t expected = y < 30 && x < 80 ? uint16_t(y * 80 + x + 1) : blank_cell;
            ASSERT_EQ(expected, grid.at(x, y)) << x << "," << y;
        }
    EXPECT_EQ(29, row);
    EXPECT_EQ(79, column);
}

TEST(ConsoleFont, SmallerGridKeepsTheCursorRow)
{
    // 6 x 12 cells to 16 x 32 cells on a 640 x 480 screen. The cursor is on row 39.
    Grid grid(106, 106 * 40);
    for(unsigned y = 0; y < 40; ++y)
        for(unsigned x = 0; x < 106; ++x)
            grid.cells[y * 106 + x] = uint16_t(y * 106 + x + 1);
    int row = 39, column = 100;
    ASSERT_TRUE(os1::terminal::resize_cells(
        grid.cells.data(), grid.cells.size(), 106, 40, 40, 15, row, column));
    grid.columns = 40;
    // The last 15 rows stay, with their left 40 cells.
    for(unsigned y = 0; y < 15; ++y)
        for(unsigned x = 0; x < 40; ++x)
            ASSERT_EQ(uint16_t((y + 25) * 106 + x + 1), grid.at(x, y)) << x << "," << y;
    EXPECT_EQ(14, row);
    EXPECT_EQ(39, column);
}

TEST(ConsoleFont, WiderAndShorterGridWithAShift)
{
    // A grid that gets more columns and fewer rows. The rows move up first.
    Grid grid(10, 12 * 8);
    for(unsigned i = 0; i < 80; ++i)
        grid.cells[i] = uint16_t(i + 1);
    int row = 7, column = 3;
    ASSERT_TRUE(os1::terminal::resize_cells(
        grid.cells.data(), grid.cells.size(), 10, 8, 12, 5, row, column));
    grid.columns = 12;
    for(unsigned y = 0; y < 5; ++y)
        for(unsigned x = 0; x < 12; ++x)
            ASSERT_EQ(x < 10 ? uint16_t((y + 3) * 10 + x + 1) : blank_cell, grid.at(x, y))
                << x << "," << y;
    EXPECT_EQ(4, row);
}

TEST(ConsoleFont, GridLargerThanTheBufferChangesNothing)
{
    std::vector<uint16_t> cells(80 * 25, 7);
    int row = 3, column = 4;
    EXPECT_FALSE(
        os1::terminal::resize_cells(cells.data(), cells.size(), 80, 25, 106, 40, row, column));
    EXPECT_EQ(3, row);
    EXPECT_EQ(4, column);
    EXPECT_EQ(std::vector<uint16_t>(80 * 25, 7), cells);
}

TEST(ShellSettings, DefaultIsTheSixByTwelveFont)
{
    os1::settings::ShellSettings settings;
    EXPECT_STREQ("/share/fonts/spleen-6x12.font", settings.font);
}

TEST(ShellSettings, ReadsTheFontAndNone)
{
    os1::settings::ShellSettings settings;
    std::string text = "version=1\n# comment\n\nfont=/share/fonts/spleen-8x16.font\n";
    ASSERT_TRUE(os1::settings::parse_shell(text.data(), text.size(), settings));
    EXPECT_STREQ("/share/fonts/spleen-8x16.font", settings.font);
    text = "version=1\nfont=none\n";
    ASSERT_TRUE(os1::settings::parse_shell(text.data(), text.size(), settings));
    EXPECT_STREQ("", settings.font);
    // Only the version line: the default font.
    text = "version=1\n";
    ASSERT_TRUE(os1::settings::parse_shell(text.data(), text.size(), settings));
    EXPECT_STREQ("/share/fonts/spleen-6x12.font", settings.font);
}

TEST(ShellSettings, RefusesFilesThatAreNotValid)
{
    for(const char* text : {"font=/a\n",
                            "version=2\nfont=/a\n",
                            "version=1\nfont=/a",
                            "version=1\nfont=relative\n",
                            "version=1\nfont=/a\nfont=/b\n",
                            "version=1\ncolor=1\n",
                            "version=1\nfont\n",
                            "version=1\nfont=/a b\n",
                            ""})
    {
        os1::settings::ShellSettings settings;
        settings.font[0] = 'x';
        EXPECT_FALSE(os1::settings::parse_shell(text, std::string(text).size(), settings)) << text;
        EXPECT_EQ('x', settings.font[0]);
    }
}

TEST(GraphicsSettings, DefaultStepsAreOneStepSmaller)
{
    os1::settings::GraphicsSettings settings;
    EXPECT_STREQ("/share/fonts/spleen-6x12.font", settings.font_for(600));
    EXPECT_STREQ("/share/fonts/spleen-6x12.font", settings.font_for(999));
    EXPECT_STREQ("/share/fonts/spleen-8x16.font", settings.font_for(1000));
    EXPECT_STREQ("/share/fonts/spleen-8x16.font", settings.font_for(1799));
    EXPECT_STREQ("/share/fonts/spleen-12x24.font", settings.font_for(2160));
}

TEST(GraphicsSettings, OldFileKeepsTheModeAndTheDefaultSteps)
{
    os1::settings::GraphicsSettings settings;
    const std::string text = "version=1\nwidth=1280\nheight=720\n";
    ASSERT_TRUE(os1::settings::parse_graphics(text.data(), text.size(), 4096, settings));
    EXPECT_EQ(1280u, settings.width);
    EXPECT_EQ(720u, settings.height);
    EXPECT_FALSE(settings.custom_steps);
    EXPECT_STREQ("/share/fonts/spleen-6x12.font", settings.font_for(720));
}

TEST(GraphicsSettings, FontStepsReplaceTheDefaults)
{
    os1::settings::GraphicsSettings settings;
    const std::string text =
        "version=1\nfont=600 /share/fonts/spleen-8x16.font\n"
        "font=2000 /share/fonts/spleen-32x64.font\n";
    ASSERT_TRUE(os1::settings::parse_graphics(text.data(), text.size(), 4096, settings));
    EXPECT_EQ(0u, settings.width);
    EXPECT_TRUE(settings.custom_steps);
    EXPECT_EQ(2u, settings.step_count);
    // No step for a mode height under 600: the built-in font.
    EXPECT_EQ(nullptr, settings.font_for(599));
    EXPECT_STREQ("/share/fonts/spleen-8x16.font", settings.font_for(1999));
    EXPECT_STREQ("/share/fonts/spleen-32x64.font", settings.font_for(2160));
}

TEST(GraphicsSettings, RefusesFilesThatAreNotValid)
{
    for(const char* text : {"version=1\nwidth=1280\n",
                            "version=1\nheight=720\n",
                            "version=1\nwidth=0\nheight=720\n",
                            "version=1\nwidth=01280\nheight=720\n",
                            "version=1\nwidth=5000\nheight=720\n",
                            "version=1\nfont=1000 /a\nfont=1000 /b\n",
                            "version=1\nfont=1000 /a\nfont=900 /b\n",
                            "version=1\nfont=/a\n",
                            "version=1\nfont=x /a\n",
                            "version=1\nfont=1000 a\n",
                            "version=1\nfont=0 /1\nfont=1 /2\nfont=2 /3\nfont=3 /4\nfont=4 /5\n"
                            "font=5 /6\nfont=6 /7\nfont=7 /8\nfont=8 /9\n",
                            "version=1\nmode=1\n"})
    {
        os1::settings::GraphicsSettings settings;
        settings.width = 7;
        EXPECT_FALSE(os1::settings::parse_graphics(text, std::string(text).size(), 4096, settings))
            << text;
        EXPECT_EQ(7u, settings.width);
    }
}

TEST(GraphicsSettings, SaveKeepsTheOtherLines)
{
    const std::string text =
        "version=1\n# fonts\nwidth=1280\nfont=0 /share/fonts/spleen-5x8.font\nheight=720\n";
    std::string kept;
    os1::settings::other_graphics_lines(
        text.data(), text.size(), [&](const char* line, size_t bytes) {
            kept.append(line, bytes);
        });
    EXPECT_EQ("# fonts\nfont=0 /share/fonts/spleen-5x8.font\n", kept);
    // The saved file is valid and has the same steps.
    const std::string saved = "version=1\n" + kept + "width=1920\nheight=1080\n";
    os1::settings::GraphicsSettings settings;
    ASSERT_TRUE(os1::settings::parse_graphics(saved.data(), saved.size(), 4096, settings));
    EXPECT_EQ(1920u, settings.width);
    EXPECT_STREQ("/share/fonts/spleen-5x8.font", settings.font_for(100));
}

TEST(Settings, RefusesAnOversizedFile)
{
    std::string text = "version=1\n";
    while(text.size() <= os1::settings::file_limit)
        text += "# padding\n";
    os1::settings::ShellSettings settings;
    EXPECT_FALSE(os1::settings::parse_shell(text.data(), text.size(), settings));
}
}  // namespace
