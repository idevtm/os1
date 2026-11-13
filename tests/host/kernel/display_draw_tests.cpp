#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "driver/draw.hpp"

namespace
{
// A 16 x 8 surface with a pitch of 20 pixels. Blue is the low byte, as on the desktop card.
struct Surface
{
    static constexpr uint32_t width = 16, height = 8, stride = 20;
    std::vector<uint32_t> video = std::vector<uint32_t>(stride * height, 0xdeadbeef);
    std::vector<uint32_t> shadow = std::vector<uint32_t>(stride * height, 0xdeadbeef);
    os1_framebuffer_surface surface{
        reinterpret_cast<volatile uint8_t*>(video.data()), width, height, stride * 4, 4, 16, 8, 0};
    bool with_shadow = true;

    template<typename Request>
    bool run(uint32_t member, const Request& request, uint32_t bytes = sizeof(Request))
    {
        return display_draw::draw(&surface,
                                  with_shadow ? shadow.data() : nullptr,
                                  member,
                                  reinterpret_cast<const uint8_t*>(&request),
                                  bytes);
    }
    uint32_t at(uint32_t x, uint32_t y) const
    {
        return video[y * stride + x];
    }
    // Give each pixel a value that names its position.
    void number()
    {
        for(uint32_t y = 0; y < height; ++y)
            for(uint32_t x = 0; x < width; ++x)
                video[y * stride + x] = shadow[y * stride + x] = y << 8 | x;
    }
    void expect_shadow_equal() const
    {
        for(uint32_t y = 0; y < height; ++y)
            for(uint32_t x = 0; x < width; ++x)
                EXPECT_EQ(shadow[y * stride + x], video[y * stride + x]) << x << ' ' << y;
    }
    // The pixels between the width and the pitch must keep their value.
    void expect_padding_kept() const
    {
        for(uint32_t y = 0; y < height; ++y)
            for(uint32_t x = width; x < stride; ++x)
                EXPECT_EQ(video[y * stride + x], 0xdeadbeefu) << x << ' ' << y;
    }
};
constexpr uint32_t bitmap_header = sizeof(os1_display_bitmap) - OS1_DISPLAY_BITMAP_BYTES;
}  // namespace

TEST(DisplayDraw, FillsAnAreaInVideoMemoryAndShadow)
{
    Surface s;
    s.number();
    ASSERT_TRUE(s.run(OS1_DISPLAY_FILL_AREA, os1_display_fill{2, 1, 14, 7, 0x123456}));
    for(uint32_t y = 0; y < Surface::height; ++y)
        for(uint32_t x = 0; x < Surface::width; ++x)
            EXPECT_EQ(s.at(x, y), x >= 2 && y >= 1 ? 0x123456u : (y << 8 | x)) << x << ' ' << y;
    s.expect_shadow_equal();
    s.expect_padding_kept();
}

TEST(DisplayDraw, ConvertsTheColorForARedLowSurface)
{
    Surface s;
    s.surface.red_shift = 0;
    s.surface.blue_shift = 16;
    ASSERT_TRUE(s.run(OS1_DISPLAY_FILL_AREA, os1_display_fill{0, 0, 1, 1, 0x123456}));
    EXPECT_EQ(s.at(0, 0), 0x563412u);
}

