#include <font8x8_basic.h>
#include <os1/display.h>

#include <os1/clock.hpp>
#include <os1/console.hpp>
#include <os1/file_tools.hpp>
#include <os1/input.hpp>
#include <os1/system.hpp>

namespace u = os1::user;
namespace f = u::files;

namespace
{
constexpr unsigned background = 0x14283e;
// Arrow pointer. 'X' is the outline and '.' is the body. The tip is the top-left pixel.
constexpr const char* cursor_rows[] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.........X ",
    "X......XXXXX",
    "X...X..X    ",
    "X..XX..X    ",
    "X.X  X..X   ",
    "XX   X..X   ",
    "X     XX    ",
};
constexpr unsigned cursor_width = 12;
constexpr unsigned cursor_height = sizeof(cursor_rows) / sizeof(*cursor_rows);
constexpr unsigned cursor_outline = 0x000000;

void print(int fd, const char* text)
{
    f::put(fd, text, f::length(text));
}

bool equal(const char* left, const char* right)
{
    while(*left && *left == *right)
    {
        ++left;
        ++right;
    }
    return *left == *right;
}

long bind_display(int source, uint64_t id)
{
    os1_ipc_kernel_binding binding{1, sizeof(binding), id, 1, OS1_RIGHT_DISPLAY};
    return u::ipc(OS1_IPC_BIND_KERNEL, source, &binding);
}

long display_call(int fd,
                  unsigned member,
                  const void* bytes,
                  unsigned size,
                  os1_ipc_result& r,
                  bool owned = false)
{
    return u::input::call(fd, OS1_DISPLAY_INTERFACE_ID, member, bytes, size, r, owned);
}

long fill(int client, unsigned x, unsigned y, unsigned width, unsigned height, unsigned rgb)
{
    os1_display_fill area{x, y, width, height, rgb};
    os1_ipc_result r{};
    return u::input::call(
        client, OS1_DISPLAY_SESSION_INTERFACE_ID, OS1_DISPLAY_FILL, &area, sizeof(area), r);
}

long find_pointer()
{
    f::Handle root(u::input::bind(u::root(), OS1_INPUT_INTERFACE_ID));
    if(root.fd < 0)
        return root.fd;
    os1_input_selector selector{}, fallback{};
    for(unsigned i = 0; i < OS1_INPUT_DEVICE_LIMIT; ++i)
    {
        os1_ipc_result r{};
        long status = u::input::call(
            root.fd, OS1_INPUT_INTERFACE_ID, OS1_INPUT_LIST, &selector, sizeof(selector), r);
        if(status < 0)
            return status;
        if(!r.payload_bytes)
            break;
        os1_input_device info{};
        if(r.payload_bytes != sizeof(info))
            return OS1_ERR_INVALID;
        __builtin_memcpy(&info, r.payload, sizeof(info));
        selector.device = info.device;
        if(info.kind != OS1_INPUT_POINTER)
            continue;
        if(info.catalog_id)
            return u::input::call(root.fd,
                                  OS1_INPUT_INTERFACE_ID,
                                  OS1_INPUT_OPEN,
                                  &selector,
                                  sizeof(selector),
                                  r,
                                  true);
        if(!fallback.device)
            fallback = selector;
    }
    if(!fallback.device)
        return OS1_ERR_NOT_FOUND;
    os1_ipc_result r{};
    return u::input::call(
        root.fd, OS1_INPUT_INTERFACE_ID, OS1_INPUT_OPEN, &fallback, sizeof(fallback), r, true);
}

bool pointer_state(int client, os1_input_state& state)
{
    os1_ipc_result r{};
    if(u::input::call(client, OS1_POINTER_INTERFACE_ID, OS1_INPUT_GET_STATE, nullptr, 0, r) ||
       r.payload_bytes != sizeof(state))
        return false;
    __builtin_memcpy(&state, r.payload, sizeof(state));
    return state.connected;
}

unsigned move(unsigned position, int64_t delta, unsigned limit)
{
    if(delta > int64_t(limit - position))
        return limit;
    if(delta < -int64_t(position))
        return 0;
    return unsigned(int64_t(position) + delta);
}

