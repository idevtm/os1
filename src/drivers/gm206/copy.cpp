// Copy engine channel of the GM206 module. See `CopyState` in the module header.
#include "driver/gm206.h"

namespace gm206
{
namespace
{
// Video memory of the channel. The scheduler reads the first four objects by their video memory
// address. The channel reads the ring, the commands, and the completion word by a channel address.
constexpr uint32_t instance = copy_offset, user = copy_offset + 0x1000;
constexpr uint32_t runlist = copy_offset + 0x2000, empty_runlist = copy_offset + 0x3000;
constexpr uint32_t ring = copy_offset + 0x4000, commands = copy_offset + 0x5000;
constexpr uint32_t completion = copy_offset + 0x6000;
// The directory and the table have the size of the larger of the two page layouts of the card.
constexpr uint32_t directory = copy_offset + 0x20000, directory_bytes = 0x20000;
constexpr uint32_t table = copy_offset + 0x40000, table_bytes = 0x40000;
constexpr uint32_t scratch = copy_offset + 0x80000, scratch_bytes = 0x80000;
// Channel addresses. All are below 64 MiB and use 4 KiB pages. Thus the two page layouts of the
// card give the same directory entry and the same table index.
constexpr uint32_t control_address = 0x00100000, control_bytes = 0x3000;
constexpr uint32_t scratch_address = 0x00200000, surface_address = 0x02000000;
constexpr uint32_t surface_bytes = console_offset - native_offset;
constexpr uint32_t address_limit = 0x03ffffff;
constexpr uint32_t channel = 0, subchannel = 4, ring_entries = 512, ring_order = 9;
constexpr uint32_t command_words = 0x1000 / 4;

constexpr uint32_t method(uint32_t offset, uint32_t count)
{
    return 0x20000000 | (count << 16) | (subchannel << 13) | (offset >> 2);
}

struct Copier
{
    const ProbeIo& io;
    volatile uint8_t* video;
    CopyState& s;
    CopyLog log;
    uint32_t words = 0;