TEST(DisplayDraw, RefusesRequestsOutsideTheSurfaceOrTheLimits)
{
    Surface s;
    s.number();
    EXPECT_FALSE(s.run(OS1_DISPLAY_FILL_AREA, os1_display_fill{0, 0, 17, 1, 0}));
    EXPECT_FALSE(s.run(OS1_DISPLAY_FILL_AREA, os1_display_fill{16, 0, 1, 1, 0}));
    EXPECT_FALSE(s.run(OS1_DISPLAY_FILL_AREA, os1_display_fill{0, 8, 1, 1, 0}));
    EXPECT_FALSE(s.run(OS1_DISPLAY_FILL_AREA, os1_display_fill{0, 0, 0, 1, 0}));
    EXPECT_FALSE(s.run(OS1_DISPLAY_FILL_AREA, os1_display_fill{0, 0, 1, 1, 0x1000000}));
    EXPECT_FALSE(s.run(OS1_DISPLAY_FILL_AREA, os1_display_fill{0, 0, 1, 1, 0}, 16));
    EXPECT_FALSE(s.run(OS1_DISPLAY_FILL_AREA, os1_display_fill{~0u, 0, 2, 1, 0}));
    EXPECT_FALSE(s.run(OS1_DISPLAY_COPY, os1_display_copy{0, 0, 1, 1, 16, 1}));
    EXPECT_FALSE(s.run(OS1_DISPLAY_COPY, os1_display_copy{1, 0, 0, 0, 16, 1}));
    EXPECT_FALSE(s.run(OS1_DISPLAY_COPY, os1_display_copy{0, 0, 0, 1, 1, 8}));
    EXPECT_FALSE(s.run(7, os1_display_fill{0, 0, 1, 1, 0}));
    os1_display_bitmap bitmap{0, 0, 9, 2, 0xffffff, 0, {}};
    EXPECT_FALSE(s.run(OS1_DISPLAY_BITMAP, bitmap, bitmap_header + 3));
    EXPECT_FALSE(s.run(OS1_DISPLAY_BITMAP, bitmap, bitmap_header + 5));
    bitmap.background = 0x2000000;
    EXPECT_FALSE(s.run(OS1_DISPLAY_BITMAP, bitmap, bitmap_header + 4));
    bitmap = {8, 0, 9, 1, 0, 0, {}};
    EXPECT_FALSE(s.run(OS1_DISPLAY_BITMAP, bitmap, bitmap_header + 2));
    // No refused request changed a pixel.
    for(uint32_t y = 0; y < Surface::height; ++y)
        for(uint32_t x = 0; x < Surface::width; ++x)
            EXPECT_EQ(s.at(x, y), y << 8 | x);
    // A surface with 24-bit pixels has no support.
    s.surface.pixel_bytes = 3;
    EXPECT_FALSE(s.run(OS1_DISPLAY_FILL_AREA, os1_display_fill{0, 0, 1, 1, 0}));
}

TEST(DisplayDraw, RefusesAnAreaLargerThanTheLimit)
{
    constexpr uint32_t width = 2048, rows = OS1_DISPLAY_AREA_LIMIT / width, height = rows + 1;
    std::vector<uint32_t> video(width * height);
    const os1_framebuffer_surface surface{
        reinterpret_cast<volatile uint8_t*>(video.data()), width, height, width * 4, 4, 16, 8, 0};
    const os1_display_fill large{0, 0, width, height, 1}, limit{0, 0, width, rows, 1};
    const os1_display_copy move{0, 0, 0, 0, width, height};
    EXPECT_FALSE(display_draw::draw(&surface,
                                    nullptr,
                                    OS1_DISPLAY_FILL_AREA,
                                    reinterpret_cast<const uint8_t*>(&large),
                                    sizeof(large)));
    EXPECT_FALSE(display_draw::draw(&surface,
                                    nullptr,
                                    OS1_DISPLAY_COPY,
                                    reinterpret_cast<const uint8_t*>(&move),
                                    sizeof(move)));
    EXPECT_EQ(video[0], 0u);
    EXPECT_TRUE(display_draw::draw(&surface,
                                   nullptr,
                                   OS1_DISPLAY_FILL_AREA,
                                   reinterpret_cast<const uint8_t*>(&limit),
                                   sizeof(limit)));
    EXPECT_EQ(video[width * (rows - 1) + width - 1], 1u);
    EXPECT_EQ(video[width * rows], 0u);
}

