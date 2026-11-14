#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "driver/gm206.h"

namespace
{
// A model of the channel scheduler and the copy engine of the card. The model reads the channel
// data from video memory as the card does: the instance block, the page tables, the command
// ring, and the commands. It runs when the code under test waits.
struct Card
{
    // How the scheduler learns of new commands. See `CopyState::kick`.
    enum Notice
    {
        UserArea = 1,
        Enable,
        Reader,
        Never
    };
    static constexpr uint32_t copy_runlist = 2, copy_engine = 5, copy_reset = 21, reader_index = 1;
    std::unique_ptr<uint8_t, void (*)(void*)> video{
        static_cast<uint8_t*>(std::calloc(gm206::aperture_bytes, 1)), std::free};
    std::map<unsigned, uint32_t> registers;
    std::vector<std::pair<unsigned, uint32_t>> writes;
    Notice notice = UserArea;
    bool physical_allowed = true, translated_allowed = true, has_engine = true, dead = false;
    // A command error does not stop the release of the completion word.
    bool release_after_error = false;
    // The translation flush and the run list commit never end.
    bool flush_busy = false, commit_busy = false;
    // The reader sets its bus request record, and the first run list commit leaves a record.
    bool bus_request = false, commit_record = false;
    unsigned unmapped = 0;
    unsigned launches = 0, direct_overlaps = 0, errors = 0;
    bool failed_launch = false, bind_error = false;
    // Engine state.
    uint32_t get = 0, put = 0, object = 0, methods[0x800 / 4]{};
    gm206::ProbeIo io{this,
                      [](void* self, unsigned reg) { return static_cast<Card*>(self)->read(reg); },
                      [](void* self, unsigned reg, uint32_t value) {
                          static_cast<Card*>(self)->write(reg, value);
                      },
                      [](uint64_t) { current->work(); }};
    static inline Card* current = nullptr;
    std::vector<std::vector<uint32_t>> lines;

