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
constexpr unsigned text_color = 0xd9e4f0;
constexpr uint64_t second_ns = 1'000'000'000;

long session_call(int client, unsigned member, const void* bytes, unsigned size)
{
    os1_ipc_result r{};
    return u::input::call(client, OS1_DISPLAY_SESSION_INTERFACE_ID, member, bytes, size, r);
}

// One display session. The fast requests are FILL_AREA, COPY, and BITMAP.
struct Canvas
{
    int client;
    os1_display_mode mode;
    bool fast;      // The session accepts the fast requests.
    bool hardware;  // The hardware pointer is in use.
};

long paint(Canvas& c, unsigned x, unsigned y, unsigned width, unsigned height, unsigned rgb)
{
    if(!width || !height)
        return 0;
    const unsigned rows = (c.fast ? OS1_DISPLAY_AREA_LIMIT : OS1_DISPLAY_FILL_LIMIT) / width;
    for(unsigned done = 0; done < height;)
    {
        const unsigned count = height - done < rows ? height - done : rows;
        os1_display_fill area{x, y + done, width, count, rgb};
        const long status = session_call(
            c.client, c.fast ? OS1_DISPLAY_FILL_AREA : OS1_DISPLAY_FILL, &area, sizeof(area));
        if(status < 0)
            return status;
        done += count;
    }
    return 0;
}

// Move one rectangle. Only a session with the fast requests can do this.
long copy(Canvas& c,
          unsigned source_x,
          unsigned source_y,
          unsigned x,
          unsigned y,
          unsigned width,
          unsigned height)
{
    if(!c.fast)
        return OS1_ERR_UNSUPPORTED;
    const unsigned rows = OS1_DISPLAY_AREA_LIMIT / width;
    for(unsigned done = 0; done < height;)
    {
        const unsigned count = height - done < rows ? height - done : rows;
        // Start at the bottom for a move down. A later band must not read moved pixels.
        const unsigned first = y > source_y ? height - done - count : done;
        os1_display_copy area{source_x, source_y + first, x, y + first, width, count};
        const long status = session_call(c.client, OS1_DISPLAY_COPY, &area, sizeof(area));
        if(status < 0)
            return status;
        done += count;
    }
    return 0;
}

// Draw characters of 8 x 8 pixels on a solid background.
long text(Canvas& c,
          unsigned x,
          unsigned y,
          const char* characters,
          unsigned count,
          unsigned rgb,
          unsigned back)
{
    constexpr unsigned group = OS1_DISPLAY_BITMAP_BYTES / 8;
    if(!c.fast)
    {
        long status = paint(c, x, y, count * 8, 8, back);
        for(unsigned i = 0; status >= 0 && i < count; ++i)
        {
            const auto& glyph = font8x8_basic[characters[i] & 127];
            for(unsigned row = 0; status >= 0 && row < 8; ++row)
                for(unsigned first = 0; status >= 0 && first < 8;)
                {
                    if(!(glyph[row] & (1u << first)))
                    {
                        ++first;
                        continue;
                    }
                    unsigned end = first + 1;
                    while(end < 8 && (glyph[row] & (1u << end)))
                        ++end;
                    status = paint(c, x + i * 8 + first, y + row, end - first, 1, rgb);
                    first = end;
                }
        }
        return status;
    }
    for(unsigned first = 0; first < count; first += group)
    {
        const unsigned part = count - first < group ? count - first : group;
        os1_display_bitmap bitmap{x + first * 8, y, part * 8, 8, rgb, back, {}};
        for(unsigned row = 0; row < 8; ++row)
            for(unsigned i = 0; i < part; ++i)
                bitmap.bits[row * part + i] = font8x8_basic[characters[first + i] & 127][row];
        const long status = session_call(
            c.client, OS1_DISPLAY_BITMAP, &bitmap, sizeof(bitmap) - sizeof(bitmap.bits) + part * 8);
        if(status < 0)
            return status;
    }
    return 0;
}

