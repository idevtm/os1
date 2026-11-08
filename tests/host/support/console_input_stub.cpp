// Hardware boundary only: tests use the real console input queue and reservation code.
#include <os1/resource.h>

#include <string>

#include "console/console.hpp"
#include "console/terminal.hpp"
#include "terminal/line.hpp"
Terminal* active_terminal = nullptr;
void Terminal::write(char) {}
static std::string output;
void write_console_bytes(const char* data, size_t size) { output.append(data, size); }
void host_console_output_clear() { output.clear(); }
const std::string& host_console_output() { return output; }

os1_console_geometry console_geometry()
{
    return {80, 25};
}
void clear_console()
{
    output.clear();
}

long redraw_console_line(int delta, const char* text, size_t bytes)
{
    if(!os1::terminal::valid_line(80, delta, text, bytes))
        return OS1_ERR_INVALID;
    output += '\n';
    output.append(text, bytes);
    return 0;
}