    Card()
    {
        current = this;
        registers[0x200] = 0x40000020;
        registers[0x2004] = 3;
        registers[0x2390 + reader_index * 4] = 1u << copy_runlist;
        registers[0x2390] = 1;
    }
    void topology()
    {
        // The graphics engine on run list 0, then copy engine 2 on its own run list.
        unsigned row = 0;
        const auto add = [&](uint32_t value) { registers[0x22700 + row++ * 4] = value; };
        add(0x80000000 | (0u << 21) | (0u << 26) | (12u << 9) | 0x34 | 2);
        add((0u << 2) | 3);
        if(!has_engine)
            return;
        add(0x80000000 | (0x1bu << 3) | 4 | 1);
        add(0x80000000 | (copy_engine << 26) | (copy_runlist << 21) | (copy_reset << 9) | 0x34 | 2);
        add((3u << 2) | 3);
    }
    uint32_t& word(uint32_t offset) const
    {
        EXPECT_LT(offset, gm206::aperture_bytes);
        return *reinterpret_cast<uint32_t*>(video.get() + offset % gm206::aperture_bytes);
    }
    // The kernel maps only the listed register ranges for the module.
    void check(unsigned reg)
    {
        bool mapped = false;
        for(const auto& range : gm206::register_ranges)
            mapped = mapped || (reg >= range.offset && reg - range.offset < range.bytes);
        unmapped += !mapped;
        EXPECT_TRUE(mapped) << std::hex << reg;
    }
    uint32_t read(unsigned reg)
    {
        check(reg);
        if(reg == 0x100c80)
            return flush_busy ? 0x00ff0000 : 0x00ff8000;
        if(reg == 0x2284 + copy_runlist * 8)
            return commit_busy ? 0x00100000 : 0;
        if(reg == 0x3080 + reader_index * 4)
            return active() ? 0x2000 : 0;
        if(reg == 0x40000 + reader_index * 0x2000 + 0x120)
            return 0;  // The reader holds channel 0.
        return registers.count(reg) ? registers[reg] : 0;
    }
    void write(unsigned reg, uint32_t value)
    {
        check(reg);
        writes.push_back({reg, value});
        const unsigned control = 0x800004;
        if(reg == control)
        {
            // Bits 10 and 11 set and clear the enable state in bit 0.
            uint32_t state = (registers[control] & 1) | (value & 0x000f0000);
            if(value & 0x400)
                state |= 1;
            if(value & 0x800)
                state &= ~1u;
            registers[control] = state;
            if((value & 0x400) && notice == Enable)
                put = word(user() + 0x8c);
            return;
        }
        if(reg == 0x40000 + reader_index * 0x2000 && notice == Reader)
            put = value;
        if(reg == 0x2274 && bind_error && (value & 0xfffff))
            registers[0x2100] |= 1;
        if(reg == 0x2274 && commit_record && (value & 0xfffff))
            registers[0x2100] |= 0x10000;
        // Each of these records clears the bits that a write has set.
        const unsigned records = 0x40000 + reader_index * 0x2000;
        if(reg == 0x2100 || reg == 0x259c || reg == 0x25a0 || reg % 0x2000 == 0x108 ||
           reg % 0x2000 == 0x148 || reg == records + 0x108)
        {
            registers[reg] &= ~value;
            return;
        }
        if(reg == 0x104000 + 2 * 0x1000 + 0x908)
        {
            registers[reg] &= ~value;
            return;
        }
        registers[reg] = value;
    }
    uint32_t instance() const
    {
        return (registers.at(0x800000) & 0x0fffffff) << 12;
    }
    uint32_t user() const
    {
        return word(instance() + 0x08);
    }
    // Translate a channel address with the page tables in video memory.
    bool translate(uint32_t address, uint32_t& offset)
    {
        const uint32_t directory = word(instance() + 0x200) & ~0xfffu;
        if(address > word(instance() + 0x208) || address >= (64u << 20))
            return false;
        const uint32_t entry = word(directory + 4);
        if((entry & 3) != 1)
            return false;
        const uint32_t table = (entry & ~0xfu) << 8;
        const uint32_t low = word(table + (address >> 12) * 8);
        // Each page is volatile for the device caches.
        if(!(low & 1) || word(table + (address >> 12) * 8 + 4) != 1)
            return false;
        offset = ((low & ~0xfu) << 8) + (address & 0xfff);
        return true;
    }
    bool active()
    {
        if(dead || !(registers[0x800000] & 0x80000000) || !(registers[0x800004] & 1))
            return false;
        // The run list has the channel, and the scheduler and the engine are in operation.
        const uint32_t list = registers[0x2270] << 12;
        return (registers[0x200] & 0x100) && (registers[0x200] & (1u << copy_reset)) &&
               registers[0x2274] == ((copy_runlist << 20) | 1) && word(list) == 0 &&
               word(list + 4) == instance() >> 12 &&
               ((registers[0x800004] >> 16) & 0xf) == copy_runlist &&
               (registers[0x204] & (1u << reader_index)) && channel_state();
    }
    // The channel state words of Nouveau, the translation flush, and the reader setup.
    bool channel_state()
    {
        const uint32_t base = instance(), reader = 0x40000 + reader_index * 0x2000;
        const std::pair<uint32_t, uint32_t> expected[] = {{0x0c, 0},
                                                          {0x10, 0xface},
                                                          {0x30, 0xfffff902},
                                                          {0x4c, 9u << 16},
                                                          {0x84, 0x20400000},
                                                          {0x94, 0x30000001},
                                                          {0x9c, 0x100},
                                                          {0xac, 0x1f},
                                                          {0xb8, 0xf8000000},
                                                          {0xe4, 0x20},
                                                          {0xe8, 0},
                                                          {0xf8, 0x10003080},
                                                          {0xfc, 0x10000010},
                                                          {0x204, 0},
                                                          {0x20c, 0}};
        bool good = word(base + 0x08) % 0x200 == 0;
        for(const auto& [offset, value] : expected)
            good = good && word(base + offset) == value;
        const uint32_t directory = word(base + 0x200);
        return good && registers[0x100cb8] == (directory >> 12) << 4 &&
               registers[0x100cbc] == 0x80000001 && registers[reader + 0x10c] == 0xfffffeff &&
               registers[0x140] == 0 && registers[0x2140] == 0;
    }
    bool address(bool physical, uint32_t value, uint32_t& offset)
    {
        if(!physical)
            return translated_allowed && translate(value, offset);
        offset = value;
        return physical_allowed && methods[0x260 / 4] == 0 && methods[0x264 / 4] == 0;
    }
    bool launch(uint32_t value)
    {
        ++launches;
        if((value & 3) == 2)
        {
            uint32_t source, target;
            const bool both = address(value & 0x1000, methods[0x404 / 4], source) &&
                              address(value & 0x2000, methods[0x40c / 4], target);
            if(!both || (value & 0x780) != 0x380)
            {
                failed_launch = true;
                return false;
            }
            const uint32_t in = methods[0x410 / 4], out = methods[0x414 / 4];
            const uint32_t bytes = methods[0x418 / 4], count = methods[0x41c / 4];
            lines.push_back({source, target, in, out, bytes, count});
            const uint32_t reach = (count - 1) * (in > out ? in : out) + bytes;
            if(source < target + reach && target < source + reach)
                ++direct_overlaps;
            // Copy forward, a byte at a time. An overlap then gives a wrong image.
            for(uint32_t line = 0; line < count; ++line)
                for(uint32_t i = 0; i < bytes; ++i)
                    video.get()[target + line * out + i] = video.get()[source + line * in + i];
        }
        if(((value >> 3) & 3) == 1)
        {
            uint32_t target;
            if(!translate(methods[0x244 / 4], target))
                return false;
            word(target) = methods[0x248 / 4];
        }
        return true;
    }
    void work()
    {
        if(!active())
            return;
        if(bus_request)
            registers[0x40000 + reader_index * 0x2000 + 0x108] |= 0x100;
        // The user area must be at the start of the window of the scheduler.
        if(notice == UserArea && registers[0x2254] == (0x10000000 | (user() >> 12)))
            put = word(user() + 0x8c);
        uint32_t ring;
        if(!translate(word(instance() + 0x48), ring))
            return;
        const uint32_t entries = 1u << ((word(instance() + 0x4c) >> 16) & 0x1f);
        while(get != put)
        {
            uint32_t commands;
            const uint32_t count = word(ring + get * 8 + 4) >> 10;
            const bool mapped = translate(word(ring + get * 8), commands);
            get = (get + 1) % entries;
            word(user() + 0x88) = get;
            bool good = mapped;
            for(uint32_t i = 0; good && i < count;)
            {
                const uint32_t header = word(commands + i++ * 4);
                const uint32_t operation = header >> 29, method = (header & 0x1fff) << 2;
                uint32_t words = (header >> 16) & 0x1fff;
                good = ((header >> 13) & 7) == 4 && method < 0x800;
                if(operation == 4)
                {
                    // An immediate method has its data in the count field.
                    good = good && method == 0x300 && object == 0xb0b5 && launch(words);
                    continue;
                }
                good = good && operation == 1 && i + words <= count;
                for(uint32_t at = method; good && words; --words, at += 4)
                {
                    const uint32_t data = word(commands + i++ * 4);
                    if(at == 0)
                        object = data;
                    else if(at == 0x300)
                    {
                        good = object == 0xb0b5 && launch(data);
                        if(!good && release_after_error)
                        {
                            // Record the error and continue with the next method.
                            ++errors;
                            registers[0x104000 + 2 * 0x1000 + 0x908] |= 4;
                            good = true;
                        }
                    }
                    else
                        methods[at / 4] = data;
                }
            }
            if(!good)
            {
                // A command error stops the rest of these commands and sets an engine record.
                ++errors;
                registers[0x104000 + 2 * 0x1000 + 0x908] |= 4;
            }
        }
    }
};

struct Screen
{
    Card& card;
    gm206::Surface surface{gm206::native_offset, 640, 480, 640 * 4};
    uint32_t& pixel(uint32_t x, uint32_t y) const
    {
        return card.word(surface.offset + y * surface.pitch + x * 4);
    }
    void pattern() const
    {
        for(uint32_t y = 0; y < surface.height; ++y)
            for(uint32_t x = 0; x < surface.width; ++x)
                pixel(x, y) = (y << 16) | x;
    }
};
void no_log(const char*, const uint32_t*, uint32_t) {}

struct Started
{
    Card card;
    gm206::CopyState state{};
    explicit Started(Card::Notice notice = Card::UserArea, bool translated = true)
    {
        card.notice = notice;
        card.translated_allowed = translated;
        card.topology();
    }
    bool start()
    {
        return gm206::copy_start(card.io, card.video.get(), state, no_log);
    }
    bool copy(const Screen& screen,
              uint32_t sx,
              uint32_t sy,
              uint32_t x,
              uint32_t y,
              uint32_t w,
              uint32_t h)
    {
        return gm206::copy_area(
            card.io, card.video.get(), state, no_log, screen.surface, sx, sy, x, y, w, h);
    }
};
}  // namespace

