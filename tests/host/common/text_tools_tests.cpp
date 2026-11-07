#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "text/tools.hpp"
namespace
{
struct Memory
{
    std::string input, output;
    size_t cursor = 0, read_limit = SIZE_MAX, write_limit = SIZE_MAX;
    long read_error = 0, write_error = 0;
    size_t fail_at = SIZE_MAX, reads = 0, writes = 0;
    bool seekable = true;
    static long read(void* c, char* out, size_t n)
    {
        auto& m = *static_cast<Memory*>(c);
        ++m.reads;
        if(m.read_error && m.cursor >= m.fail_at)
            return m.read_error;
        n = std::min({n, m.read_limit, m.cursor < m.input.size() ? m.input.size() - m.cursor : 0});
        if(n)
            std::memcpy(out, m.input.data() + m.cursor, n);
        m.cursor += n;
        return n;
    }
    static long write(void* c, const char* in, size_t n)
    {
        auto& m = *static_cast<Memory*>(c);
        ++m.writes;
        if(m.write_error)
            return m.write_error;
        n = std::min(n, m.write_limit);
        m.output.append(in, n);
        return n;
    }
    static long seek(void* c, int64_t delta, unsigned origin)
    {
        auto& m = *static_cast<Memory*>(c);
        if(!m.seekable)
            return OS1_ERR_UNSUPPORTED;
        int64_t base = origin == OS1_SEEK_CUR   ? m.cursor
                       : origin == OS1_SEEK_END ? m.input.size()
                                                : 0;
        if(delta < -base)
            return OS1_ERR_RANGE;
        m.cursor = base + delta;
        return m.cursor;
    }
    os1::text::Io io()
    {
        return {this, read, write, seek, seekable};
    }
};
std::vector<std::string> lines(const std::string& input)
{
    std::vector<std::string> out;
    size_t start = 0;
    for(size_t i = 0; i < input.size(); ++i)
        if(input[i] == '\n')
        {
            out.push_back(input.substr(start, i + 1 - start));
            start = i + 1;
        }
    if(start < input.size())
        out.push_back(input.substr(start));
    return out;
}
}  // namespace
TEST(TextSelection, ExhaustiveSmallLineBoundariesOnFilesAndStreams)
{
    char ring[32];
    for(unsigned length = 0; length <= 8; ++length)
        for(unsigned bits = 0; bits < (1u << length); ++bits)
        {
            std::string input;
            for(unsigned i = 0; i < length; ++i)
                input += bits & (1u << i) ? '\n' : 'a';
            auto split = lines(input);
            for(size_t count = 0; count <= 10; ++count)
            {
                std::string first, last;
                for(size_t i = 0; i < std::min(count, split.size()); ++i)
                    first += split[i];
                for(size_t i = split.size() - std::min(count, split.size()); i < split.size(); ++i)
                    last += split[i];
                for(bool seekable : {false, true})
                {
                    Memory m{input};
                    m.seekable = seekable;
                    m.read_limit = 3;
                    m.write_limit = 2;
                    auto io = m.io();
                    ASSERT_EQ(0, os1::text::head(io, count));
                    EXPECT_EQ(first, m.output);
                    m.cursor = 0;
                    m.output.clear();
                    ASSERT_EQ(0, os1::text::tail(io, count, ring, sizeof(ring)));
                    EXPECT_EQ(last, m.output) << input << " count=" << count;
                }
            }
        }
}
TEST(TextSelection, LargeLinesAndCurrentPositionDoNotRequireWholeFileStorage)
{
    std::string long_line(100000, 'x');
    Memory m{"skip\n" + long_line + "\nlast"};
    m.cursor = 5;
    m.read_limit = 113;
    auto io = m.io();
    ASSERT_EQ(0, os1::text::head(io, 1));
    EXPECT_EQ(long_line + "\n", m.output);
    EXPECT_EQ(100006u, m.cursor);
    m.output.clear();
    m.cursor = 5;
    ASSERT_EQ(0, os1::text::tail(io, 2, nullptr, 0));
    EXPECT_EQ(long_line + "\nlast", m.output);
    EXPECT_EQ(m.input.size(), m.cursor);
    m.cursor = 10;
    m.output.clear();
    size_t reads = m.reads;
    EXPECT_EQ(0, os1::text::head(io, 0));
    EXPECT_EQ(0, os1::text::tail(io, 0, nullptr, 0));
    EXPECT_EQ(reads, m.reads);
    EXPECT_EQ(10u, m.cursor);
}
TEST(TextSelection, StreamTailKeepsSuffixAndReportsInsufficientHistory)
{
    char ring[8];
    for(auto input : {std::string(100, 'x') + "\na\nb\n", std::string(100, 'x') + "\na\nb"})
    {
        Memory m{input};
        m.seekable = false;
        auto io = m.io();
        ASSERT_EQ(0, os1::text::tail(io, 2, ring, sizeof(ring)));
        EXPECT_EQ(input.substr(101), m.output);
    }
    Memory m{std::string(9, 'x')};
    m.seekable = false;
    auto io = m.io();
    EXPECT_EQ(OS1_ERR_LIMIT, os1::text::tail(io, 1, ring, sizeof(ring)));
    EXPECT_TRUE(m.output.empty());
    m.input = std::string(8, 'x');
    m.cursor = 0;
    EXPECT_EQ(0, os1::text::tail(io, 1, ring, sizeof(ring)));
    EXPECT_EQ(m.input, m.output);
}
TEST(TextSelection, ExactCapacitySuffixRetainsItsPrecedingBoundary)
{
    char ring[8];
    for(auto suffix :
        {std::string("12345678"), std::string("1234567\n"), std::string("123\n456\n")})
    {
        Memory m{"discard\n" + suffix};
        m.seekable = false;
        auto io = m.io();
        ASSERT_EQ(0, os1::text::tail(io, suffix == "123\n456\n" ? 2 : 1, ring, sizeof(ring)));
        EXPECT_EQ(suffix, m.output);
        m.cursor = 0;
        m.output.clear();
        EXPECT_EQ(OS1_ERR_LIMIT, os1::text::tail(io, 3, ring, sizeof(ring)));
        EXPECT_TRUE(m.output.empty());
    }
}
TEST(TextCount, CountsLfAsciiWordsAndAllBytesAcrossShortReads)
{
    Memory m{std::string("one\t two\r\n\vthree\f") + char(0) + "last"};
    m.read_limit = 1;
    auto io = m.io();
    os1::text::Counts result;
    ASSERT_EQ(0, os1::text::count(io, result));
    EXPECT_EQ(1u, result.lines);
    EXPECT_EQ(4u, result.words);
    EXPECT_EQ(m.input.size(), result.bytes);
    os1::text::Counts overflow{};
    overflow.bytes = UINT64_MAX;
    EXPECT_EQ(OS1_ERR_RANGE, overflow.add("x", 1));
    overflow = {};
    overflow.lines = UINT64_MAX;
    EXPECT_EQ(OS1_ERR_RANGE, overflow.add("\n", 1));
    overflow = {};
    overflow.words = UINT64_MAX;
    EXPECT_EQ(OS1_ERR_RANGE, overflow.add("x", 1));
}
TEST(TextSearch, LiteralMatchingPreservesLinesAndHandlesOverlapsAndChunkBoundaries)
{
    char workspace[1024];
    const std::string input = "\nAbaba\nxxababa\n.*\nlastababa";
    for(bool seekable : {false, true})
        for(auto pattern : {"", "aba", ".*", "absent", "Ab"})
        {
            std::string expected;
            uint64_t expected_count = 0;
            for(auto& line : lines(input))
                if(line.find(pattern) != std::string::npos)
                {
                    expected += line;
                    ++expected_count;
                }
            Memory m{input};
            m.seekable = seekable;
            m.read_limit = 2;
            m.write_limit = 3;
            auto io = m.io();
            uint64_t matches = 0;
            ASSERT_EQ(
                0,
                os1::text::search(
                    io, pattern, std::strlen(pattern), matches, workspace, sizeof(workspace)));
            EXPECT_EQ(expected, m.output);
            EXPECT_EQ(expected_count, matches);
        }
}
TEST(TextSearch, SeekableLongLinesAndNonseekablePrefixLimitsAreExplicit)
{
    char workspace[16];
    uint64_t matches = 0;
    Memory m{std::string(100000, 'x') + "needle\n"};
    auto io = m.io();
    ASSERT_EQ(0, os1::text::search(io, "needle", 6, matches, workspace, sizeof(workspace)));
    EXPECT_EQ(1u, matches);
    EXPECT_EQ(m.input, m.output);
    m.cursor = 0;
    m.output.clear();
    m.seekable = false;
    io = m.io();
    EXPECT_EQ(OS1_ERR_LIMIT,
              os1::text::search(io, "needle", 6, matches, workspace, sizeof(workspace)));
    EXPECT_TRUE(m.output.empty());
    m.input = "needle" + std::string(100000, 'x') + "\n";
    m.cursor = 0;
    ASSERT_EQ(0, os1::text::search(io, "needle", 6, matches, workspace, sizeof(workspace)));
    EXPECT_EQ(m.input, m.output);
    EXPECT_LT(m.writes, 200u);
    m.input = std::string(16, 'x') + "\n";
    m.cursor = 0;
    m.output.clear();
    EXPECT_EQ(0, os1::text::search(io, "needle", 6, matches, workspace, sizeof(workspace)));
    EXPECT_EQ(0u, matches);
    EXPECT_TRUE(m.output.empty());
}
TEST(TextIo, PropagatesReadWriteAndPrematureEndFailures)
{
    Memory m{"a\nb\n"};
    m.read_error = OS1_ERR_IO;
    m.fail_at = 0;
    auto io = m.io();
    EXPECT_EQ(OS1_ERR_IO, os1::text::head(io, 1));
    m.read_error = 0;
    m.write_error = OS1_ERR_LIMIT;
    EXPECT_EQ(OS1_ERR_LIMIT, os1::text::head(io, 1));
    m.cursor = 0;
    m.write_error = 0;
    m.write_limit = 0;
    EXPECT_EQ(OS1_ERR_IO, os1::text::head(io, 1));
    m.write_limit = SIZE_MAX;
    m.cursor = 0;
    EXPECT_EQ(OS1_ERR_CHANGED, os1::text::copy_range(io, 0, 20));
    uint64_t matches = 0;
    char buffer[8];
    EXPECT_EQ(OS1_ERR_INVALID, os1::text::search(io, "a\nb", 3, matches, buffer, sizeof(buffer)));
    EXPECT_EQ(OS1_ERR_INVALID, os1::text::search(io, "", 257, matches, buffer, sizeof(buffer)));
}
TEST(TextIdentification, BoundedSignaturesAndAsciiClassification)
{
    auto classify = [](const std::string& s) {
        return os1::text::identify(reinterpret_cast<const unsigned char*>(s.data()), s.size());
    };
    EXPECT_STREQ("empty", classify(""));
    EXPECT_STREQ("ASCII text", classify("text\n\t"));
    EXPECT_STREQ("unknown binary data", classify(std::string("a\0b", 3)));
    EXPECT_STREQ("unknown binary data", classify("\x1b[2J"));
    EXPECT_STREQ("CPIO newc", classify("070701"));
    std::string elf(16, '\0');
    elf[0] = 0x7f;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 2;
    elf[5] = 1;
    elf[6] = 1;
    EXPECT_STREQ("ELF64", classify(elf));
    elf[4] = 1;
    EXPECT_STREQ("ELF32", classify(elf));
    EXPECT_STREQ("unknown binary data", classify(elf.substr(0, 4)));
}