    uint32_t rd(unsigned reg) const
    {
        return io.read(io.context, reg);
    }
    void wr(unsigned reg, uint32_t value) const
    {
        io.write(io.context, reg, value);
    }
    volatile uint32_t& memory(uint32_t offset) const
    {
        return *reinterpret_cast<volatile uint32_t*>(video + offset);
    }
    unsigned reader() const
    {
        return 0x40000 + s.reader * 0x2000;
    }
    unsigned engine() const
    {
        return 0x104000 + s.instance * 0x1000;
    }
    bool wait(unsigned reg, uint32_t mask, bool set) const
    {
        for(unsigned i = 0; i < 2000; ++i)
        {
            if(bool(rd(reg) & mask) == set)
                return true;
            io.delay_us(100);
        }
        return false;
    }
    void zero(uint32_t offset, uint32_t bytes) const
    {
        for(uint32_t i = 0; i < bytes; i += 4)
            memory(offset + i) = 0;
    }
    // Map pages of video memory at a channel address. The pages are volatile for the device
    // caches, because the processor also reads and writes them.
    void map(uint32_t address, uint32_t offset, uint32_t bytes) const
    {
        for(uint32_t i = 0; i < bytes; i += 0x1000)
        {
            const uint32_t entry = table + ((address + i) >> 12) * 8;
            memory(entry) = ((offset + i) >> 8) | 1;
            memory(entry + 4) = 1;
        }
    }
    // Record the scheduler, command reader, and engine state after a step that failed.
    void record(uint32_t step) const
    {
        const uint32_t faults = rd(0x259c);
        unsigned unit = 0;
        while(unit < 31 && !(faults & (1u << unit)))
            ++unit;
        {
            const uint32_t values[] = {step,
                                       uint32_t(s.stage),
                                       rd(0x2100),
                                       rd(0x252c),
                                       rd(0x254c),
                                       faults,
                                       rd(0x25a0),
                                       rd(0x2640 + s.engine * 8),
                                       rd(0x800000 + channel * 8),
                                       rd(0x800004 + channel * 8),
                                       rd(0x3080 + s.reader * 4),
                                       rd(0x2284 + s.runlist * 8),
                                       memory(user + 0x88),
                                       memory(user + 0x8c),
                                       memory(completion),
                                       s.payload};
            log("gm206 copy state", values, 16);
        }
        {
            const uint32_t values[] = {step,
                                       rd(reader() + 0x108),
                                       rd(reader() + 0x148),
                                       rd(reader() + 0xc0),
                                       rd(reader() + 0xc4),
                                       rd(reader() + 0x120),
                                       rd(reader() + 0x00),
                                       rd(reader() + 0x14),
                                       rd(reader() + 0x08),
                                       rd(reader() + 0x48),
                                       rd(engine() + 0x908),
                                       rd(engine() + 0xf14),
                                       rd(0x2800 + unit * 0x10),
                                       rd(0x2804 + unit * 0x10),
                                       rd(0x2808 + unit * 0x10),
                                       rd(0x280c + unit * 0x10)};
            log("gm206 copy reader", values, 16);
        }
    }
    // Records of the command reader without the bus request bit. Nouveau does not enable that
    // bit, because it is set in normal operation.
    uint32_t reader_errors() const
    {
        return rd(reader() + 0x108) & 0xfffffeff;
    }
    // Error records of the scheduler, the command reader, and the engine. The scheduler bits are:
    // bind, PIO, schedule, channel switch, flush timeout, bus, lost translation fault,
    // translation fault, command reader.
    bool errors() const
    {
        return (rd(0x2100) & 0x39810111) || reader_errors() || rd(reader() + 0x148) ||
               (rd(engine() + 0x908) & 7);
    }
    // Clear the error records, so that the next commands can run.
    void acknowledge() const
    {
        if(reader_errors())
            wr(reader() + 0xc0, 0x80600008);
        wr(reader() + 0x108, rd(reader() + 0x108));
        if(const uint32_t status = rd(reader() + 0x148))
            wr(reader() + 0x148, status);
        if(const uint32_t status = rd(engine() + 0x908))
        {
            wr(engine() + 0xf14, 0);
            wr(engine() + 0x908, status);
        }
        wr(0x259c, rd(0x259c));
        wr(0x25a0, rd(0x25a0));
        wr(0x2100, rd(0x2100));
    }
    bool commit(uint32_t buffer, uint32_t count) const
    {
        wr(0x2270, buffer >> 12);
        wr(0x2274, (uint32_t(s.runlist) << 20) | count);
        return wait(0x2284 + s.runlist * 8, 0x00100000, false);
    }
    // Disable the channel at once. A later `stop` call removes it from the scheduler.
    void halt(CopyStage stage) const
    {
        const unsigned control = 0x800004 + channel * 8;
        wr(control, rd(control) | 0x800);
        // Ask the scheduler to take the channel off the engine. Do not wait for it here.
        wr(0x2634, channel);
        s.ready = false;
        s.halted = true;
        s.stage = stage;
    }
    // Stop the channel. The display and the BAR 1 access do not change.
    void stop(CopyStage stage) const
    {
        const unsigned control = 0x800004 + channel * 8;
        if(s.stage >= CopyStage::Channel)
        {
            wr(control, rd(control) | 0x800);
            wr(0x2634, channel);
            const bool idle = wait(0x2634, 0x00100000, false);
            const bool removed = commit(empty_runlist, 0);
            wr(0x800000 + channel * 8, 0);
            // If the channel did not go idle, the engine can write video memory after this.
            const uint32_t values[] = {idle, removed, rd(0x2640 + s.engine * 8), rd(control)};
            log("gm206 copy stop", values, 4);
        }
        if(s.stage >= CopyStage::Memory)
        {
            wr(0x2254, 0);
            wr(0x2100, 0xffffffff);
        }
        s.ready = s.halted = false;
        s.stage = stage;
    }

