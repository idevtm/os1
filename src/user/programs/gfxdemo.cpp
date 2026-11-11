#include <os1/display.h>

#include <os1/console.hpp>
#include <os1/file_tools.hpp>
#include <os1/input.hpp>
#include <os1/system.hpp>

namespace u = os1::user;
namespace f = u::files;

namespace
{
constexpr unsigned background = 0x14283e;
constexpr unsigned cursor_size = 9;

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

long draw_cursor(int client, unsigned x, unsigned y, unsigned color)
{
    long status = fill(client, x + 3, y, 3, cursor_size, color);
    return status < 0 ? status : fill(client, x, y + 3, cursor_size, 3, color);
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
        print(1, "gfxdemo: pointer diagnostics");
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

long interactive(int client,
                 const os1_display_mode& mode,
                 int keys,
                 int key_client,
                 int pointer,
                 PointerStats& stats,
                 bool trace)
{
    os1_ipc_result r{};
    f::Handle subscription(u::input::call(
        pointer, OS1_POINTER_INTERFACE_ID, OS1_INPUT_SUBSCRIBE, nullptr, 0, r, true));
    if(subscription.fd < 0)
        return subscription.fd;
    f::Handle reader(u::input::bind(subscription.fd, OS1_INPUT_SUBSCRIPTION_INTERFACE_ID));
    if(reader.fd < 0)
        return reader.fd;
    const unsigned rows = OS1_DISPLAY_FILL_LIMIT / mode.width;
    for(unsigned y = 0; y < mode.height; y += rows)
    {
        const unsigned height = mode.height - y < rows ? mode.height - y : rows;
        long status = fill(client, 0, y, mode.width, height, background);
        if(status < 0)
            return status;
    }
    os1_input_state previous{};
    if(!pointer_state(pointer, previous))
        return OS1_ERR_CLOSED;
    stats.input_id = previous.device;
    unsigned x = (mode.width - cursor_size) / 2, y = (mode.height - cursor_size) / 2;
    long status = draw_cursor(client, x, y, previous.buttons ? 0xffcb54 : 0xffffff);
    if(status < 0)
        return status;
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
    const int waits[] = {keys, subscription.fd};
    for(;;)
    {
        long ready = u::ipc_wait_many(waits, 2);
        if(ready < 0)
            return ready;
        if(!ready)
        {
            unsigned key = 0;
            status = u::console_read_key(keys, key_client, key);
            if(status < 0)
                return status;
            if(key == 27 || key == 'q' || key == 'Q' || key == 3)
                return 0;
            continue;
        }
        auto next = previous;
        unsigned nx = x, ny = y;
        for(unsigned i = 0; i < 32; ++i)
        {
            status = u::input::call(
                reader.fd, OS1_INPUT_SUBSCRIPTION_INTERFACE_ID, OS1_INPUT_TRY_READ, nullptr, 0, r);
            if(status == OS1_ERR_WOULD_BLOCK)
                break;
            if(status < 0 || r.payload_bytes != sizeof(os1_input_event))
                return status < 0 ? status : OS1_ERR_INVALID;
            os1_input_event event{};
            __builtin_memcpy(&event, r.payload, sizeof(event));
            if(event.flags & OS1_INPUT_REMOVED)
                return OS1_ERR_CLOSED;
            if(event.flags & OS1_INPUT_LOST)
            {
                stats.losses += event.lost;
                auto snapshot = next;
                if(!pointer_state(pointer, snapshot))
                    return OS1_ERR_CLOSED;
                nx = move(nx, int64_t(snapshot.x - next.x), mode.width - cursor_size);
                ny = move(ny, int64_t(snapshot.y - next.y), mode.height - cursor_size);
                next = snapshot;
                continue;
            }
            if(event.sequence <= next.sequence)
                continue;
            stats.event(event);
            nx = move(nx, event.dx, mode.width - cursor_size);
            ny = move(ny, event.dy, mode.height - cursor_size);
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
        status = fill(client, x, y, cursor_size, cursor_size, background);
        if(status < 0)
            return status;
        x = nx;
        y = ny;
        status = draw_cursor(client, x, y, next.buttons ? 0xffcb54 : 0xffffff);
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
            print(1, "gfxdemo: ns acquire=");
            f::number(1, opened.nanoseconds - start.nanoseconds);
            print(1, " fill=");
            f::number(1, drawn.nanoseconds - filling.nanoseconds);
            print(1, " restore=");
            f::number(1, released.nanoseconds - drawn.nanoseconds);
            print(1, "\n");
        }
    }
    return 0;
}

long run(bool check, int menu)
{
    f::Handle display(bind_display(u::root(), OS1_DISPLAY_INTERFACE_ID));
    if(display.fd < 0)
        return display.fd;
    os1_ipc_result r{};
    long status = display_call(display.fd, OS1_DISPLAY_LIST_MODES, nullptr, 0, r);
    if(status < 0)
        return status;
    os1_display_modes modes{};
    if(r.payload_bytes != sizeof(modes))
        return OS1_ERR_INVALID;
    __builtin_memcpy(&modes, r.payload, sizeof(modes));
    if(!modes.count || modes.count > OS1_DISPLAY_MODE_LIMIT)
        return OS1_ERR_INVALID;
    for(unsigned i = 0; i < modes.count; ++i)
    {
        const auto& mode = modes.modes[i];
        if(mode.id != i + 1 || mode.width < cursor_size || mode.height < cursor_size ||
           mode.width > OS1_DISPLAY_FILL_LIMIT)
            return OS1_ERR_INVALID;
        if(check)
        {
            status = check_session(display.fd, mode);
            if(status < 0)
                return status;
        }
    }
    if(check)
        return 0;
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
    print(menu, "Video modes\n");
    for(unsigned i = 0; i < modes.count; ++i)
    {
        const auto& mode = modes.modes[i];
        f::number(menu, i + 1);
        print(menu, ". ");
        f::number(menu, mode.width);
        print(menu, " x ");
        f::number(menu, mode.height);
        print(menu, " x ");
        f::number(menu, mode.bits_per_pixel);
        print(menu, i ? "\n" : " (boot mode)\n");
    }
    print(menu, "Choose a mode. Enter uses the boot mode. Esc or q exits.\n");
    unsigned selected = 0;
    for(;;)
    {
        unsigned key = 0;
        status = u::console_read_key(keys.fd, key_client.fd, key);
        if(status < 0)
            return status;
        if(key == 'q' || key == 'Q' || key == 27 || key == 3)
            return 0;
        if(key == '\r' || key == '\n')
            break;
        if(key >= '1' && key < '1' + modes.count)
        {
            selected = key - '1';
            break;
        }
    }
    const auto& mode = modes.modes[selected];
    f::Handle session(
        display_call(display.fd, OS1_DISPLAY_OPEN, &mode.id, sizeof(mode.id), r, true));
    if(session.fd < 0)
        return session.fd;
    f::Handle client(bind_display(session.fd, OS1_DISPLAY_SESSION_INTERFACE_ID));
    if(client.fd < 0)
        return client.fd;
    PointerStats stats{};
    status =
        interactive(client.fd, mode, keys.fd, key_client.fd, pointer_client.fd, stats, menu == 2);
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
    if(argc != 1 && !check && !report)
    {
        print(2, "usage: gfxdemo [--check | --stall-report]\n");
        return 1;
    }
    long status = run(check, report ? 2 : 1);
    if(status < 0)
    {
        print(2, "gfxdemo: ");
        print(2,
              status == OS1_ERR_UNSUPPORTED ? "no compatible graphics driver\n"
                                            : "display or input operation failed\n");
        return 1;
    }
    if(check)
        print(1, "gfxdemo: session checks passed\n");
    return 0;
}
