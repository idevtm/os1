#pragma once
#include "discovery/completion.hpp"
namespace os1::discovery
{
struct Row
{
    char bytes[OS1_CONSOLE_REDRAW_MAX + 1]{};
    unsigned size = 0, limit;
    explicit Row(unsigned width) : limit(width) {}
    void append(const char* text)
    {
        for(unsigned i = 0; text[i] && size < limit; ++i)
            bytes[size++] = text[i];
    }
    void member(const Member& entry)
    {
        append(entry.name);
        append(" [");
        append(kind_name(entry.kind));
        append(entry.flags & Native ? ", native] " : "] ");
        append(entry.signature);
    }
};
// Track the last successful row so cleanup can recover a partial redraw.
// Reserving space can scroll once; row replacement never wraps or scrolls.
struct Presentation
{
    unsigned width = 79, menu_rows = 9, reserved = 0, position = 0, prefix = 5;
    bool clipped = false;
    void configure(unsigned columns, unsigned rows)
    {
        width = columns > 1 ? columns - 1 : 1;
        if(width > OS1_CONSOLE_REDRAW_MAX)
            width = OS1_CONSOLE_REDRAW_MAX;
        menu_rows = rows > 1 ? rows - 1 : 1;
        if(menu_rows > page_limit + 1)
            menu_rows = page_limit + 1;
    }
    template<class Sink>
    bool paint(const View& view, const Completion& state, Sink& sink)
    {
        bool visible = state.open || state.detail;
        if(visible && !reserved)
        {
            for(unsigned i = 0; i < menu_rows; ++i)
                if(!sink.advance())
                    return false;
            reserved = position = menu_rows;
        }
        auto draw = [&](unsigned target, const Row& line) {
            if(!sink.row(int(target) - int(position), line.bytes, line.size))
                return false;
            position = target;
            return true;
        };
        for(unsigned row = 0; row < reserved; ++row)
        {
            Row line(width);
            if(state.open)
            {
                if(!row)
                    line.append("context choices (Tab explore, Enter accept, Esc cancel)");
                else
                {
                    unsigned choices = menu_rows - 1;
                    unsigned first = state.selected / choices * choices;
                    unsigned index = first + row - 1;
                    if(index < state.matches)
                    {
                        line.append(index == state.selected ? "> " : "  ");
                        line.member(view.nodes[state.ids[index]]);
                    }
                }
            }
            else if(state.detail && state.leaf >= 0)
            {
                const auto& member = view.nodes[state.leaf];
                if(!row)
                    line.member(member);
                else if(row == 1 && (member.flags & Native) && !(member.flags & Shell))
                    line.append("Metadata only in this shell; use the typed API.");
            }
            if(!draw(row, line))
                return false;
        }
        Row command(width);
        command.append("  > ");
        unsigned available = width > command.size ? width - command.size : 0;
        unsigned start = state.size > available ? state.size - available : 0;
        command.append(state.line + start);
        if(!visible && reserved)
        {
            // Clear the old command row, then return to the original prompt row.
            if(!draw(reserved, Row(width)))
                return false;
        }
        if(!draw(visible ? reserved : 0, command))
            return false;
        if(!visible)
            reserved = 0;
        prefix = 4;
        clipped = start != 0;
        return true;
    }
};
// Restore the command row on all exits, including input errors during a sequence.
// If output also fails, cleanup is best effort and preserves the original error.
template<class Sink>
struct PresentationCleanup
{
    const View& view;
    Completion& state;
    Presentation& presentation;
    Sink& sink;
    ~PresentationCleanup()
    {
        if(presentation.reserved)
        {
            state.cancel();
            presentation.paint(view, state, sink);
        }
    }
};
}  // namespace os1::discovery