TEST(Gm206Copy, StartsTheChannelAndCopiesAnArea)
{
    Started t;
    ASSERT_TRUE(t.start());
    EXPECT_EQ(t.state.stage, gm206::CopyStage::Ready);
    EXPECT_EQ(t.state.kick, 1);
    EXPECT_FALSE(t.state.physical);
    EXPECT_EQ(t.state.runlist, Card::copy_runlist);
    EXPECT_EQ(t.state.engine, Card::copy_engine);
    EXPECT_EQ(t.state.reader, Card::reader_index);
    EXPECT_EQ(t.state.instance, 2);
    EXPECT_EQ(t.card.errors, 0u);
    // The module enables the scheduler and the engine and changes no other unit.
    EXPECT_EQ(t.card.registers[0x200], 0x40000020u | 0x100 | (1u << Card::copy_reset));
    // The module does not change the BAR 1 block, the BAR 2 block, or the page layout control.
    for(const auto& [reg, value] : t.card.writes)
        EXPECT_TRUE(reg != 0x1704 && reg != 0x1714 && reg != 0x100c80 && reg != 0x1700) << reg;

    Screen screen{t.card};
    screen.pattern();
    ASSERT_TRUE(t.copy(screen, 10, 20, 300, 200, 100, 50));
    for(uint32_t y = 0; y < 480; ++y)
        for(uint32_t x = 0; x < 640; ++x)
        {
            const bool target = x >= 300 && x < 400 && y >= 200 && y < 250;
            const uint32_t expected = target ? ((y - 180) << 16) | (x - 290) : (y << 16) | x;
            ASSERT_EQ(screen.pixel(x, y), expected) << x << "," << y;
        }
    EXPECT_EQ(t.state.copies, 1u);
    EXPECT_EQ(t.card.direct_overlaps, 0u);
}