TEST(DisplayDraw, CopiesOverlappingAreasInEachDirection)
{
    struct Case
    {
        uint32_t source_x, source_y, x, y;
    };
    for(const bool with_shadow : {true, false})
        for(const Case& c : {Case{2, 2, 3, 2},
                             Case{3, 2, 2, 2},
                             Case{2, 1, 2, 2},
                             Case{2, 2, 2, 1},
                             Case{1, 1, 3, 3},
                             Case{3, 3, 1, 1},
                             Case{4, 1, 2, 3},
                             Case{0, 0, 8, 4}})
        {
            Surface s;
            s.with_shadow = with_shadow;
            s.number();
            const uint32_t width = 7, height = 4;
            ASSERT_TRUE(s.run(OS1_DISPLAY_COPY,
                              os1_display_copy{c.source_x, c.source_y, c.x, c.y, width, height}));
            for(uint32_t y = 0; y < Surface::height; ++y)
                for(uint32_t x = 0; x < Surface::width; ++x)
                {
                    const bool target = x >= c.x && x < c.x + width && y >= c.y && y < c.y + height;
                    const uint32_t expected =
                        target ? (y - c.y + c.source_y) << 8 | (x - c.x + c.source_x) : y << 8 | x;
                    EXPECT_EQ(s.at(x, y), expected)
                        << x << ' ' << y << " case " << c.source_x << c.source_y << c.x << c.y;
                }
            if(with_shadow)
                s.expect_shadow_equal();
            s.expect_padding_kept();
        }
}

TEST(DisplayDraw, CopyReadsTheShadowAndNotVideoMemory)
{
    Surface s;
    s.number();
    // Video memory holds different values. A read from it is slow on the card.
    std::fill(s.video.begin(), s.video.end(), 0x777777u);
    ASSERT_TRUE(s.run(OS1_DISPLAY_COPY, os1_display_copy{0, 0, 4, 4, 2, 2}));
    EXPECT_EQ(s.at(4, 4), 0u);
    EXPECT_EQ(s.at(5, 5), 0x101u);
    EXPECT_EQ(s.at(3, 4), 0x777777u);
}

TEST(DisplayDraw, DrawsBitmapRowsWithLeftPixelInBitZero)
{
    Surface s;
    s.number();
    // Two rows of 10 pixels. Each row uses two bytes.
    os1_display_bitmap bitmap{3, 2, 10, 2, 0xffffff, 0x000080, {0x01, 0x02, 0x80, 0x01}};
    ASSERT_TRUE(s.run(OS1_DISPLAY_BITMAP, bitmap, bitmap_header + 4));
    for(uint32_t x = 0; x < 10; ++x)
    {
        EXPECT_EQ(s.at(3 + x, 2), x == 0 || x == 9 ? 0xffffffu : 0x80u) << x;
        EXPECT_EQ(s.at(3 + x, 3), x == 7 || x == 8 ? 0xffffffu : 0x80u) << x;
    }
    EXPECT_EQ(s.at(2, 2), 0x202u);
    EXPECT_EQ(s.at(13, 2), 0x20du);
    EXPECT_EQ(s.at(3, 4), 0x403u);
    s.expect_shadow_equal();
    // A transparent background keeps the pixels that are not set.
    bitmap = {3, 2, 10, 2, 0x00ff00, OS1_DISPLAY_TRANSPARENT, {0x02, 0x00, 0x00, 0x00}};
    ASSERT_TRUE(s.run(OS1_DISPLAY_BITMAP, bitmap, bitmap_header + 4));
    EXPECT_EQ(s.at(3, 2), 0xffffffu);
    EXPECT_EQ(s.at(4, 2), 0x00ff00u);
    EXPECT_EQ(s.at(5, 2), 0x80u);
    s.expect_shadow_equal();
    s.expect_padding_kept();
}

TEST(DisplayDraw, AcceptsTheLargestBitmapOfOneRequest)
{
    constexpr uint32_t width = 232, height = 8;
    std::vector<uint32_t> video(width * height);
    const os1_framebuffer_surface surface{
        reinterpret_cast<volatile uint8_t*>(video.data()), width, height, width * 4, 4, 16, 8, 0};
    os1_display_bitmap bitmap{0, 0, width, height, 0xffffff, 0, {}};
    std::memset(bitmap.bits, 0xff, sizeof(bitmap.bits));
    ASSERT_TRUE(display_draw::draw(&surface,
                                   nullptr,
                                   OS1_DISPLAY_BITMAP,
                                   reinterpret_cast<const uint8_t*>(&bitmap),
                                   sizeof(bitmap)));
    for(const uint32_t value : video)
        ASSERT_EQ(value, 0xffffffu);
}
