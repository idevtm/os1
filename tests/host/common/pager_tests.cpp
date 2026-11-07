#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "pager/render.hpp"
namespace
{
struct File
{
    std::string bytes, output;
    size_t cursor = 0, reads = 0, max_read = 3, max_write = 2;
    long error = 0;
    static long read(void* p, char* out, size_t n)
    {
        auto& f = *static_cast<File*>(p);
        ++f.reads;
        if(f.error)
            return f.error;
        size_t size = std::min({n, f.max_read, f.bytes.size() - f.cursor});
        std::memcpy(out, f.bytes.data() + f.cursor, size);
        f.cursor += size;
        return size;
    }
    static long write(void* p, const char* bytes, size_t n)
    {
        auto& f = *static_cast<File*>(p);
        n = std::min(n, f.max_write);
        f.output.append(bytes, n);
        return n;
    }
    static long seek(void* p, int64_t offset, unsigned origin)
    {
        auto& f = *static_cast<File*>(p);
        if(origin || offset < 0 || uint64_t(offset) > f.bytes.size())
            return OS1_ERR_RANGE;
        f.cursor = offset;
        return offset;
    }
    os1::text::Io io()
    {
        return {this, read, write, seek, true};
    }
};
}  // namespace
TEST(Pager, WrappingNewlinesAndBackwardPageBoundaries)
{
    File f{"abcd\nefg h\nlast"};
    auto io = f.io();
    os1::pager::Page page;
    ASSERT_EQ(0, os1::pager::page(io, 0, f.bytes.size(), 4, 2, page));
    EXPECT_EQ("abcd\nefg \n", f.output);
    EXPECT_EQ(9u, page.next);
    EXPECT_FALSE(page.eof);
    f.output.clear();
    ASSERT_EQ(0, os1::pager::page(io, page.next, f.bytes.size(), 4, 2, page));
    EXPECT_EQ("h\nlast\n", f.output);
    EXPECT_TRUE(page.eof);
    uint64_t previous = 99;
    ASSERT_EQ(0, os1::pager::previous(io, 9, f.bytes.size(), 4, 2, previous));
    EXPECT_EQ(0u, previous);
}
TEST(Pager, ControlAndNonAsciiBytesNeverReachTerminal)
{
    File f{std::string("a\033[2J\r\t") + char(0) + char(255)};
    auto io = f.io();
    os1::pager::Page page;
    ASSERT_EQ(0, os1::pager::page(io, 0, f.bytes.size(), 30, 1, page));
    EXPECT_EQ("a\\x1B[2J\\x0D\\x09\\x00\\xFF\n", f.output);
    EXPECT_TRUE(page.eof);
    f.bytes = "abc\tZ";
    f.output.clear();
    f.cursor = 0;
    ASSERT_EQ(0, os1::pager::page(io, 0, f.bytes.size(), 4, 1, page));
    EXPECT_EQ("abc\n", f.output);
    EXPECT_EQ(3u, page.next);
    f.output.clear();
    ASSERT_EQ(0, os1::pager::page(io, 3, f.bytes.size(), 4, 1, page));
    EXPECT_EQ("\\x09\n", f.output);
    EXPECT_EQ(4u, page.next);
}
TEST(Pager, EmptyPartialLinesAndErrorsAreExplicit)
{
    File f{};
    auto io = f.io();
    os1::pager::Page page;
    ASSERT_EQ(0, os1::pager::page(io, 0, 0, 10, 2, page));
    EXPECT_TRUE(page.eof);
    EXPECT_EQ("\n\n", f.output);
    f.bytes = "last";
    f.output.clear();
    ASSERT_EQ(0, os1::pager::page(io, 0, 4, 10, 2, page));
    EXPECT_EQ("last\n\n", f.output);
    f.error = OS1_ERR_IO;
    EXPECT_EQ(OS1_ERR_IO, os1::pager::page(io, 0, 4, 10, 2, page));
    f.error = 0;
    EXPECT_EQ(OS1_ERR_CHANGED, os1::pager::page(io, 0, 5, 10, 2, page));
    EXPECT_EQ(OS1_ERR_INVALID, os1::pager::page(io, 0, 4, 0, 2, page));
    f.max_write = 0;
    EXPECT_EQ(OS1_ERR_IO, os1::pager::page(io, 0, 4, 10, 2, page));
}
TEST(Pager, LongFileUsesBoundedReadsAndStablePageOffsets)
{
    File f{std::string(100000, 'x')};
    f.max_read = SIZE_MAX;
    f.max_write = SIZE_MAX;
    auto io = f.io();
    os1::pager::Page page;
    uint64_t at = 0;
    std::vector<uint64_t> offsets;
    while(at < f.bytes.size())
    {
        offsets.push_back(at);
        f.output.clear();
        ASSERT_EQ(0, os1::pager::page(io, at, f.bytes.size(), 79, 23, page));
        ASSERT_GT(page.next, at);
        EXPECT_LE(page.next - at, 79u * 23u);
        EXPECT_LE(f.output.size(), 80u * 23u);
        at = page.next;
    }
    uint64_t previous = 0;
    ASSERT_EQ(0, os1::pager::previous(io, offsets.back(), f.bytes.size(), 79, 23, previous));
    EXPECT_EQ(offsets[offsets.size() - 2], previous);
}