TEST(Gm206Copy, CopiesAnOverlappingAreaThroughScratchMemory)
{
    // Each direction of the move: down, up, right, left.
    const int moves[][2] = {{0, 16}, {0, -16}, {16, 0}, {-16, 0}, {5, 3}};
    for(const auto& move : moves)
    {
        Started t;
        ASSERT_TRUE(t.start());
        Screen screen{t.card};
        screen.pattern();
        const uint32_t sx = 100, sy = 100, x = sx + move[0], y = sy + move[1];
        ASSERT_TRUE(t.copy(screen, sx, sy, x, y, 200, 120));
        for(uint32_t row = 0; row < 120; ++row)
            for(uint32_t col = 0; col < 200; ++col)
                ASSERT_EQ(screen.pixel(x + col, y + row), ((sy + row) << 16) | (sx + col))
                    << move[0] << "," << move[1];
        // A pixel next to the target keeps its value.
        EXPECT_EQ(screen.pixel(x + 200, y), (y << 16) | (x + 200));
        EXPECT_EQ(t.card.direct_overlaps, 0u);
    }
}

TEST(Gm206Copy, FindsTheKickMethodThatTheSchedulerAccepts)
{
    for(const auto notice : {Card::Enable, Card::Reader})
    {
        Started t(notice);
        ASSERT_TRUE(t.start()) << notice;
        EXPECT_EQ(t.state.kick, notice);
        Screen screen{t.card};
        screen.pattern();
        ASSERT_TRUE(t.copy(screen, 0, 0, 320, 240, 64, 64));
        EXPECT_EQ(screen.pixel(320 + 63, 240 + 63), (63u << 16) | 63u);
    }
}

TEST(Gm206Copy, UsesVideoMemoryAddressesIfTheEngineRefusesChannelAddresses)
{
    Started t(Card::UserArea, false);
    // The engine records the error and releases the completion word.
    t.card.release_after_error = true;
    ASSERT_TRUE(t.start());
    EXPECT_TRUE(t.state.physical);
    EXPECT_EQ(t.card.errors, 1u);
    // The module cleared the engine error record.
    EXPECT_EQ(t.card.registers[0x104000 + 2 * 0x1000 + 0x908], 0u);
    Screen screen{t.card};
    screen.pattern();
    ASSERT_TRUE(t.copy(screen, 0, 470, 0, 0, 640, 10));
    EXPECT_EQ(screen.pixel(639, 9), (479u << 16) | 639u);
    ASSERT_TRUE(t.copy(screen, 0, 0, 0, 5, 640, 10));
    EXPECT_EQ(screen.pixel(639, 14), (479u << 16) | 639u);
}

