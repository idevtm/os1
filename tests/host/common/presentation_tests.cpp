#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "discovery/presentation.hpp"
#include "terminal/line.hpp"
namespace
{
struct Screen
{
    unsigned width, height, advances = 0, scrolls = 0;
    int y, x = 0;
    std::vector<uint16_t> cells;
    Screen(unsigned columns = 80, unsigned rows = 25)
        : width(columns), height(rows), y(rows - 1), cells(columns * rows, 0x0720)
    {
        for(unsigned r = 0; r + 1 < rows; ++r)
            for(unsigned c = 0; c < columns; ++c)
                cells[r * columns + c] = 0x0741 + r;
    }
    bool advance()
    {
        ++advances;
        x = 0;
        if(++y >= int(height))
        {
            std::move(cells.begin() + width, cells.end(), cells.begin());
            std::fill(cells.end() - width, cells.end(), 0x0720);
            y = height - 1;
            ++scrolls;
        }
        return true;
    }
    bool row(int delta, const char* text, unsigned bytes)
    {
        return os1::terminal::redraw_line(cells.data(), width, height, y, x, delta, text, bytes);
    }
    std::string line(unsigned row) const
    {
        std::string out;
        for(unsigned i = 0; i < width; ++i)
            out += char(cells[row * width + i] & 255);
        return out;
    }
};
struct Presentation : testing::Test
{
    os1::context::Data data{};
    os1::discovery::View view;
    os1::discovery::Completion state;
    os1::discovery::Presentation display;
    void SetUp() override
    {
        os1::context::defaults(data);
        os1::discovery::Grants grants{};
        grants.root.kind = OS1_OBJECT_DIRECTORY;
        grants.root.rights = OS1_RIGHT_ALL;
        grants.ceiling = OS1_RIGHT_ALL;
        grants.shell = true;
        ASSERT_TRUE(view.build(grants, data, 1));
        ASSERT_TRUE(state.begin(view));
    }
};
TEST_F(Presentation, RepeatedSelectionAndDescentPreserveHistoryAndScreenPosition)
{
    Screen screen;
    ASSERT_TRUE(display.paint(view, state, screen));
    unsigned scrolls = screen.scrolls, advances = screen.advances;
    auto history = std::vector<uint16_t>(screen.cells.begin(), screen.cells.begin() + 15 * 80);
    for(unsigned i = 0; i < 100; ++i)
    {
        state.move(1);
        ASSERT_TRUE(display.paint(view, state, screen));
        EXPECT_EQ(24, screen.y);
        EXPECT_EQ(scrolls, screen.scrolls);
        EXPECT_EQ(advances, screen.advances);
        EXPECT_TRUE(std::equal(history.begin(), history.end(), screen.cells.begin()));
    }
    state.selected = 0;
    ASSERT_TRUE(state.accept(view, true));
    ASSERT_TRUE(display.paint(view, state, screen));
    EXPECT_EQ("os.", std::string(state.line));
    EXPECT_EQ(0, screen.line(16).find("> fs [object]"));
    for(unsigned r = 18; r < 24; ++r)
        EXPECT_EQ(std::string(80, ' '), screen.line(r));
    EXPECT_EQ(scrolls, screen.scrolls);
}
TEST_F(Presentation, ClosingClearsChoicesAndRestoresOriginalCommandRow)
{
    Screen screen;
    ASSERT_TRUE(display.paint(view, state, screen));
    state.cancel();
    ASSERT_TRUE(display.paint(view, state, screen));
    EXPECT_EQ(15, screen.y);
    EXPECT_EQ(4, screen.x);
    EXPECT_EQ(0u, display.reserved);
    for(unsigned r = 16; r < 25; ++r)
        EXPECT_EQ(std::string(80, ' '), screen.line(r));
    unsigned scrolls = screen.scrolls;
    ASSERT_TRUE(state.begin(view));
    ASSERT_TRUE(display.paint(view, state, screen));
    EXPECT_EQ(scrolls, screen.scrolls);  // Cleared space is reused when reopened.
}
TEST_F(Presentation, InputErrorExitRestoresCommandRowBeforeSessionRelease)
{
    Screen screen;
    auto read = [&](long status) {
        os1::discovery::PresentationCleanup<Screen> cleanup{view, state, display, screen};
        EXPECT_TRUE(state.begin(view));
        EXPECT_TRUE(display.paint(view, state, screen));
        return status;  // Includes early returns inside Escape and CSI parsing.
    };
    for(long status : {OS1_ERR_RANGE, OS1_ERR_CLOSED, OS1_ERR_FAULT})
    {
        EXPECT_EQ(status, read(status));
        EXPECT_EQ(15, screen.y);
        EXPECT_EQ(0u, display.reserved);
        EXPECT_FALSE(state.open);
        for(unsigned r = 16; r < 25; ++r)
            EXPECT_EQ(std::string(80, ' '), screen.line(r));
    }
}
TEST_F(Presentation, PartialRedrawFailureCannotEraseEarlierHistoryDuringCleanup)
{
    struct FailingScreen : Screen
    {
        unsigned calls = 0, fail_at;
        explicit FailingScreen(unsigned fail) : fail_at(fail) {}
        bool row(int delta, const char* text, unsigned bytes)
        {
            if(++calls == fail_at)
                return false;
            return Screen::row(delta, text, bytes);
        }
    };
    for(unsigned failed_row = 1; failed_row <= 10; ++failed_row)
    {
        FailingScreen screen(failed_row);
        display = {};
        ASSERT_TRUE(state.begin(view));
        // Reservation moves original rows 9..23 into the retained history area.
        auto history =
            std::vector<uint16_t>(screen.cells.begin() + 9 * 80, screen.cells.begin() + 24 * 80);
        {
            os1::discovery::PresentationCleanup<FailingScreen> cleanup{
                view, state, display, screen};
            EXPECT_FALSE(display.paint(view, state, screen));
        }
        EXPECT_TRUE(std::equal(history.begin(), history.end(), screen.cells.begin()));
        EXPECT_EQ(15, screen.y);
        EXPECT_EQ(0u, display.reserved);
        for(unsigned r = 16; r < 25; ++r)
            EXPECT_EQ(std::string(80, ' '), screen.line(r));
    }
}
TEST_F(Presentation, LongCommandAndSignatureCannotWrapTheDisplay)
{
    Screen screen;
    for(unsigned i = 0; i < 127; ++i)
        ASSERT_TRUE(state.insert('x'));
    ASSERT_TRUE(display.paint(view, state, screen));
    EXPECT_EQ(24, screen.y);
    EXPECT_EQ(79, screen.x);
    EXPECT_TRUE(display.clipped);
    EXPECT_EQ("  > " + std::string(75, 'x') + " ", screen.line(24));
    state = {};
    state.insert('y');
    ASSERT_TRUE(display.paint(view, state, screen));
    EXPECT_EQ("  > y" + std::string(75, ' '), screen.line(24));
    EXPECT_FALSE(display.clipped);
}
TEST_F(Presentation, SmallViewportUsesBoundedPagesAndCanClose)
{
    Screen screen(20, 4);
    display.configure(20, 4);
    ASSERT_TRUE(display.paint(view, state, screen));
    state.selected = state.matches - 1;
    ASSERT_TRUE(display.paint(view, state, screen));
    EXPECT_EQ(3, screen.y);
    EXPECT_EQ(3u, display.reserved);
    state.cancel();
    ASSERT_TRUE(display.paint(view, state, screen));
    EXPECT_EQ(0, screen.y);
}
TEST(TerminalRedraw, InvalidRequestsLeaveTheWholeScreenAndCursorUnchanged)
{
    Screen screen;
    auto original = screen.cells;
    EXPECT_FALSE(screen.row(0, "control\n", 8));
    EXPECT_FALSE(screen.row(201, "x", 1));
    EXPECT_FALSE(screen.row(-201, "x", 1));
    EXPECT_FALSE(screen.row(0, nullptr, 1));
    std::string full(80, 'x');
    EXPECT_FALSE(screen.row(0, full.c_str(), full.size()));
    EXPECT_EQ(original, screen.cells);
    EXPECT_EQ(24, screen.y);
    EXPECT_EQ(0, screen.x);
}
TEST(TerminalRedraw, MovementClampsWithoutScrollingAndShortRowsEraseOldText)
{
    Screen screen;
    ASSERT_TRUE(screen.row(-200, "longer", 6));
    EXPECT_EQ(0, screen.y);
    ASSERT_TRUE(screen.row(0, "x", 1));
    EXPECT_EQ("x" + std::string(79, ' '), screen.line(0));
    ASSERT_TRUE(screen.row(200, "last", 4));
    EXPECT_EQ(24, screen.y);
    EXPECT_EQ("x" + std::string(79, ' '), screen.line(0));
}
}  // namespace