long text(Canvas& c, unsigned x, unsigned y, const char* characters, unsigned back)
{
    return text(c, x, y, characters, unsigned(f::length(characters)), text_color, back);
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

// A window that shows the output of shell commands. Each command runs in a new shell process.
// The window waits for the end of the command. A command cannot read from the keyboard.
struct Terminal
{
    static constexpr unsigned max_columns = 80, max_rows = 24, title = 12, border = 4;
    static constexpr unsigned frame_color = 0x4a6d8c, back = 0x0b1420;
    bool open{};
    unsigned x{}, y{}, columns{}, rows{}, row{}, column{};
    char cells[max_rows][max_columns]{};
    char line[max_columns * 2]{};
    unsigned length{};

    unsigned width() const
    {
        return columns * 8 + border * 2;
    }
    unsigned height() const
    {
        return rows * 8 + title + border;
    }
    bool title_hit(unsigned px, unsigned py) const
    {
        return open && px >= x && px < x + width() && py >= y && py < y + title;
    }
    bool overlaps(unsigned ax, unsigned ay, unsigned aw, unsigned ah) const
    {
        return open && ax < x + width() && ax + aw > x && ay < y + height() && ay + ah > y;
    }
    long draw_rows(Canvas& c, unsigned first, unsigned last)
    {
        long status = 0;
        for(unsigned r = first; status >= 0 && r <= last && r < rows; ++r)
            status = text(c, x + border, y + title + r * 8, cells[r], columns, text_color, back);
        return status;
    }
    long draw(Canvas& c)
    {
        long status = paint(c, x, y, width(), title, frame_color);
        if(status >= 0)
            status = text(c, x + border, y + 2, "Shell  |  Esc closes", frame_color);
        if(status >= 0)
            status = paint(c, x, y + title, border, height() - title, frame_color);
        if(status >= 0)
            status =
                paint(c, x + width() - border, y + title, border, height() - title, frame_color);
        if(status >= 0)
            status = paint(c, x, y + height() - border, width(), border, frame_color);
        return status < 0 ? status : draw_rows(c, 0, rows - 1);
    }
    long show(Canvas& c, unsigned top)
    {
        if(c.mode.width < 32 + 20 * 8 || c.mode.height < top + 64 + 4 * 8)
            return OS1_ERR_UNSUPPORTED;
        columns = (c.mode.width - 32) / 8 < max_columns ? (c.mode.width - 32) / 8 : max_columns;
        const unsigned space = (c.mode.height - top - 64) / 8;
        rows = space < max_rows ? space : max_rows;
        x = (c.mode.width - width()) / 2;
        y = top + (c.mode.height - top - height()) / 2;
        for(auto& cell_row : cells)
            for(char& cell : cell_row)
                cell = ' ';
        row = column = length = 0;
        open = true;
        const long status = draw(c);
        return status < 0 ? status : write(c, "$ ", 2);
    }
    long new_line(Canvas& c)
    {
        column = 0;
        if(++row < rows)
            return 0;
        row = rows - 1;
        for(unsigned r = 0; r + 1 < rows; ++r)
            __builtin_memcpy(cells[r], cells[r + 1], max_columns);
        for(char& cell : cells[rows - 1])
            cell = ' ';
        // Move the text up by one row. Without the copy request, draw each row again.
        const long status =
            copy(c, x + border, y + title + 8, x + border, y + title, columns * 8, (rows - 1) * 8);
        if(status == OS1_ERR_UNSUPPORTED)
            return draw_rows(c, 0, rows - 1);
        return status < 0 ? status : draw_rows(c, rows - 1, rows - 1);
    }
    // Add output bytes. Draw each changed row one time.
    long write(Canvas& c, const char* bytes, unsigned count)
    {
        long status = 0;
        bool dirty = false;
        for(unsigned i = 0; status >= 0 && i < count; ++i)
        {
            const char byte = bytes[i];
            if(byte == '\n' || column == columns)
            {
                if(dirty)
                    status = draw_rows(c, row, row);
                dirty = false;
                if(status >= 0)
                    status = new_line(c);
                if(byte == '\n')
                    continue;
            }
            if(byte == '\b')
            {
                if(column)
                    cells[row][--column] = ' ';
                dirty = true;
            }
            else if(byte >= ' ' && byte < 127)
            {
                cells[row][column++] = byte;
                dirty = true;
            }
        }
        return status >= 0 && dirty ? draw_rows(c, row, row) : status;
    }
    // Run the typed line in a shell. The shell reads the line from a file and writes to a file.
    long run(Canvas& c)
    {
        static char output[4096];
        f::Handle script(f::temporary()), capture(f::temporary());
        os1_resource_info input{}, errors{};
        if(script.fd < 0 || capture.fd < 0 || u::query(0, &input) < 0 || u::query(2, &errors) < 0 ||
           !f::put(script.fd, line, length) || !f::put(script.fd, "\n", 1) ||
           u::seek(script.fd, 0) < 0)
            return write(c, "gsh: cannot prepare the command\n", 32);
        f::Handle saved_input(u::duplicate(0, input.rights)),
            saved_errors(u::duplicate(2, errors.rights));
        long pid = OS1_ERR_ACCESS;
        if(saved_input.fd >= 0 && saved_errors.fd >= 0)
        {
            // The child gets the script as standard input and the capture file as both outputs.
            const bool swapped = u::duplicate(script.fd, u::fs::file_rights, 0, 0) == 0 &&
                                 u::duplicate(capture.fd, u::fs::file_rights, 0, 2) == 2;
            const char* arguments[] = {"sh"};
            if(swapped)
                pid = u::run_program("/bin/sh",
                                     1,
                                     arguments,
                                     false,
                                     0,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     -1,
                                     nullptr,
                                     capture.fd);
            u::duplicate(saved_input.fd, input.rights, input.descriptor_flags, 0);
            u::duplicate(saved_errors.fd, errors.rights, errors.descriptor_flags, 2);
        }
        int exit_status = 0;
        if(pid < 0 || u::waitpid(pid, &exit_status) != pid)
            return write(c, "gsh: cannot run the shell\n", 26);
        long status = u::seek(capture.fd, 0) < 0 ? OS1_ERR_IO : 0;
        for(long n = 0; status >= 0 && (n = u::read(capture.fd, output, sizeof(output))) > 0;)
            status = write(c, output, unsigned(n));
        if(status >= 0 && column)
            status = write(c, "\n", 1);
        return status;
    }
    long key(Canvas& c, unsigned byte)
    {
        if(byte == '\n' || byte == '\r')
        {
            long status = write(c, "\n", 1);
            if(status >= 0 && length)
                status = run(c);
            length = 0;
            return status < 0 ? status : write(c, "$ ", 2);
        }
        if(byte == '\b' || byte == 127)
        {
            if(!length)
                return 0;
            --length;
            return write(c, "\b", 1);
        }
        if(byte < ' ' || byte > 126 || length == sizeof(line) || column + 1 >= columns)
            return 0;
        line[length++] = char(byte);
        const char typed = char(byte);
        return write(c, &typed, 1);
    }
};

struct Desktop
{
    Canvas& c;
    bool desktop;
    unsigned top;
    Terminal terminal{};

    unsigned clock_x() const
    {
        return c.mode.width - 200;
    }
    unsigned clock_y() const
    {
        return c.mode.height - 20;
    }
    long draw_clock()
    {
        char time[24]{};
        uint64_t now;
        if(u::clock::now(now) == 0)
            u::clock::format(time, now, false);
        else
            __builtin_memcpy(time, "Clock unavailable      ", 24);
        // The window can cover the clock.
        if(terminal.overlaps(clock_x(), clock_y(), 184, 8))
            return 0;
        return text(c, clock_x(), clock_y(), time, 23, text_color, background);
    }
    long draw_header()
    {
        long status = paint(c, 0, 0, c.mode.width, top, header_color);
        const char* label = "os1  |  R Reboot  S Shut down  T Shell  M Mode  C Pointer";
        if(status >= 0 && c.mode.width >= 24 + f::length(label) * 8)
            status = text(c, 12, 12, label, header_color);
        else if(status >= 0)
            status = text(c, 12, 12, "os1  |  R Reboot  S Shut down", header_color);
        return status;
    }
    // Draw one area of the desktop again. The software pointer uses this to remove itself.
    // A window move draws the window itself and passes false.
    long repaint(unsigned x, unsigned y, unsigned width, unsigned height, bool window = true)
    {
        long status = 0;
        if(y < top)
            status = draw_header();
        if(status >= 0 && y + height > top)
        {
            const unsigned first = y < top ? top : y;
            status = paint(c, x, first, width, y + height - first, background);
        }
        if(status >= 0 && x + width > clock_x() && y + height > clock_y() && y < clock_y() + 8)
            status = draw_clock();
        if(status >= 0 && window && terminal.overlaps(x, y, width, height))
            status = terminal.draw(c);
        return status;
    }
    long draw_all()
    {
        long status = paint(c, 0, top, c.mode.width, c.mode.height - top, background);
        if(status >= 0 && top)
            status = draw_header();
        if(status >= 0)
            status = draw_clock();
        return status;
    }
    // Move the window. Copy its pixels, then draw the area that the window left.
    long move_window(unsigned x, unsigned y)
    {
        const unsigned old_x = terminal.x, old_y = terminal.y;
        const unsigned width = terminal.width(), height = terminal.height();
        if(x == old_x && y == old_y)
            return 0;
        long status = copy(c, old_x, old_y, x, y, width, height);
        const bool copied = status >= 0;
        if(status == OS1_ERR_UNSUPPORTED)
            status = 0;
        // The clock test in the repaint uses the new window position.
        terminal.x = x;
        terminal.y = y;
        const unsigned shift_x = x > old_x ? x - old_x : old_x - x;
        const unsigned shift_y = y > old_y ? y - old_y : old_y - y;
        if(status >= 0 && (!copied || shift_x >= width || shift_y >= height))
            status = repaint(old_x, old_y, width, height, false);
        else if(status >= 0)
        {
            // The left area is one strip beside the window and one strip above or below it.
            if(shift_x)
                status = repaint(x > old_x ? old_x : x + width, old_y, shift_x, height, false);
            if(status >= 0 && shift_y)
                status = repaint(old_x, y > old_y ? old_y : y + height, width, shift_y, false);
        }
        if(status >= 0 && !copied)
            status = terminal.draw(c);
        // Show the clock again after the window leaves it.
        return status < 0 ? status : draw_clock();
    }
};

// Show the pointer with its tip at (x, y). The hardware pointer needs no drawing.
long show_pointer(Desktop& d, unsigned x, unsigned y, unsigned color)
{
    Canvas& c = d.c;
    if(c.hardware)
    {
        os1_display_cursor cursor{x, y, 1};
        if(session_call(c.client, OS1_DISPLAY_CURSOR, &cursor, sizeof(cursor)) >= 0)
            return 0;
        c.hardware = false;
    }
    return draw_cursor(c.client, x, y, color, c.mode.width, c.mode.height);
}

long hide_pointer(Desktop& d, unsigned x, unsigned y)
{
    Canvas& c = d.c;
    if(c.hardware)
        return 0;
    return d.repaint(x,
                     y,
                     c.mode.width - x < cursor_width ? c.mode.width - x : cursor_width,
                     c.mode.height - y < cursor_height ? c.mode.height - y : cursor_height);
}

// Return 0 to end the session, 1 to open the next mode, or an error.
long interactive(Canvas& c,
                 int keys,
                 int key_client,
                 int pointer,
                 const PointerInput& input,
                 PointerStats& stats,
                 bool trace,
                 bool desktop)
{
    const auto& mode = c.mode;
    const int client = c.client;
    os1_ipc_result r{};
    // A session without the fast requests refuses this fill. The old fill request then draws.
    os1_display_fill probe{0, 0, 1, 1, background};
    c.fast = session_call(client, OS1_DISPLAY_FILL_AREA, &probe, sizeof(probe)) >= 0;
    Desktop d{c, desktop, desktop ? desktop_header : 0};
    const unsigned top = d.top;
    long status = d.draw_all();
    if(status < 0)
        return status;
    auto previous = input.state;
    if(!pointer_state(pointer, previous) && !desktop)
        return OS1_ERR_CLOSED;
    stats.input_id = previous.device;
    unsigned x = (mode.width - 1) / 2, y = (mode.height - top - 1) / 2;
    // Use the hardware pointer if the session has one.
    c.hardware = true;
    status = show_pointer(d, x, top + y, previous.buttons ? 0xffcb54 : 0xffffff);
    if(status < 0)
        return status;
    if(desktop)
    {
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
    bool dragging = false;
    unsigned grab_x = 0, grab_y = 0;
    for(;;)
    {
        const long now = u::ipc_clock();
        if(!next_clock || (now >= 0 && uint64_t(now) >= next_clock))
        {
            status = d.draw_clock();
            // The clock text can cover the drawn pointer.
            if(status >= 0 && !c.hardware && next_clock)
                status = show_pointer(d, x, top + y, previous.buttons ? 0xffcb54 : 0xffffff);
            if(status < 0)
                return status;
            next_clock = now < 0 || uint64_t(now) > INT64_MAX - second_ns
                             ? UINT64_MAX
                             : uint64_t(now) + second_ns;
        }
        long ready = u::ipc_wait_many(waits, wait_count, next_clock);
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
            const unsigned letter = key >= 'A' && key <= 'Z' ? key + 32 : key;
            if(d.terminal.open)
            {
                // The window has the keyboard. Esc closes it.
                if(key == 27)
                {
                    d.terminal.open = false;
                    status = d.repaint(
                        d.terminal.x, d.terminal.y, d.terminal.width(), d.terminal.height());
                    dragging = false;
                }
                else
                    status = d.terminal.key(c, key);
                if(status >= 0 && !c.hardware)
                    status = show_pointer(d, x, top + y, previous.buttons ? 0xffcb54 : 0xffffff);
                if(status < 0)
                    return status;
                continue;
            }
            if(letter == 't')
            {
                status = hide_pointer(d, x, top + y);
                if(status >= 0)
                    status = d.terminal.show(c, top);
                if(status == OS1_ERR_UNSUPPORTED)
                    status = 0;
                if(status >= 0)
                    status = show_pointer(d, x, top + y, previous.buttons ? 0xffcb54 : 0xffffff);
                if(status < 0)
                    return status;
            }
            else if(letter == 'm' && !trace)
                return 1;
            else if(letter == 'c')
            {
                // Change between the hardware pointer and the drawn pointer.
                if(c.hardware)
                {
                    os1_display_cursor hidden{x, top + y, 0};
                    session_call(client, OS1_DISPLAY_CURSOR, &hidden, sizeof(hidden));
                    c.hardware = false;
                }
                else
                {
                    status = hide_pointer(d, x, top + y);
                    c.hardware = true;
                }
                if(status >= 0)
                    status = show_pointer(d, x, top + y, previous.buttons ? 0xffcb54 : 0xffffff);
                if(status < 0)
                    return status;
            }
            else if(desktop)
            {
                if(letter == 'r' || letter == 's')
                {
                    status =
                        u::system_control(letter == 'r' ? OS1_SYSTEM_REBOOT : OS1_SYSTEM_SHUTDOWN);
                    if(status < 0)
                    {
                        status = text(c, 12, 24, "System command failed", header_color);
                        if(status < 0)
                            return status;
                    }
                }
            }
            else if(key == 27 || letter == 'q' || key == 3)
                return 0;
            continue;
        }
        auto next = previous;
        unsigned nx = x, ny = y;
        // The position at the time of a button press. A batch can contain later movement.
        bool pressed = false;
        unsigned press_x = 0, press_y = 0;
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
            if(event.buttons && !next.buttons && !pressed)
            {
                pressed = true;
                press_x = nx;
                press_y = ny;
            }
            next.buttons = event.buttons;
        }
        const bool changed = nx != x || ny != y || next.buttons != previous.buttons;
        previous = next;
        if(!changed)
            continue;
        os1_measurement before{}, after{};
        const bool measured = !u::ipc_measure(before) && before.source == OS1_MEASUREMENT_HPET;
        status = hide_pointer(d, x, top + y);
        if(status < 0)
            return status;
        // A press on the title starts a window move. The window follows the pointer.
        if(pressed && d.terminal.title_hit(press_x, top + press_y))
        {
            dragging = true;
            grab_x = press_x - d.terminal.x;
            grab_y = top + press_y - d.terminal.y;
        }
        if(!next.buttons)
            dragging = false;
        if(dragging && d.terminal.open)
        {
            const unsigned limit_x = mode.width - d.terminal.width();
            const unsigned limit_y = mode.height - d.terminal.height();
            unsigned wx = nx > grab_x ? nx - grab_x : 0,
                     wy = top + ny > grab_y ? top + ny - grab_y : 0;
            wx = wx > limit_x ? limit_x : wx;
            wy = wy < top ? top : wy > limit_y ? limit_y : wy;
            status = d.move_window(wx, wy);
            if(status < 0)
                return status;
        }
        x = nx;
        y = ny;
        status = show_pointer(d, x, top + y, next.buttons ? 0xffcb54 : 0xffffff);
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
        auto mode = preferred_mode(modes + 1, count - 1);
        // The M key opens the next mode. The boot mode is not a native mode.
        bool changed = false;
        for(unsigned refused = 0;;)
        {
            f::Handle session(display_call(
                display.fd, OS1_DISPLAY_OPEN_NATIVE, &mode.id, sizeof(mode.id), r, true));
            // After a mode change, a mode that does not open is not the end. Open the next one.
            if(session.fd < 0 && (!changed || session.fd != OS1_ERR_IO || ++refused >= count))
                return session.fd;
            status = 1;
            if(session.fd >= 0)
            {
                changed = true;
                f::Handle client(bind_display(session.fd, OS1_DISPLAY_SESSION_INTERFACE_ID));
                if(client.fd < 0)
                    return client.fd;
                refused = 0;
                PointerStats stats{};
                Canvas canvas{client.fd, mode, false, false};
                status = interactive(
                    canvas, keys.fd, key_client.fd, pointer_client.fd, input, stats, false, true);
            }
            if(status != 1)
                return status;
            unsigned index = 1;
            while(index < count && modes[index].id != mode.id)
                ++index;
            mode = modes[index + 1 < count ? index + 1 : 1];
        }
    }
    // Write one mode record. The text has no line end.
    auto mode_text = [&](char* out, unsigned index, bool marker) {
        const auto& mode = modes[index];
        unsigned at = 0;
        auto add = [&](const char* part) {
            while(*part)
                out[at++] = *part++;
        };
        auto number = [&](unsigned value) {
            char digits[10];
            unsigned n = 0;
            do
                digits[n++] = char('0' + value % 10);
            while(value /= 10);
            while(n)
                out[at++] = digits[--n];
        };
        if(marker)
            add("> ");
        number(index + 1);
        add(". ");
        number(mode.width);
        add(" x ");
        number(mode.height);
        add(", ");
        number(mode.bits_per_pixel);
        add(index ? " bpp" : " bpp (boot mode)");
        out[at] = 0;
        return at;
    };
    char record[64];
    print(menu, "Video modes\n");
    for(unsigned i = 0; i < count; ++i)
    {
        mode_text(record, i, false);
        print(menu, record);
        print(menu, "\n");
    }
    print(menu, "Choose a mode. Up/Down selects. Enter applies. Esc or q exits.\n");
    unsigned selected = 0;
    // Draw the selection on one console row. Without the redraw request, write a new line.
    f::Handle writer(u::console_bind(menu, OS1_RIGHT_WRITE));
    auto highlight = [&] {
        const unsigned bytes = mode_text(record, selected, true);
        if(writer.fd < 0 || u::console_redraw_line(writer.fd, 0, record, bytes) < 0)
        {
            print(menu, record);
            print(menu, "\n");
        }
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
    print(menu, "\n");
    PointerInput input;
    status = input.prepare(pointer_client.fd);
    if(status < 0)
        return status;
    PointerStats stats{};
    // The M key opens the next mode in the list.
    unsigned refused = 0;
    bool changed = false;
    for(status = 1; status == 1; selected = selected + 1 == count ? 0 : selected + 1)
    {
        const auto& mode = modes[selected];
        f::Handle session(
            display_call(display.fd, OS1_DISPLAY_OPEN, &mode.id, sizeof(mode.id), r, true));
        // After a mode change, a mode that does not open is not the end. Open the next one.
        if(session.fd == OS1_ERR_IO && changed && ++refused < count)
            continue;
        if(session.fd < 0)
            return session.fd;
        refused = 0;
        changed = true;
        f::Handle client(bind_display(session.fd, OS1_DISPLAY_SESSION_INTERFACE_ID));
        if(client.fd < 0)
            return client.fd;
        Canvas canvas{client.fd, mode, false, false};
        status = interactive(
            canvas, keys.fd, key_client.fd, pointer_client.fd, input, stats, menu == 2, false);
    }
    os1_measurement end{};
    if(!u::ipc_measure(end) && end.source == OS1_MEASUREMENT_HPET)
        stats.end_ns = end.nanoseconds;
    if(stats.trace_ticket)
    {
        long stopped = u::system_control(OS1_SYSTEM_EHCI_TRACE_STOP, stats.trace_ticket);
        if(stopped < 0 && !status)
            status = stopped;
    }
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