    void push(uint32_t word)
    {
        if(words < command_words)
            memory(commands + words++ * 4) = word;
    }
    // Start new commands. The completion word is zero until the commands end.
    void begin()
    {
        words = 0;
        memory(completion) = 0;
        if(!s.bound)
        {
            push(method(0x000, 1));
            push(0xb0b5);
            s.bound = true;
        }
    }
    void copy(uint32_t source,
              uint32_t target,
              uint32_t pitch_in,
              uint32_t pitch_out,
              uint32_t bytes,
              uint32_t lines)
    {
        // Video memory is the target of the two physical modes.
        push(method(0x260, 2));
        push(0);
        push(0);
        push(method(0x400, 8));
        push(0);
        push(source);
        push(0);
        push(target);
        push(pitch_in);
        push(pitch_out);
        push(bytes);
        push(lines);
        // Transfer without pipeline, pitch layout for the two sides, and more than one line.
        // Bits 12 and 13 select addresses without translation.
        push(method(0x300, 1));
        push(s.physical ? 0x3382 : 0x0382);
    }
    // End the commands with a release of the completion word, and put them in the ring.
    void end()
    {
        s.payload = s.payload + 1 ? s.payload + 1 : 1;
        push(method(0x240, 3));
        push(0);
        push(control_address + (completion - ring));
        push(s.payload);
        // An immediate launch: no transfer, a cache flush, and a release of one word.
        push(0x800c80c0);
        memory(ring + s.put * 8) = control_address + (commands - ring);
        memory(ring + s.put * 8 + 4) = words << 10;
        s.put = (s.put + 1) % ring_entries;
    }
    bool completed(unsigned polls) const
    {
        for(unsigned i = 0; i < polls; ++i)
        {
            if(memory(completion) == s.payload)
            {
                s.polls += i;
                s.longest = i > s.longest ? i : s.longest;
                return true;
            }
            io.delay_us(10);
        }
        return false;
    }
    void kick(unsigned level) const
    {
        const unsigned control = 0x800004 + channel * 8;
        if(level == 1)
            memory(user + 0x8c) = s.put;
        else if(level == 2)
        {
            wr(control, rd(control) | 0x800);
            wr(control, rd(control) | 0x400);
            (void)commit(runlist, 1);
        }
        // The reader must hold a channel, and that channel must be this one.
        else if((rd(0x3080 + s.reader * 4) & 0xe000) == 0x2000 &&
                (rd(reader() + 0x120) & 0xfff) == channel)
            wr(reader(), s.put);
    }
    // Tell the scheduler of the new ring entry and wait for the completion word.
    bool run(unsigned polls)
    {
        end();
        kick(1);
        if(s.kick > 1)
            kick(s.kick);
        if(completed(polls))
        {
            s.kick = s.kick ? s.kick : 1;
            return true;
        }
        if(s.kick)
            return false;
        // The start finds the method one time. No reference driver uses methods 2 and 3.
        for(unsigned level = 2; level <= 3; ++level)
        {
            record(0x100 + level - 1);
            kick(level);
            if(completed(polls))
            {
                s.kick = uint8_t(level);
                return true;
            }
        }
        return false;
    }
};

// Read the device table. Find a copy engine with a run list that the graphics engine does not use.
// A copy engine on the graphics run list needs a graphics context.
bool find_engine(const Copier& c)
{
    struct Device
    {
        uint32_t type = UINT32_MAX, instance = 0;
        int fault = -1, engine = -1, runlist = -1, reset = -1;
    } device, engines[4];
    unsigned count = 0;
    int graphics_runlist = -1;
    uint32_t rows[8];
    for(unsigned i = 0; i < 64; ++i)
    {
        const uint32_t row = c.rd(0x22700 + i * 4);
        rows[i % 8] = row;
        if(i % 8 == 7)
            c.log("gm206 copy devices", rows, 8);
        if(!(row & 3))
            continue;
        if((row & 3) == 1)
        {
            device.instance = (row >> 26) & 0xf;
            if(row & 4)
                device.fault = (row >> 3) & 0x7f;
        }
        else if((row & 3) == 2)
        {
            if(row & 0x20)
                device.engine = (row >> 26) & 0xf;
            if(row & 0x10)
                device.runlist = (row >> 21) & 0xf;
            if(row & 0x04)
                device.reset = (row >> 9) & 0x1f;
        }
        else
            device.type = (row >> 2) & 0x1fffffff;
        // Bit 31 means that the next row is for the same device.
        if(row & 0x80000000)
            continue;
        // Types 1 to 3 are copy engines 0 to 2. Type 0x13 has the instance in its data row.
        if(device.type >= 1 && device.type <= 3)
            device.instance = device.type - 1;
        const bool copy = (device.type >= 1 && device.type <= 3) || device.type == 0x13;
        if(device.type == 0)
            graphics_runlist = device.runlist;
        else if(copy && device.instance <= 2 && device.engine >= 0 && device.runlist >= 0 &&
                device.reset >= 0 && count < 4)
            engines[count++] = device;
        device = {};
    }
    // Nouveau uses the last such engine of the table.
    for(unsigned i = count; i-- > 0;)
        if(engines[i].runlist != graphics_runlist)
        {
            c.s.runlist = uint8_t(engines[i].runlist);
            c.s.engine = uint8_t(engines[i].engine);
            c.s.reset = uint8_t(engines[i].reset);
            c.s.fault = uint8_t(engines[i].fault < 0 ? 0xff : engines[i].fault);
            c.s.instance = uint8_t(engines[i].instance);
            return true;
        }
    return false;
}
}  // namespace

bool copy_start(const ProbeIo& io, volatile uint8_t* video, CopyState& state, CopyLog log)
{
    state = {};
    Copier c{io, video, state, log};
    auto& s = state;
    const auto failed = [&](uint32_t step) {
        c.record(step);
        c.acknowledge();
        const CopyStage stage = s.stage;
        c.stop(stage);
        return false;
    };
    s.stage = CopyStage::Topology;
    const uint32_t enabled = c.rd(0x200);
    const bool engine = find_engine(c);
    {
        const uint32_t values[] = {enabled,
                                   engine,
                                   s.runlist,
                                   s.engine,
                                   s.reset,
                                   s.fault,
                                   s.instance,
                                   c.rd(0x2004),
                                   c.rd(0x2008),
                                   c.rd(0x1704),
                                   c.rd(0x100c80),
                                   c.rd(0x140),
                                   c.rd(0x2100),
                                   c.rd(0x2254)};
        log("gm206 copy engine", values, 14);
    }
    // The channel needs BAR 1 without translation, as the scanout path does.
    if(!engine || (c.rd(0x1704) & 0x80000000))
        return false;

    // The module polls. Enable the scheduler and the engine without a change of other units.
    s.stage = CopyStage::Scheduler;
    c.wr(0x140, 0);
    c.wr(0x144, 0);
    if(!(enabled & 0x100))
        c.wr(0x200, c.rd(0x200) | 0x100);
    if(!(c.rd(0x200) & (1u << s.reset)))
        c.wr(0x200, c.rd(0x200) | (1u << s.reset));
    (void)c.rd(0x200);
    io.delay_us(1000);
    s.readers = uint8_t(c.rd(0x2004) & 0xff);
    s.reader = 0xff;
    for(unsigned i = 0; i < s.readers && i < 8; ++i)
        if(s.reader == 0xff && (c.rd(0x2390 + i * 4) & (1u << s.runlist)))
            s.reader = uint8_t(i);
    {
        const uint32_t values[] = {c.rd(0x200),
                                   s.readers,
                                   s.reader,
                                   c.rd(0x204),
                                   c.rd(0x800000 + channel * 8),
                                   c.rd(0x800004 + channel * 8)};
        log("gm206 copy scheduler", values, 6);
    }
    if(!s.readers || s.readers > 8 || s.reader == 0xff ||
       (c.rd(0x800000 + channel * 8) & 0x80000000))
    {
        s.reader = 0;
        return false;
    }
    c.wr(0x204, (1u << s.readers) - 1);
    for(unsigned i = 0; i < s.readers; ++i)
    {
        const unsigned base = 0x40000 + i * 0x2000;
        c.wr(base + 0x13c, c.rd(base + 0x13c) & ~0x10000100u);
        c.wr(base + 0x108, 0xffffffff);
        c.wr(base + 0x10c, 0xfffffeff);
        c.wr(base + 0x148, 0xffffffff);
        c.wr(base + 0x14c, 0xffffffff);
        c.wr(base + 0x12c, 0x0000ffff);
    }

    // The user area of channel 0 is at the start of the BAR 1 window of the scheduler.
    s.stage = CopyStage::Memory;
    c.wr(0x2254, 0x10000000 | (user >> 12));
    c.wr(0x2100, 0xffffffff);
    c.wr(0x2140, 0);
    c.zero(instance, 0x7000);
    c.zero(directory, directory_bytes);
    c.zero(table, table_bytes);
    // One directory entry with a table of 4 KiB pages.
    c.memory(directory + 4) = (table >> 8) | 1;
    c.map(control_address, ring, control_bytes);
    c.map(scratch_address, scratch, scratch_bytes);
    c.map(surface_address, native_offset, surface_bytes);
    c.memory(instance + 0x200) = directory;
    c.memory(instance + 0x208) = address_limit;
    // Channel state, with the values of Nouveau `gk104_fifo_gpfifo_new_`.
    c.memory(instance + 0x08) = user;
    c.memory(instance + 0x10) = 0x0000face;
    c.memory(instance + 0x30) = 0xfffff902;
    c.memory(instance + 0x48) = control_address;
    c.memory(instance + 0x4c) = ring_order << 16;
    c.memory(instance + 0x84) = 0x20400000;
    c.memory(instance + 0x94) = 0x30000001;
    c.memory(instance + 0x9c) = 0x00000100;
    c.memory(instance + 0xac) = 0x0000001f;
    c.memory(instance + 0xb8) = 0xf8000000;
    c.memory(instance + 0xe4) = 0x00000020;
    c.memory(instance + 0xe8) = channel;
    c.memory(instance + 0xf8) = 0x10003080;
    c.memory(instance + 0xfc) = 0x10000010;
    c.memory(runlist) = channel;
    c.memory(runlist + 4) = instance >> 12;
    if(c.memory(instance + 0x10) != 0x0000face ||
       c.memory(table + (surface_address >> 12) * 8) != ((native_offset >> 8) | 1))
        return failed(1);

    s.stage = CopyStage::Translation;
    if(!c.wait(0x100c80, 0x00ff0000, true))
        return failed(2);
    c.wr(0x100cb8, (directory >> 12) << 4);
    c.wr(0x100cbc, 0x80000001);
    if(!c.wait(0x100c80, 0x00008000, true))
        return failed(3);

    // The first commands are in the ring before the scheduler gets the channel.
    c.begin();
    c.end();
    c.memory(user + 0x8c) = s.put;
    s.stage = CopyStage::Channel;
    const unsigned control = 0x800004 + channel * 8;
    c.wr(control, (c.rd(control) & ~0x000f0000u) | (uint32_t(s.runlist) << 16));
    c.wr(0x800000 + channel * 8, 0x80000000 | (instance >> 12));
    c.wr(control, c.rd(control) | 0x400);
    if(!c.commit(runlist, 1))
        return failed(4);
    c.wr(control, c.rd(control) | 0x400);
    if(c.rd(0x2100) & 1)
        return failed(5);

    // Each kick method gets the same ring position again.
    s.stage = CopyStage::Submit;
    if(!c.completed(10000))
    {
        bool done = false;
        for(unsigned level = 1; level <= 3 && !done; ++level)
        {
            c.record(0x10 + level - 1);
            c.kick(level);
            done = c.completed(10000);
        }
        if(!done)
            return failed(6);
    }

    // Commands without a transfer show which kick method the scheduler accepts.
    s.stage = CopyStage::Kick;
    c.begin();
    if(!c.run(10000))
        return failed(7);

    // A record from the steps before must not count as a failure of the test copy.
    if(c.errors())
    {
        c.record(0x1f);
        c.acknowledge();
    }

    // Copy a pattern between the two halves of the scratch memory, with two different pitches as
    // an overlapping request has them. Try channel addresses first, as Nouveau uses them.
    s.stage = CopyStage::Launch;
    constexpr uint32_t test_width = 64, test_height = 4, half = scratch_bytes / 2;
    constexpr uint32_t test_pitch = test_width * 8;
    for(unsigned attempt = 0; attempt < 2 && !s.ready; ++attempt)
    {
        s.physical = attempt == 1;
        const auto value = [&](uint32_t i) { return 0x5a000000 + i * 0x01010101u + attempt; };
        for(uint32_t i = 0; i < test_width * test_height; ++i)
        {
            c.memory(scratch + i / test_width * test_pitch + i % test_width * 4) = value(i);
            c.memory(scratch + half + i * 4) = 0;
        }
        const uint32_t base = s.physical ? scratch : scratch_address;
        c.begin();
        c.copy(base, base + half, test_pitch, test_width * 4, test_width * 4, test_height);
        // Without a completion, the ring entry stays in use. A second attempt is then not safe.
        if(!c.run(10000))
            return failed(8);
        uint32_t different = 0;
        while(different < test_width * test_height &&
              c.memory(scratch + half + different * 4) == value(different))
            ++different;
        if(different == test_width * test_height && !c.errors())
            s.ready = true;
        else
        {
            const uint32_t values[] = {
                attempt,
                different,
                value(different % (test_width * test_height)),
                c.memory(scratch + half + different % (test_width * test_height) * 4)};
            log("gm206 copy pattern", values, 4);
            c.record(0x20 + attempt);
            c.acknowledge();
        }
    }
    if(!s.ready)
        return failed(9);
    s.stage = CopyStage::Ready;
    // The poll counts of the start are in this line. The later counts are for requests only.
    const uint32_t values[] = {
        s.kick, s.physical, s.runlist, s.reader, c.rd(0x2100), c.rd(0x200), s.polls, s.longest};
    log("gm206 copy ready", values, 8);
    s.polls = s.longest = 0;
    return true;
}

bool copy_area(const ProbeIo& io,
               volatile uint8_t* video,
               CopyState& state,
               CopyLog log,
               const Surface& surface,
               uint32_t source_x,
               uint32_t source_y,
               uint32_t x,
               uint32_t y,
               uint32_t width,
               uint32_t height)
{
    const auto inside = [&](uint32_t left, uint32_t top) {
        return left < surface.width && top < surface.height && width <= surface.width - left &&
               height <= surface.height - top;
    };
    if(!state.ready || surface.offset != native_offset || surface.pitch != surface.width * 4 ||
       !width || !height || height > copy_pixel_limit / width || !inside(source_x, source_y) ||
       !inside(x, y) || uint64_t(surface.pitch) * surface.height > surface_bytes)
        return false;
    Copier c{io, video, state, log};
    const uint32_t first = state.physical ? native_offset : surface_address;
    const uint32_t spare = state.physical ? scratch : scratch_address;
    const uint32_t source = first + source_y * surface.pitch + source_x * 4;
    const uint32_t target = first + y * surface.pitch + x * 4;
    const bool overlap = source_x < x + width && x < source_x + width && source_y < y + height &&
                         y < source_y + height;
    c.begin();
    if(overlap)
    {
        // The order in which the engine copies is not known. Use the scratch memory between.
        c.copy(source, spare, surface.pitch, width * 4, width * 4, height);
        c.copy(spare, target, width * 4, surface.pitch, width * 4, height);
    }
    else
        c.copy(source, target, surface.pitch, surface.pitch, width * 4, height);
    // A completion with an error record can leave a wrong image. The processor then draws the area.
    if(c.run(5000) && !c.errors())
    {
        ++state.copies;
        return true;
    }
    // This code runs with interrupts disabled. Only disable the channel here.
    c.record(0x30);
    c.halt(CopyStage::Stopped);
    return false;
}

void copy_use(const CopyState& state, CopyLog log)
{
    const uint32_t values[] = {uint32_t(state.stage),
                               state.copies,
                               state.polls,
                               state.longest,
                               state.kick,
                               state.physical};
    log("gm206 copy use", values, 6);
}

void copy_stop(const ProbeIo& io, volatile uint8_t* video, CopyState& state, CopyLog log)
{
    copy_use(state, log);
    if(state.halted)
        Copier{io, video, state, log}.stop(CopyStage::Stopped);
}
}  // namespace gm206
