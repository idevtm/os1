// Hardware boundary only: tests use the real console input queue and reservation code.
#include "console/terminal.hpp"
#include "console/console.hpp"
#include <string>
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