// Draw the pointer with its tip at (x, y). Clip it at the right and bottom limits.
long draw_cursor(
    int client, unsigned x, unsigned y, unsigned color, unsigned right, unsigned bottom)
{
    const unsigned width = right - x < cursor_width ? right - x : cursor_width;
    // Draw each run of equal pixels in a row with one fill.
    for(unsigned row = 0; row < cursor_height && y + row < bottom; ++row)
    {
        const char* pixels = cursor_rows[row];
        for(unsigned start = 0, end; start < width; start = end)
        {
            for(end = start + 1; end < width && pixels[end] == pixels[start]; ++end)
            {
            }
            if(pixels[start] == ' ')
                continue;
            const unsigned rgb = pixels[start] == '.' ? color : cursor_outline;
            const long status = fill(client, x + start, y + row, end - start, 1, rgb);
            if(status < 0)
                return status;
        }
    }
    return 0;
}

struct PointerStats
{
    uint64_t events{}, losses{}, gaps{}, delayed{}, slow_draws{};
    uint64_t previous_event{}, gap_max{}, age_max{}, draw_max{};
    uint64_t gap_samples{}, age_samples{}, draw_samples{};
    uint64_t input_id{}, start_ns{}, end_ns{}, trace_ticket{};

    void event(const os1_input_event& e)
    {
        if(trace_ticket && start_ns && !(e.flags & OS1_INPUT_COARSE_CLOCK) && e.time_ns < start_ns)
            return;
        ++events;
        if(e.flags & OS1_INPUT_COARSE_CLOCK)
            return;
        if(previous_event && e.time_ns >= previous_event)
        {
            ++gap_samples;
            sample(e.time_ns - previous_event, gap_max, gaps);
        }
        previous_event = e.time_ns;
        os1_measurement now{};
        if(!u::ipc_measure(now) && now.source == OS1_MEASUREMENT_HPET &&
           now.nanoseconds >= e.time_ns)
        {
            ++age_samples;
            sample(now.nanoseconds - e.time_ns, age_max, delayed);
        }
    }

    static void sample(uint64_t ns, uint64_t& maximum, uint64_t& count)
    {
        if(ns > maximum)
            maximum = ns;
        if(ns >= 50000000)
            ++count;
    }

