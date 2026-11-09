#pragma once
#include <os1/console.hpp>
#include <os1/discovery.hpp>
#include <os1/file_tools.hpp>
#include <os1/log.hpp>

#include "discovery/presentation.hpp"
namespace os1::user::prompt
{
inline os1::discovery::View view;
inline char retained_line[os1::shell::line_capacity]{};
inline bool last_guided = false;
inline void retain(const char* line)
{
    os1::discovery::copy(retained_line, sizeof(retained_line), line);
}
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
        return describe(fd, view.member(id)) ? 0 : OS1_ERR_FAULT;
    os1::discovery::Cursor cursor{};
    os1::discovery::Page page{};
    do
    {
        long r = view.enumerate(id, cursor, page);
        if(r < 0)
            return r;
        for(unsigned i = 0; i < page.count; ++i)
            if(!describe(fd, view.member(page.ids[i])))
                return OS1_ERR_FAULT;
    } while(!page.end);
    return 0;
}
struct Display
{
    files::Handle client;
    os1::discovery::Presentation presentation;
    Display() : client(console_bind(2, OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT))
    {
        os1_console_geometry geometry{};
        if(client.fd < 0 || console_geometry(client.fd, geometry) < 0 || geometry.columns < 8 ||
           geometry.rows < 3)
            client.reset();
        else
            presentation.configure(geometry.columns, geometry.rows);
    }
    bool advance()
    {
        return output(2, "\n");
    }
    bool row(int delta, const char* text, unsigned bytes)
    {
        if(client.fd >= 0)
            return console_redraw_line(client.fd, delta, text, bytes) >= 0;
        return advance() && files::put(2, text, bytes);
    }
    bool cursor(unsigned column)
    {
        if(client.fd < 0)
            return true;
        auto request = console_request(OS1_CONSOLE_SET_CURSOR);
        request.payload_bytes = sizeof(uint32_t);
        uint32_t c = column;
        __builtin_memcpy(request.payload, &c, sizeof(c));
        os1_ipc_result result{};
        long r = ipc_call(client.fd, request, result);
        return r >= 0 && !result.transport_status && !result.application_status;
    }
    bool paint(const os1::discovery::Completion& state)
    {
        return presentation.paint(view, state, *this);
    }
    bool cancel(os1::discovery::Completion& state)
    {
        state.cancel();
        state.focus = -1;
        return !presentation.reserved || paint(state);
    }
};
struct History
{
    char draft[os1::shell::line_capacity]{};
    unsigned draft_cursor = 0;
    uint64_t selected = UINT64_MAX;
    bool move(os1::discovery::Completion& state, int direction)
    {
        if(selected == UINT64_MAX && direction > 0)
            return false;
        if(selected == UINT64_MAX)
        {
            __builtin_memcpy(draft, state.line, state.size + 1);
            draft_cursor = state.cursor;
        }
        os1_log_history_query query{};
        query.query.version = 1;
        query.query.size = sizeof(query.query);
        query.query.min_level = 1;
        uint64_t best = direction < 0 ? 0 : UINT64_MAX;
        char candidate[os1::shell::line_capacity]{};
        for(unsigned i = 0; i < 32; ++i)
        {
            if(logs::context(OS1_LOG_HISTORY_READ, &query) < 0)
                break;
            const auto& r = query.result;
            uint64_t sequence = r.record.sequence;
            if(sequence <= query.query.after)
                break;
            query.query.after = sequence;
            if(!(r.record.flags & OS1_LOG_INTERACTIVE) ||
               (r.record.flags & (OS1_LOG_SUPPRESSED | OS1_LOG_PENDING)) || !r.text[0])
                continue;
            bool match = direction < 0 ? (sequence < selected && sequence > best)
                                       : (sequence > selected && sequence < best);
            if(match)
            {
                best = sequence;
                __builtin_memcpy(candidate, r.text, sizeof(candidate));
                candidate[sizeof(candidate) - 1] = 0;
            }
        }
        if(direction < 0 && !best)
            return false;
        if(direction > 0 && best == UINT64_MAX)
        {
            selected = UINT64_MAX;
            os1::discovery::copy(state.line, sizeof(state.line), draft);
            state.size = files::length(state.line);
            state.cursor = draft_cursor;
        }
        else
        {
            selected = best;
            os1::discovery::copy(state.line, sizeof(state.line), candidate);
            state.size = files::length(state.line);
            state.cursor = state.size;
        }
        state.cancel();
        state.focus = -1;
        return true;
    }
};
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
    Display display;
    if(!output(2, "os1> "))
        return OS1_ERR_FAULT;
    os1::discovery::Completion state;
    os1::discovery::PresentationCleanup<Display> cleanup{
        view, state, display.presentation, display};
    History history;
    bool overflow = false, skip_lf = false;
    if(!discovery::refresh(view, true))
        return OS1_ERR_LIMIT;
    if(retained_line[0])
    {
        os1::discovery::copy(state.line, sizeof(state.line), retained_line);
        retained_line[0] = 0;
        state.size = files::length(state.line);
        state.cursor = state.size;
        state.guided = true;
        state.scan_fields(view);
        for(unsigned i = 0; i < state.field_count; ++i)
            if(((state.fields[i].end - state.fields[i].begin == 3 &&
                 state.line[state.fields[i].begin] == '<') ||
                (state.fields[i].end - state.fields[i].begin == 5 &&
                 state.line[state.fields[i].begin + 1] == '<')))
            {
                state.focus_field(i);
                break;
            }
        if(!display.paint(state))
            return OS1_ERR_FAULT;
    }

    for(;;)
    {
        unsigned key = 0;
        long r = console_read_key(session.fd, keys.fd, key);
        if(r == OS1_ERR_RANGE)
        {
            overflow = true;
            if(!display.cancel(state))
                return OS1_ERR_FAULT;
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
                if(!display.cancel(state))
                    return OS1_ERR_FAULT;
                continue;
            }
            if(r < 0)
                return r;
            if(key == '[' || key == 'O')
            {
                // Consume a complete bounded CSI/SS3 sequence, including ignored keys.
                // Otherwise Delete (ESC [ 3 ~) would insert its trailing tilde.
                unsigned consumed = 0, parameter = 0;
                do
                {
                    r = console_read_key(session.fd, keys.fd, key, uint64_t(clock) + 50000000);
                    if(r >= 0 && key >= '0' && key <= '9')
                        parameter = parameter * 10 + key - '0';
                } while(r >= 0 && (key < 0x40 || key > 0x7e) && ++consumed < 16);
                if(r == OS1_ERR_TIMED_OUT)
                {
                    if(!display.cancel(state))
                        return OS1_ERR_FAULT;
                    continue;
                }
                if(r < 0)
                    return r;
                if(key == 'A')
                    key = OS1_CONSOLE_KEY_UP;
                else if(key == 'B')
                    key = OS1_CONSOLE_KEY_DOWN;
                else if(key == 'C')
                    key = OS1_CONSOLE_KEY_RIGHT;
                else if(key == 'D')
                    key = OS1_CONSOLE_KEY_LEFT;
                else if(key == 'H' || (key == '~' && parameter == 1))
                    key = OS1_CONSOLE_KEY_HOME;
                else if(key == 'F' || (key == '~' && parameter == 4))
                    key = OS1_CONSOLE_KEY_END;
                else if(key == 'Z')
                    key = OS1_CONSOLE_KEY_BACKTAB;
                else if(key == '~' && parameter == 3)
                    key = OS1_CONSOLE_KEY_DELETE;
                else
                {
                    if(!display.cancel(state))
                        return OS1_ERR_FAULT;
                    continue;
                }
            }
            else if(!display.cancel(state))
                return OS1_ERR_FAULT;
        }
        if(key == 3)
        {
            if(!display.cancel(state))
                return OS1_ERR_FAULT;
            line = {};
            last_guided = false;
            return output(2, "^C\n") ? 0 : OS1_ERR_FAULT;
        }
        if(key == '\r' || key == '\n' || key == 4)
        {
            if(state.open && !overflow && key != 4)
            {
                state.accept(view, false);
                if(!display.paint(state))
                    return OS1_ERR_FAULT;
                continue;
            }
            if(!display.cancel(state))
                return OS1_ERR_FAULT;
            if(!output(2, "\n"))
                return OS1_ERR_FAULT;
            line = {};
            if(overflow)
                return OS1_ERR_RANGE;
            for(unsigned i = 0; i < state.size; ++i)
                line.bytes[i] = state.line[i];
            line.size = state.size;
            last_guided = state.guided;
            line.eof = key == 4;
            return 0;
        }
        if(overflow)
            continue;
        if(key == '\t' || key == OS1_CONSOLE_KEY_BACKTAB)
        {
            if(state.open)
                state.accept(view, true);
            else if(!state.step_field(view, key == OS1_CONSOLE_KEY_BACKTAB ? -1 : 1) && key == '\t')
                state.begin(view);
            if(!display.paint(state))
                return OS1_ERR_FAULT;
        }
        else if(key == OS1_CONSOLE_KEY_COMPLETE || key == 0)
        {
            if(!state.value_choices(view))
                state.begin(view);
            if(!display.paint(state))
                return OS1_ERR_FAULT;
        }
        else if(key == OS1_CONSOLE_KEY_UP || key == OS1_CONSOLE_KEY_DOWN)
        {
            if(state.open)
                state.move(key == OS1_CONSOLE_KEY_UP ? -1 : 1);
            else if(state.focus >= 0)
                state.value_choices(view);
            else
                history.move(state, key == OS1_CONSOLE_KEY_UP ? -1 : 1);
            if(!display.paint(state))
                return OS1_ERR_FAULT;
        }
        else if(key == OS1_CONSOLE_KEY_LEFT || key == OS1_CONSOLE_KEY_RIGHT ||
                key == OS1_CONSOLE_KEY_HOME || key == OS1_CONSOLE_KEY_END)
        {
            state.horizontal(key == OS1_CONSOLE_KEY_LEFT || key == OS1_CONSOLE_KEY_HOME ? -1 : 1,
                             key == OS1_CONSOLE_KEY_HOME || key == OS1_CONSOLE_KEY_END);
            if(!display.paint(state))
                return OS1_ERR_FAULT;
        }
        else if(key == '\b' || key == 127 || key == OS1_CONSOLE_KEY_DELETE)
        {
            bool repaint = state.open || state.focus >= 0 || display.presentation.clipped ||
                           state.cursor != state.size || key == OS1_CONSOLE_KEY_DELETE;
            unsigned old_size = state.size;
            if(key == OS1_CONSOLE_KEY_DELETE)
                state.erase();
            else
                state.backspace();
            state.scan_fields(view);
            if(repaint)
            {
                if(!display.paint(state))
                    return OS1_ERR_FAULT;
            }
            else if(old_size != state.size && !output(2, "\b \b"))
                return OS1_ERR_FAULT;
        }
        else if(key >= 32 && key < 127)
        {
            bool repaint = state.open || state.focus >= 0 || display.presentation.clipped ||
                           state.cursor != state.size;
            bool choices_open = state.open, value_list = state.values;
            if(!state.insert(static_cast<char>(key)))
                overflow = true;
            state.scan_fields(view);
            if(key == '.')
                state.begin(view);
            else if(choices_open && os1::discovery::Completion::token(static_cast<char>(key)))
                state.refine(view, value_list);
            if(repaint || state.open ||
               state.size + display.presentation.prefix > display.presentation.width)
            {
                if(!display.paint(state))
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
