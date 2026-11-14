#include <gtest/gtest.h>

#include <numeric>
#include <vector>

#include "ipc/stream_ring.hpp"

namespace
{
struct Ring
{
    std::vector<uint8_t> memory = std::vector<uint8_t>(OS1_STREAM_BYTES, 0xee);
    StreamRing ring{memory.data(), 0, 0, false, false};
    // Bytes with a value that names their position in the stream.
    static std::vector<uint8_t> pattern(uint32_t first, uint32_t count)
    {
        std::vector<uint8_t> bytes(count);
        for(uint32_t i = 0; i < count; ++i)
            bytes[i] = uint8_t((first + i) * 13 + ((first + i) >> 8));
        return bytes;
    }
};
}  // namespace

TEST(StreamRing, GivesBytesBackInOrder)
{
    Ring r;
    const auto source = Ring::pattern(0, 1000);
    EXPECT_EQ(r.ring.put(source.data(), 1000), 1000u);
    EXPECT_EQ(r.ring.count, 1000u);
    std::vector<uint8_t> target(1000);
    // A reader can take a part of the bytes of one write.
    EXPECT_EQ(r.ring.take(target.data(), 300), 300u);
    EXPECT_EQ(r.ring.take(target.data() + 300, 5000), 700u);
    EXPECT_EQ(target, source);
    EXPECT_EQ(r.ring.count, 0u);
    EXPECT_EQ(r.ring.take(target.data(), 1), 0u);
}

TEST(StreamRing, TakesOnlyItsSizeAndReportsAShortWrite)
{
    Ring r;
    const auto source = Ring::pattern(0, OS1_STREAM_BYTES + 100);
    EXPECT_EQ(r.ring.put(source.data(), OS1_STREAM_BYTES + 100), uint32_t(OS1_STREAM_BYTES));
    EXPECT_EQ(r.ring.count, uint32_t(OS1_STREAM_BYTES));
    // A full ring takes no byte and changes no byte.
    const uint8_t extra = 0x5a;
    EXPECT_EQ(r.ring.put(&extra, 1), 0u);
    std::vector<uint8_t> target(OS1_STREAM_BYTES);
    EXPECT_EQ(r.ring.take(target.data(), OS1_STREAM_BYTES), uint32_t(OS1_STREAM_BYTES));
    EXPECT_TRUE(std::equal(target.begin(), target.end(), source.begin()));
}

TEST(StreamRing, WrapsAtTheEndOfItsMemory)
{
    Ring r;
    uint32_t written = 0, read = 0;
    std::vector<uint8_t> target(7001);
    // Many writes and reads of different sizes move the ring position through its memory.
    for(unsigned step = 0; step < 200; ++step)
    {
        const uint32_t put_size = 1 + (step * 977) % 7000;
        const auto source = Ring::pattern(written, put_size);
        const uint32_t space = OS1_STREAM_BYTES - r.ring.count;
        const uint32_t added = r.ring.put(source.data(), put_size);
        ASSERT_EQ(added, put_size < space ? put_size : space) << step;
        written += added;
        const uint32_t take_size = 1 + (step * 613) % 7001;
        const uint32_t available = r.ring.count;
        const uint32_t taken = r.ring.take(target.data(), take_size);
        ASSERT_EQ(taken, take_size < available ? take_size : available) << step;
        const auto expected = Ring::pattern(read, taken);
        ASSERT_TRUE(std::equal(expected.begin(), expected.end(), target.begin())) << step;
        read += taken;
        ASSERT_LT(r.ring.head, uint32_t(OS1_STREAM_BYTES));
        ASSERT_EQ(r.ring.count, written - read);
    }
    EXPECT_GT(written, 4u * OS1_STREAM_BYTES);
}

TEST(StreamRing, ReportsReadinessForEachEnd)
{
    Ring r;
    // An empty ring: the reader waits and the writer does not.
    EXPECT_FALSE(r.ring.reader_ready());
    EXPECT_TRUE(r.ring.writer_ready());
    const auto source = Ring::pattern(0, OS1_STREAM_BYTES);
    r.ring.put(source.data(), 1);
    EXPECT_TRUE(r.ring.reader_ready());
    r.ring.put(source.data(), OS1_STREAM_BYTES);
    // A full ring: the writer waits.
    EXPECT_FALSE(r.ring.writer_ready());
    // A closed reader end ends the wait of the writer. The writer then gets an error.
    r.ring.reader_closed = true;
    EXPECT_TRUE(r.ring.writer_ready());
    // A closed writer end ends the wait of the reader. The reader first gets the bytes.
    Ring ended;
    ended.ring.writer_closed = true;
    EXPECT_TRUE(ended.ring.reader_ready());
    uint8_t byte = 0;
    EXPECT_EQ(ended.ring.take(&byte, 1), 0u);
    ended.ring.put(source.data(), 2);
    EXPECT_EQ(ended.ring.take(&byte, 1), 1u);
    EXPECT_EQ(byte, source[0]);
}