TEST(Gm206Copy, LeavesTheCardAloneWithoutAUsableEngine)
{
    {
        Started t;
        t.card.registers.erase(t.card.registers.find(0x22700), t.card.registers.end());
        t.card.registers.clear();
        EXPECT_FALSE(t.start());
        EXPECT_EQ(t.state.stage, gm206::CopyStage::Topology);
        EXPECT_TRUE(t.card.writes.empty());
    }
    {
        // BAR 1 with translation: the module cannot reach the channel data.
        Started t;
        t.card.registers[0x1704] = 0x80000123;
        EXPECT_FALSE(t.start());
        EXPECT_TRUE(t.card.writes.empty());
    }
    {
        // Channel 0 has an owner.
        Started t;
        t.card.registers[0x800000] = 0x80000456;
        EXPECT_FALSE(t.start());
        for(const auto& [reg, value] : t.card.writes)
            EXPECT_TRUE(reg == 0x140 || reg == 0x144 || reg == 0x200) << reg;
    }
}

TEST(Gm206Copy, StopsTheChannelIfNoKickMethodOperates)
{
    Started t(Card::Never);
    EXPECT_FALSE(t.start());
    EXPECT_FALSE(t.state.ready);
    EXPECT_EQ(t.state.stage, gm206::CopyStage::Submit);
    EXPECT_EQ(t.card.registers[0x800000], 0u);
    EXPECT_EQ(t.card.registers[0x800004] & 1, 0u);
    EXPECT_EQ(t.card.registers[0x2254], 0u);
    EXPECT_EQ(t.card.registers[0x2274], Card::copy_runlist << 20);
    Screen screen{t.card};
    EXPECT_FALSE(t.copy(screen, 0, 0, 100, 100, 64, 64));
}

TEST(Gm206Copy, StopsTheChannelAfterARequestWithoutCompletion)
{
    Started t;
    ASSERT_TRUE(t.start());
    Screen screen{t.card};
    screen.pattern();
    t.card.dead = true;
    EXPECT_FALSE(t.copy(screen, 0, 0, 100, 100, 64, 64));
    EXPECT_EQ(t.state.stage, gm206::CopyStage::Stopped);
    EXPECT_EQ(t.card.registers[0x800004] & 1, 0u);
    // The surface did not change, and each later request fails without a device access.
    EXPECT_EQ(screen.pixel(100, 100), (100u << 16) | 100u);
    const auto writes = t.card.writes.size();
    EXPECT_FALSE(t.copy(screen, 0, 0, 100, 100, 64, 64));
    EXPECT_EQ(t.card.writes.size(), writes);
}

TEST(Gm206Copy, RefusesAnAreaOutsideTheSurfaceOrTheLimit)
{
    Started t;
    ASSERT_TRUE(t.start());
    Screen screen{t.card};
    const auto launches = t.card.launches;
    EXPECT_FALSE(t.copy(screen, 600, 0, 0, 0, 64, 64));
    EXPECT_FALSE(t.copy(screen, 0, 0, 0, 450, 64, 64));
    EXPECT_FALSE(t.copy(screen, 0, 0, 0, 0, 0, 64));
    EXPECT_FALSE(t.copy(screen, 0, 0, 0, 200, 640, 103));
    EXPECT_FALSE(t.copy(screen, 0xfffffff0, 0, 0, 0, 64, 64));
    Screen console{t.card, {gm206::console_offset, 640, 480, 2560}};
    EXPECT_FALSE(t.copy(console, 0, 0, 100, 100, 64, 64));
    // A refused area does not stop the channel.
    EXPECT_TRUE(t.state.ready);
    EXPECT_EQ(t.card.launches, launches);
    EXPECT_TRUE(t.copy(screen, 0, 0, 0, 200, 640, 102));
}

TEST(Gm206Copy, StopsAtTheLaunchStageWithoutACompletion)
{
    // The engine refuses the test copy and does not release the completion word.
    Started t(Card::UserArea, false);
    EXPECT_FALSE(t.start());
    EXPECT_EQ(t.state.stage, gm206::CopyStage::Launch);
    EXPECT_EQ(t.card.registers[0x800000], 0u);
    // The module made one attempt only. The ring entry of that attempt stays in use.
    EXPECT_EQ(t.card.errors, 1u);
}

