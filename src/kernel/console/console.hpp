// Kernel console stream service. This layer mirrors bytes to serial debug and
// the active logical terminal without exposing terminal internals to syscalls.
#pragma once

#include <os1/console.h>
#include <stddef.h>

os1_console_geometry console_geometry();
void clear_console();
long set_console_column(unsigned column);
long redraw_console_line(int delta, const char* text, size_t bytes);

// write exactly `length` bytes to the kernel console stream.
void write_console_bytes(const char* data, size_t length);

// write a nul-terminated line and append a newline on both console backends.
void write_console_line(const char* text);
void console_echo_backspace();

// Debug mirror sink that draws one byte on the active terminal. Bytes are
// dropped while the console is busy so debug output can never deadlock it.
void console_mirror_debug_byte(char c);

// Hold the console lock across an exclusive framebuffer operation.
struct FramebufferTextDisplay;
struct os1_framebuffer_surface;
struct os1_display_driver;
struct os1_display_fill;
struct NativeObject;
long console_display_acquire(NativeObject*, bool native);
void console_display_release(NativeObject*, bool restored, bool native);
long console_display_fill(NativeObject*,
                          const os1_display_driver&,
                          const os1_framebuffer_surface&,
                          const os1_display_fill&);
void console_switch_terminal(unsigned index);
