#include <os1/display.h>

#include <os1/console.hpp>
#include <os1/file_tools.hpp>
#include <os1/input.hpp>

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

long interactive(int client, const os1_display_mode& mode, int keys, int key_client, int pointer)
{
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
    unsigned x = (mode.width - cursor_size) / 2, y = (mode.height - cursor_size) / 2;
    for(;;)
    {
        long status = draw_cursor(client, x, y, previous.buttons ? 0xffcb54 : 0xffffff);
        if(status < 0)
            return status;
        for(;;)
        {
            unsigned key = 0;
            long now = u::ipc_clock();
            if(now < 0)
                return now;
            status = u::console_read_key(keys, key_client, key, uint64_t(now) + 32000000);
            if(status == 0 && (key == 27 || key == 'q' || key == 'Q' || key == 3))
                return 0;
            if(status < 0 && status != OS1_ERR_TIMED_OUT)
                return status;
            os1_input_state current{};
            if(!pointer_state(pointer, current))
                return OS1_ERR_CLOSED;
            if(current.x == previous.x && current.y == previous.y &&
               current.buttons == previous.buttons)
                continue;
            status = fill(client, x, y, cursor_size, cursor_size, background);
            if(status < 0)
                return status;
            x = move(x, int64_t(current.x - previous.x), mode.width - cursor_size);
            y = move(y, int64_t(current.y - previous.y), mode.height - cursor_size);
            previous = current;
            break;
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

long run(bool check)
{
    f::Handle display(bind_display(u::root(), OS1_DISPLAY_INTERFACE_ID));
    if(display.fd < 0)
        return display.fd;
    os1_ipc_result r{};
    long status = display_call(display.fd, OS1_DISPLAY_MODE, nullptr, 0, r);
    if(status < 0)
        return status;
    os1_display_mode mode{};
    if(r.payload_bytes != sizeof(mode))
        return OS1_ERR_INVALID;
    __builtin_memcpy(&mode, r.payload, sizeof(mode));
    if(mode.width < cursor_size || mode.height < cursor_size || mode.width > OS1_DISPLAY_FILL_LIMIT)
        return OS1_ERR_INVALID;
    if(check)
        return check_session(display.fd, mode);
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
    print(1, "Video modes\n1. ");
    f::number(1, mode.width);
    print(1, " x ");
    f::number(1, mode.height);
    print(1, " x ");
    f::number(1, mode.bits_per_pixel);
    print(1, " (firmware)\nPress 1 or Enter to start. Esc or q exits.\n");
    for(;;)
    {
        unsigned key = 0;
        status = u::console_read_key(keys.fd, key_client.fd, key);
        if(status < 0)
            return status;
        if(key == 'q' || key == 'Q' || key == 27 || key == 3)
            return 0;
        if(key == '1' || key == '\r' || key == '\n')
            break;
    }
    f::Handle session(
        display_call(display.fd, OS1_DISPLAY_OPEN, &mode.id, sizeof(mode.id), r, true));
    if(session.fd < 0)
        return session.fd;
    f::Handle client(bind_display(session.fd, OS1_DISPLAY_SESSION_INTERFACE_ID));
    if(client.fd < 0)
        return client.fd;
    return interactive(client.fd, mode, keys.fd, key_client.fd, pointer_client.fd);
}
}  // namespace

int main(int argc, char** argv)
{
    const bool check = argc == 2 && equal(argv[1], "--check");
    if(argc != 1 && !check)
    {
        print(2, "usage: gfxdemo [--check]\n");
        return 1;
    }
    long status = run(check);
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