    void report() const
    {
        print(1, "gsh: pointer diagnostics");
        const char* labels[] = {" events=",
                                " lost_records=",
                                " gaps_ge_50ms=",
                                " age_ge_50ms=",
                                " draws_ge_50ms=",
                                " gap_max_ns=",
                                " age_max_ns=",
                                " draw_max_ns=",
                                " gap_samples=",
                                " age_samples=",
                                " draw_samples=",
                                " input_id=",
                                " start_hpet_ns=",
                                " end_hpet_ns=",
                                " trace_ticket="};
        const uint64_t values[] = {events,
                                   losses,
                                   gaps,
                                   delayed,
                                   slow_draws,
                                   gap_max,
                                   age_max,
                                   draw_max,
                                   gap_samples,
                                   age_samples,
                                   draw_samples,
                                   input_id,
                                   start_ns,
                                   end_ns,
                                   trace_ticket};
        for(unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        {
            print(1, labels[i]);
            f::number(1, values[i]);
        }
        print(1, "\n");
    }
};

constexpr unsigned desktop_header = 32;
constexpr unsigned header_color = 0x223d57;
constexpr uint64_t second_ns = 1'000'000'000;

long draw_text(int client, unsigned x, unsigned y, const char* text)
{
    for(; *text; ++text, x += 8)
    {
        const auto& glyph = font8x8_basic[static_cast<unsigned char>(*text)];
        for(unsigned row = 0; row < 8; ++row)
            for(unsigned first = 0; first < 8;)
            {
                if(!(glyph[row] & (1u << first)))
                {
                    ++first;
                    continue;
                }
                unsigned end = first + 1;
                while(end < 8 && (glyph[row] & (1u << end)))
                    ++end;
                const long status = fill(client, x + first, y + row, end - first, 1, 0xd9e4f0);
                if(status < 0)
                    return status;
                first = end;
            }
    }
    return 0;
}

long draw_clock(int client, unsigned width)
{
    char time[24]{};
    uint64_t now;
    if(u::clock::now(now) == 0)
        u::clock::format(time, now, false);
    else
        __builtin_memcpy(time, "Clock unavailable", sizeof("Clock unavailable"));
    const unsigned x = width - 200;
    const long status = fill(client, x, 12, 192, 8, header_color);
    return status < 0 ? status : draw_text(client, x, 12, time);
}

bool read_literal(const char*& cursor, const char* literal)
{
    for(; *literal; ++literal, ++cursor)
        if(*cursor != *literal)
            return false;
    return true;
}

bool read_dimension(const char*& cursor, unsigned& dimension)
{
    if(*cursor < '1' || *cursor > '9')
        return false;
    unsigned value = 0;
    while(*cursor >= '0' && *cursor <= '9')
    {
        value = value * 10 + unsigned(*cursor++ - '0');
        if(value > OS1_DISPLAY_FILL_LIMIT)
            return false;
    }
    dimension = value;
    return true;
}

os1_display_mode preferred_mode(const os1_display_mode* modes, unsigned count)
{
    const auto fallback = modes[0];
    f::Handle config(u::open("/etc/gsh.conf", OS1_RIGHT_READ));
    if(config.fd < 0)
        return fallback;
    char text[64]{};
    size_t used = 0;
    while(used < sizeof(text) - 1)
    {
        const long n = u::read(config.fd, text + used, sizeof(text) - 1 - used);
        if(n < 0)
            return fallback;
        if(!n)
            break;
        used += n;
    }
    if(used == sizeof(text) - 1 || f::length(text) != used)
        return fallback;
    const char* cursor = text;
    unsigned width, height;
    if(read_literal(cursor, "version=1\nwidth=") && read_dimension(cursor, width) &&
       read_literal(cursor, "\nheight=") && read_dimension(cursor, height) &&
       read_literal(cursor, "\n") && !*cursor)
        for(unsigned i = 0; i < count; ++i)
            if(modes[i].width == width && modes[i].height == height)
                return modes[i];
    return fallback;
}

struct PointerInput
{
    f::Handle subscription, reader;
    os1_input_state state{};

