#pragma once
#include <os1/console.hpp>
#include <os1/discovery.hpp>
#include <os1/file_tools.hpp>

#include "discovery/completion.hpp"
namespace os1::user::prompt
{
inline os1::discovery::View view;
struct ReleaseInput
{
    int client;
    ~ReleaseInput()
    {
        os1_ipc_result result{};
        console_call(client, OS1_CONSOLE_INPUT_INTERFACE_ID, OS1_CONSOLE_INPUT_RELEASE, result);
    }
};
inline bool output(int fd, const char* text)
{
    return files::put(fd, text, files::length(text));
}
inline bool describe(int fd, const os1::discovery::Member& member)
{
    return output(fd, member.name) && output(fd, " [") &&
           output(fd, os1::discovery::kind_name(member.kind)) &&
           output(fd, member.flags & os1::discovery::Native ? ", native] " : "] ") &&
           output(fd, member.signature) && output(fd, "\n");
}
inline long inspect(const char* path, int fd)
{
    if(!discovery::refresh(view, true))
        return OS1_ERR_LIMIT;
    int id = view.resolve(path);
    if(id < 0)
        return OS1_ERR_NOT_FOUND;
    if(!output(fd, "context ") || !output(fd, *path ? path : "<root>") || !output(fd, "\n"))
        return OS1_ERR_FAULT;
    if(!view.container(id))
        return describe(fd, view.nodes[id]) ? 0 : OS1_ERR_FAULT;
    os1::discovery::Cursor cursor{};
    os1::discovery::Page page{};
    do
    {
        long r = view.enumerate(id, cursor, page);
        if(r < 0)
            return r;
        for(unsigned i = 0; i < page.count; ++i)
            if(!describe(fd, view.nodes[page.ids[i]]))
                return OS1_ERR_FAULT;
    } while(!page.end);
    return 0;
}
inline bool display(const os1::discovery::Completion& state)
{
    if(!output(2, "\n"))
        return false;
    if(state.open)
    {
        if(!output(2, "context choices (Tab explore, Enter accept, Esc cancel)\n"))
            return false;
        unsigned first = state.selected / os1::discovery::page_limit * os1::discovery::page_limit;
        for(unsigned i = first; i < state.matches && i < first + os1::discovery::page_limit; ++i)
            if(!output(2, i == state.selected ? "> " : "  ") ||
               !describe(2, view.nodes[state.ids[i]]))
                return false;
    }
    else if(state.detail && state.leaf >= 0)
    {
        if(!describe(2, view.nodes[state.leaf]))
            return false;
        if((view.nodes[state.leaf].flags & os1::discovery::Native) &&
           !(view.nodes[state.leaf].flags & os1::discovery::Shell) &&
           !output(2,
                   "Metadata only in this shell; invoke native members through the typed API.\n"))
            return false;
    }
    return output(2, "  > ") && output(2, state.line);
}
inline long read(files::Reader& fallback, files::Line& line)
{
    files::Handle input(console_bind(0, OS1_RIGHT_READ));
    os1_resource_info output_info{};
    files::Handle session;
    if(input.fd >= 0 && query(2, &output_info) >= 0 && output_info.kind == OS1_OBJECT_CONSOLE)
        session.reset(console_acquire_input(input.fd, true));
    if(session.fd < 0)
    {
        if(!output(2, "os1> "))
            return OS1_ERR_FAULT;
        return fallback.line(line);
    }
    files::Handle keys(console_input_bind(session.fd));
    if(keys.fd < 0)
        return keys.fd;
    ReleaseInput release{keys.fd};
    if(!output(2, "os1> "))
        return OS1_ERR_FAULT;
    os1::discovery::Completion state;
    bool overflow = false, skip_lf = false;
    for(;;)
    {
        unsigned key = 0;
        long r = console_read_key(session.fd, keys.fd, key);
        if(r == OS1_ERR_RANGE)
        {
            overflow = true;
            state.cancel();
            continue;
        }
        if(r < 0)
            return r;
        // A serial CRLF pair represents one Enter, including menu acceptance.
        if(skip_lf && key == '\n')
        {
            skip_lf = false;
            continue;
        }
        skip_lf = key == '\r';
        if(key == 27)
        {
            // A lone Escape cancels after a bounded serial-sequence interval.
            long clock = ipc_clock();
            r = clock < 0 ? clock
                          : console_read_key(session.fd, keys.fd, key, uint64_t(clock) + 50000000);
            if(r == OS1_ERR_TIMED_OUT)
            {
                state.cancel();
                if(!display(state))
                    return OS1_ERR_FAULT;
                continue;
            }
            if(r < 0)
                return r;
            if(key == '[' || key == 'O')
            {
                // Consume a complete bounded CSI/SS3 sequence, including ignored keys.
                // Otherwise Delete (ESC [ 3 ~) would insert its trailing tilde.
                unsigned consumed = 0;
                do
                {
                    r = console_read_key(session.fd, keys.fd, key, uint64_t(clock) + 50000000);
                } while(r >= 0 && (key < 0x40 || key > 0x7e) && ++consumed < 16);
                if(r == OS1_ERR_TIMED_OUT)
                {
                    state.cancel();
                    continue;
                }
                if(r < 0)
                    return r;
                if(key == 'A')
                    key = OS1_CONSOLE_KEY_UP;
                else if(key == 'B')
                    key = OS1_CONSOLE_KEY_DOWN;
                else
                {
                    state.cancel();
                    continue;
                }
            }
            else
                state.cancel();
        }
        if(key == 3)
        {
            state = {};
            overflow = false;
            line = {};
            output(2, "^C\n");
            return 0;
        }
        if(key == '\r' || key == '\n' || key == 4)
        {
            if(state.open && !overflow && key != 4)
            {
                state.accept(view, false);
                if(!display(state))
                    return OS1_ERR_FAULT;
                continue;
            }
            if(!output(2, "\n"))
                return OS1_ERR_FAULT;
            line = {};
            if(overflow)
                return OS1_ERR_RANGE;
            for(unsigned i = 0; i < state.size; ++i)
                line.bytes[i] = state.line[i];
            line.size = state.size;
            line.eof = key == 4;
            return 0;
        }
        if(overflow)
            continue;
        if(key == '\t')
        {
            if(state.open)
                state.accept(view, true);
            else if(discovery::refresh(view, true))
                state.begin(view);
            if(!display(state))
                return OS1_ERR_FAULT;
        }
        else if(key == OS1_CONSOLE_KEY_UP || key == OS1_CONSOLE_KEY_DOWN)
        {
            if(state.open)
            {
                state.move(key == OS1_CONSOLE_KEY_UP ? -1 : 1);
                if(!display(state))
                    return OS1_ERR_FAULT;
            }
        }
        else if(key == '\b' || key == 127)
        {
            bool menu = state.open || state.detail;
            if(state.size)
            {
                state.backspace();
                if(menu)
                {
                    if(!display(state))
                        return OS1_ERR_FAULT;
                }
                else if(!output(2, "\b \b"))
                    return OS1_ERR_FAULT;
            }
        }
        else if(key >= 32 && key < 127)
        {
            bool menu = state.open || state.detail;
            if(!state.insert(static_cast<char>(key)))
                overflow = true;
            else if(menu)
            {
                if(!display(state))
                    return OS1_ERR_FAULT;
            }
            else
            {
                char byte = key;
                if(!files::put(2, &byte, 1))
                    return OS1_ERR_FAULT;
            }
        }
    }
}
}  // namespace os1::user::prompt