TEST(Gm206Copy, RefusesACompletedCopyWithAnErrorRecord)
{
    Started t;
    ASSERT_TRUE(t.start());
    Screen screen{t.card};
    screen.pattern();
    // From now, the engine refuses each transfer but releases the completion word.
    t.card.translated_allowed = false;
    t.card.release_after_error = true;
    const auto writes = t.card.writes.size();
    EXPECT_FALSE(t.copy(screen, 0, 0, 100, 100, 64, 64));
    EXPECT_EQ(t.state.stage, gm206::CopyStage::Stopped);
    EXPECT_FALSE(t.state.ready);
    EXPECT_TRUE(t.state.halted);
    // The request path disables the channel and asks for its removal from the engine. It does
    // not commit a run list, because that step waits.
    bool removal = false;
    for(auto i = writes; i < t.card.writes.size(); ++i)
    {
        EXPECT_NE(t.card.writes[i].first, 0x2270u);
        removal = removal || t.card.writes[i].first == 0x2634;
    }
    EXPECT_TRUE(removal);
    EXPECT_EQ(t.card.registers[0x800004] & 1, 0u);
    // The session end removes the channel from the scheduler.
    gm206::copy_stop(t.card.io, t.card.video.get(), t.state, no_log);
    EXPECT_FALSE(t.state.halted);
    EXPECT_EQ(t.card.registers[0x800000], 0u);
    EXPECT_EQ(t.card.registers[0x2254], 0u);
}

TEST(Gm206Copy, StopsIfTheCardDoesNotAcceptTheChannel)
{
    {
        Started t;
        t.card.flush_busy = true;
        EXPECT_FALSE(t.start());
        EXPECT_EQ(t.state.stage, gm206::CopyStage::Translation);
        // No channel was bound.
        for(const auto& [reg, value] : t.card.writes)
            EXPECT_NE(reg, 0x800000u);
        EXPECT_EQ(t.card.registers[0x2254], 0u);
    }
    {
        Started t;
        t.card.commit_busy = true;
        EXPECT_FALSE(t.start());
        EXPECT_EQ(t.state.stage, gm206::CopyStage::Channel);
        EXPECT_EQ(t.card.registers[0x800000], 0u);
    }
    {
        // A bind error record after the run list commit.
        Started t;
        t.card.bind_error = true;
        EXPECT_FALSE(t.start());
        EXPECT_EQ(t.state.stage, gm206::CopyStage::Channel);
        EXPECT_EQ(t.card.registers[0x800000], 0u);
    }
}

TEST(Gm206Copy, UsesTheRingAgainFromItsStart)
{
    Started t;
    ASSERT_TRUE(t.start());
    Screen screen{t.card};
    screen.pattern();
    // More requests than the ring has entries. Each request moves a marker one pixel to the right.
    for(uint32_t i = 0; i < 600; ++i)
    {
        screen.pixel(i % 500, 0) = 0xabc00000 + i;
        ASSERT_TRUE(t.copy(screen, i % 500, 0, i % 500, 100, 40, 30)) << i;
        ASSERT_EQ(screen.pixel(i % 500, 100), 0xabc00000 + i) << i;
    }
    EXPECT_EQ(t.state.copies, 600u);
    EXPECT_EQ(t.card.errors, 0u);
    EXPECT_EQ(t.card.unmapped, 0u);
}

TEST(Gm206Copy, IgnoresRecordsThatAreNotErrorsOfARequest)
{
    {
        // The bus request record of the reader is set in normal operation.
        Started t;
        t.card.bus_request = true;
        ASSERT_TRUE(t.start());
        EXPECT_FALSE(t.state.physical);
        Screen screen{t.card};
        screen.pattern();
        EXPECT_TRUE(t.copy(screen, 0, 0, 100, 100, 64, 64));
    }
    {
        // A record from the channel start does not count against the test copy.
        Started t;
        t.card.commit_record = true;
        ASSERT_TRUE(t.start());
        EXPECT_FALSE(t.state.physical);
        EXPECT_EQ(t.card.registers[0x2100] & 0x10000, 0u);
    }
}

TEST(Gm206Copy, CountsThePollsOfRequestsOnly)
{
    Started t;
    ASSERT_TRUE(t.start());
    EXPECT_EQ(t.state.polls, 0u);
    EXPECT_EQ(t.state.longest, 0u);
    Screen screen{t.card};
    ASSERT_TRUE(t.copy(screen, 0, 0, 100, 100, 64, 64));
    // The model completes the commands in the first wait.
    EXPECT_EQ(t.state.polls, 1u);
    EXPECT_EQ(t.state.longest, 1u);
}