    long prepare(int pointer)
    {
        os1_ipc_result result{};
        subscription.reset(u::input::call(
            pointer, OS1_POINTER_INTERFACE_ID, OS1_INPUT_SUBSCRIBE, nullptr, 0, result, true));
        if(subscription.fd < 0)
            return subscription.fd;
        reader.reset(u::input::bind(subscription.fd, OS1_INPUT_SUBSCRIPTION_INTERFACE_ID));
        if(reader.fd < 0)
            return reader.fd;
        return pointer_state(pointer, state) ? 0 : OS1_ERR_CLOSED;
    }
};

long interactive(int client,
                 const os1_display_mode& mode,
                 int keys,
                 int key_client,
                 int pointer,
                 const PointerInput& input,
                 PointerStats& stats,
                 bool trace,
                 bool desktop)
{
    os1_ipc_result r{};
    const unsigned rows = OS1_DISPLAY_FILL_LIMIT / mode.width;
    for(unsigned y = 0; y < mode.height; y += rows)
    {
        const unsigned height = mode.height - y < rows ? mode.height - y : rows;
        long status = fill(client, 0, y, mode.width, height, background);
        if(status < 0)
            return status;
    }
    if(desktop)
    {
        long status = 0;
        for(unsigned y = 0; status >= 0 && y < desktop_header;)
        {
            const unsigned height = desktop_header - y < rows ? desktop_header - y : rows;
            status = fill(client, 0, y, mode.width, height, header_color);
            y += height;
        }
        if(status < 0)
            return status;
        status = draw_text(client, 12, 12, "os1  |  R Reboot  S Shut down");
        if(status < 0)
            return status;
    }
    const unsigned top = desktop ? desktop_header : 0;
    auto previous = input.state;
    if(!pointer_state(pointer, previous) && !desktop)
        return OS1_ERR_CLOSED;
    stats.input_id = previous.device;
    unsigned x = (mode.width - 1) / 2, y = (mode.height - top - 1) / 2;
    long status = draw_cursor(
        client, x, top + y, previous.buttons ? 0xffcb54 : 0xffffff, mode.width, mode.height);
    if(status < 0)
        return status;
    if(desktop)
    {
        status = draw_clock(client, mode.width);
        if(status < 0)
            return status;
        status = u::input::call(
            client, OS1_DISPLAY_SESSION_INTERFACE_ID, OS1_DISPLAY_COMMIT, nullptr, 0, r);
        if(status < 0)
            return status;
        print(1, "gsh: native display acquired\n");
    }
    if(trace)
    {
        const long ticket = u::system_control(OS1_SYSTEM_EHCI_TRACE_START, previous.device);
        if(ticket <= 0)
            return ticket < 0 ? ticket : OS1_ERR_INVALID;
        stats.trace_ticket = ticket;
    }
    os1_measurement start{};
    if(!u::ipc_measure(start) && start.source == OS1_MEASUREMENT_HPET)
        stats.start_ns = start.nanoseconds;
    const int waits[] = {keys, input.subscription.fd};
    unsigned wait_count = 2;
    uint64_t next_clock = 0;
    for(;;)
    {
        const long now = desktop ? u::ipc_clock() : -1;
        if(desktop && (!next_clock || (now >= 0 && uint64_t(now) >= next_clock)))
        {
            status = draw_clock(client, mode.width);
            if(status < 0)
                return status;
            next_clock = now < 0 || uint64_t(now) > INT64_MAX - second_ns
                             ? UINT64_MAX
                             : uint64_t(now) + second_ns;
        }
        long ready = u::ipc_wait_many(waits, wait_count, desktop ? next_clock : UINT64_MAX);
        if(ready == OS1_ERR_TIMED_OUT)
            continue;
        if(ready < 0)
            return ready;
        if(!ready)
        {
            unsigned key = 0;
            status = u::console_read_key(keys, key_client, key);
            if(status < 0)
                return status;
            if(desktop)
            {
                if(key == 'r' || key == 'R' || key == 's' || key == 'S')
                {
                    const bool reboot = key == 'r' || key == 'R';
                    status = u::system_control(reboot ? OS1_SYSTEM_REBOOT : OS1_SYSTEM_SHUTDOWN);
                    if(status < 0)
                    {
                        status = draw_text(client, 12, 24, "System command failed");
                        if(status < 0)
                            return status;
                    }
                }
            }
            else if(key == 27 || key == 'q' || key == 'Q' || key == 3)
                return 0;
            continue;
        }
        auto next = previous;
        unsigned nx = x, ny = y;
        for(unsigned i = 0; i < 32; ++i)
        {
            status = u::input::call(input.reader.fd,
                                    OS1_INPUT_SUBSCRIPTION_INTERFACE_ID,
                                    OS1_INPUT_TRY_READ,
                                    nullptr,
                                    0,
                                    r);
            if(status == OS1_ERR_WOULD_BLOCK)
                break;
            if(desktop && status == OS1_ERR_CLOSED)
            {
                wait_count = 1;
                break;
            }
            if(status < 0 || r.payload_bytes != sizeof(os1_input_event))
                return status < 0 ? status : OS1_ERR_INVALID;
            os1_input_event event{};
            __builtin_memcpy(&event, r.payload, sizeof(event));
            if(event.flags & OS1_INPUT_REMOVED)
            {
                if(!desktop)
                    return OS1_ERR_CLOSED;
                wait_count = 1;
                break;
            }
            if(event.flags & OS1_INPUT_LOST)
            {
                stats.losses += event.lost;
                auto snapshot = next;
                if(!pointer_state(pointer, snapshot))
                {
                    if(!desktop)
                        return OS1_ERR_CLOSED;
                    wait_count = 1;
                    break;
                }
                nx = move(nx, int64_t(snapshot.x - next.x), mode.width - 1);
                ny = move(ny, int64_t(snapshot.y - next.y), mode.height - top - 1);
                next = snapshot;
                continue;
            }
            if(event.sequence <= next.sequence)
                continue;
            stats.event(event);
            nx = move(nx, event.dx, mode.width - 1);
            ny = move(ny, event.dy, mode.height - top - 1);
            next.sequence = event.sequence;
            next.x += uint64_t(int64_t(event.dx));
            next.y += uint64_t(int64_t(event.dy));
            next.buttons = event.buttons;
        }
        const bool changed = nx != x || ny != y || next.buttons != previous.buttons;
        previous = next;
        if(!changed)
            continue;
        os1_measurement before{}, after{};
        const bool measured = !u::ipc_measure(before) && before.source == OS1_MEASUREMENT_HPET;
        // Erase the visible part of the pointer.
        status = fill(client,
                      x,
                      top + y,
                      mode.width - x < cursor_width ? mode.width - x : cursor_width,
                      mode.height - top - y < cursor_height ? mode.height - top - y : cursor_height,
                      background);
        if(status < 0)
            return status;
        x = nx;
        y = ny;
        status = draw_cursor(
            client, x, top + y, next.buttons ? 0xffcb54 : 0xffffff, mode.width, mode.height);
        if(status < 0)
            return status;
        if(measured && !u::ipc_measure(after) && after.source == before.source &&
           after.nanoseconds >= before.nanoseconds)
        {
            ++stats.draw_samples;
            PointerStats::sample(
                after.nanoseconds - before.nanoseconds, stats.draw_max, stats.slow_draws);
        }
    }
}

long check_session(int display, const os1_display_mode& mode)
{
    os1_ipc_result r{};
    uint32_t invalid = 0;
    if(display_call(display, OS1_DISPLAY_OPEN, &invalid, sizeof(invalid), r, true) !=
       OS1_ERR_INVALID)
        return OS1_ERR_INVALID;
    for(unsigned pass = 0; pass < 2; ++pass)
    {
        os1_measurement start{}, opened{}, filling{}, drawn{}, released{};
        bool measured = u::ipc_measure(start) == 0 && start.source != OS1_MEASUREMENT_UNAVAILABLE;
        f::Handle session(
            display_call(display, OS1_DISPLAY_OPEN, &mode.id, sizeof(mode.id), r, true));
        if(session.fd < 0)
            return session.fd;
        measured = u::ipc_measure(opened) == 0 && measured;
        f::Handle client(bind_display(session.fd, OS1_DISPLAY_SESSION_INTERFACE_ID));
        if(client.fd < 0)
            return client.fd;
        session.reset();  // The bound client alone must retain the exclusive session.
        if(display_call(display, OS1_DISPLAY_OPEN, &mode.id, sizeof(mode.id), r, true) !=
               OS1_ERR_WOULD_BLOCK ||
           fill(client.fd, mode.width, 0, 1, 1, 0) != OS1_ERR_INVALID)
            return OS1_ERR_INVALID;
        measured = u::ipc_measure(filling) == 0 && measured;
        if(fill(client.fd, 0, 0, 1, 1, 0xffcb54))
            return OS1_ERR_INVALID;
        measured = u::ipc_measure(drawn) == 0 && measured;
        client.reset();
        measured = u::ipc_measure(released) == 0 && measured;
        if(measured && opened.nanoseconds >= start.nanoseconds &&
           drawn.nanoseconds >= filling.nanoseconds && released.nanoseconds >= drawn.nanoseconds)
        {
            print(1, "gsh: ns acquire=");
            f::number(1, opened.nanoseconds - start.nanoseconds);
            print(1, " fill=");
            f::number(1, drawn.nanoseconds - filling.nanoseconds);
            print(1, " restore=");
            f::number(1, released.nanoseconds - drawn.nanoseconds);
            print(1, "\n");
        }
    }
    if(mode.id != OS1_DISPLAY_FIXED_MODE)
    {
        // An uncommitted native session must leave the diagnostic path usable.
        f::Handle session(
            display_call(display, OS1_DISPLAY_OPEN_NATIVE, &mode.id, sizeof(mode.id), r, true));
        if(session.fd < 0)
            return session.fd;
        session.reset();
        session.reset(display_call(display, OS1_DISPLAY_OPEN, &mode.id, sizeof(mode.id), r, true));
        if(session.fd < 0)
            return session.fd;
    }
    return 0;
}

long run(bool check, int menu)
{
    f::Handle display(bind_display(u::root(), OS1_DISPLAY_INTERFACE_ID));
    if(display.fd < 0)
        return display.fd;
    os1_ipc_result r{};
    constexpr unsigned mode_limit = 64;
    os1_display_mode modes[mode_limit]{};
    unsigned count = 0;
    long status = 0;
    for(;;)
    {
        const uint32_t index = count;
        status = display_call(display.fd, OS1_DISPLAY_MODE_AT, &index, sizeof(index), r);
        if(status < 0)
            return status;
        if(!r.payload_bytes)
            break;
        if(count == mode_limit || r.payload_bytes != sizeof(os1_display_mode))
            return OS1_ERR_LIMIT;
        auto& mode = modes[count++];
        __builtin_memcpy(&mode, r.payload, sizeof(mode));
        if(!mode.id || mode.width < cursor_width || mode.height < cursor_height ||
           mode.width > OS1_DISPLAY_FILL_LIMIT ||
           (mode.bits_per_pixel != 24 && mode.bits_per_pixel != 32))
            return OS1_ERR_INVALID;
        for(unsigned i = 0; i + 1 < count; ++i)
            if(modes[i].id == mode.id)
                return OS1_ERR_INVALID;
        if(check)
        {
            status = check_session(display.fd, mode);
            if(status < 0)
                return status;
        }
    }
    if(!count || modes[0].id != OS1_DISPLAY_FIXED_MODE)
        return OS1_ERR_INVALID;
    if(check)
    {
        const uint32_t invalid_index = count + 1;
        if(display_call(
               display.fd, OS1_DISPLAY_MODE_AT, &invalid_index, sizeof(invalid_index), r) !=
               OS1_ERR_INVALID ||
           display_call(display.fd, OS1_DISPLAY_MODE_AT, nullptr, 0, r) != OS1_ERR_INVALID)
            return OS1_ERR_INVALID;
        status = display_call(display.fd, OS1_DISPLAY_LIST_MODES, nullptr, 0, r);
        if(status < 0 || r.payload_bytes != sizeof(os1_display_modes))
            return OS1_ERR_INVALID;
        os1_display_modes legacy{};
        __builtin_memcpy(&legacy, r.payload, sizeof(legacy));
        const unsigned legacy_count =
            count < OS1_DISPLAY_MODE_LIMIT ? count : OS1_DISPLAY_MODE_LIMIT;
        if(legacy.count != legacy_count)
            return OS1_ERR_INVALID;
        for(unsigned i = 0; i < legacy_count; ++i)
            if(legacy.modes[i].id != modes[i].id || legacy.modes[i].width != modes[i].width ||
               legacy.modes[i].height != modes[i].height ||
               legacy.modes[i].bits_per_pixel != modes[i].bits_per_pixel)
                return OS1_ERR_INVALID;
        return 0;
    }
    if(!menu && count < 2)
        return OS1_ERR_UNSUPPORTED;
    f::Handle pointer(find_pointer());
    if(pointer.fd < 0)
        return pointer.fd;
    f::Handle pointer_client(u::input::bind(pointer.fd, OS1_POINTER_INTERFACE_ID));
    f::Handle console(u::console_bind(0, OS1_RIGHT_READ));
    if(console.fd < 0 || pointer_client.fd < 0)
        return OS1_ERR_ACCESS;
    f::Handle keys(u::console_acquire_input(console.fd, true));
    if(keys.fd < 0)
        return keys.fd;
    f::Handle key_client(u::console_input_bind(keys.fd));
    if(key_client.fd < 0)
        return key_client.fd;
    if(!menu)
    {
        PointerInput input;
        status = input.prepare(pointer_client.fd);
        if(status < 0)
            return status;
        const auto mode = preferred_mode(modes + 1, count - 1);
        f::Handle session(
            display_call(display.fd, OS1_DISPLAY_OPEN_NATIVE, &mode.id, sizeof(mode.id), r, true));
        if(session.fd < 0)
            return session.fd;
        f::Handle client(bind_display(session.fd, OS1_DISPLAY_SESSION_INTERFACE_ID));
        if(client.fd < 0)
            return client.fd;
        PointerStats stats{};
        return interactive(
            client.fd, mode, keys.fd, key_client.fd, pointer_client.fd, input, stats, false, true);
    }
    auto print_mode = [&](unsigned index) {
        const auto& mode = modes[index];
        f::number(menu, index + 1);
        print(menu, ". ");
        f::number(menu, mode.width);
        print(menu, " x ");
        f::number(menu, mode.height);
        print(menu, ", ");
        f::number(menu, mode.bits_per_pixel);
        print(menu, index ? " bpp\n" : " bpp (boot mode)\n");
    };
    print(menu, "Video modes\n");
    for(unsigned i = 0; i < count; ++i)
        print_mode(i);
    print(menu, "Choose a mode. Up/Down selects. Enter applies. Esc or q exits.\n");
    unsigned selected = 0;
    auto highlight = [&] {
        print(menu, "> ");
        print_mode(selected);
    };
    highlight();
    for(;;)
    {
        unsigned key = 0;
        status = u::console_read_key(keys.fd, key_client.fd, key);
        if(status < 0)
            return status;
        if(key == 27)
        {
            // Serial consoles send escape sequences instead of native arrow key codes.
            const long now = u::ipc_clock();
            if(now < 0)
                return now;
            const auto deadline = uint64_t(now) + 50'000'000;
            status = u::console_read_key(keys.fd, key_client.fd, key, deadline);
            if(status == OS1_ERR_TIMED_OUT)
                return 0;
            if(status < 0)
                return status;
            if(key != '[' && key != 'O')
                return 0;
            status = u::console_read_key(keys.fd, key_client.fd, key, deadline);
            if(status == OS1_ERR_TIMED_OUT)
                continue;
            if(status < 0)
                return status;
            key = key == 'A' ? OS1_CONSOLE_KEY_UP : key == 'B' ? OS1_CONSOLE_KEY_DOWN : 0;
        }
        if(key == 'q' || key == 'Q' || key == 3)
            return 0;
        if(key == '\r' || key == '\n')
            break;
        if(key == OS1_CONSOLE_KEY_UP)
            selected = selected ? selected - 1 : count - 1;
        else if(key == OS1_CONSOLE_KEY_DOWN)
            selected = selected + 1 == count ? 0 : selected + 1;
        else
            continue;
        highlight();
    }
    const auto& mode = modes[selected];
    PointerInput input;
    status = input.prepare(pointer_client.fd);
    if(status < 0)
        return status;
    f::Handle session(
        display_call(display.fd, OS1_DISPLAY_OPEN, &mode.id, sizeof(mode.id), r, true));
    if(session.fd < 0)
        return session.fd;
    f::Handle client(bind_display(session.fd, OS1_DISPLAY_SESSION_INTERFACE_ID));
    if(client.fd < 0)
        return client.fd;
    PointerStats stats{};
    status = interactive(
        client.fd, mode, keys.fd, key_client.fd, pointer_client.fd, input, stats, menu == 2, false);
    os1_measurement end{};
    if(!u::ipc_measure(end) && end.source == OS1_MEASUREMENT_HPET)
        stats.end_ns = end.nanoseconds;
    if(stats.trace_ticket)
    {
        long stopped = u::system_control(OS1_SYSTEM_EHCI_TRACE_STOP, stats.trace_ticket);
        if(stopped < 0 && !status)
            status = stopped;
    }
    client.reset();
    session.reset();
    stats.report();
    return status;
}
}  // namespace

int main(int argc, char** argv)
{
    const bool check = argc == 2 && equal(argv[1], "--check");
    const bool report = argc == 2 && equal(argv[1], "--stall-report");
    const bool menu = argc == 2 && equal(argv[1], "--menu");
    if(argc != 1 && !check && !report && !menu)
    {
        print(2, "usage: gsh [--check | --menu | --stall-report]\n");
        return 1;
    }
    long status = run(check, report ? 2 : (menu ? 1 : 0));
    if(status < 0)
    {
        print(2, "gsh: ");
        print(2,
              status == OS1_ERR_UNSUPPORTED ? "no compatible graphics driver\n"
                                            : "display or input operation failed\n");
        return 1;
    }
    if(check)
        print(1, "gsh: session checks passed\n");
    return 0;
}
