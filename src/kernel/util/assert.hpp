// Freestanding kernel assertion helpers. Runtime assertions report the file,
// line, and expression through the serial debug logger before halting.
#pragma once

#include "debug/debug.hpp"

#ifndef NDEBUG
#define assert(x)                           \
    if(!(x))                                \
    {                                       \
        debug(__FILE__)(":")(__LINE__);     \
        debug(" assertion failed: ")(#x)(); \
        do                                  \
        {                                   \
            asm volatile("cli");            \
            asm volatile("hlt");            \
        } while(true);                      \
    }
#else
#define assert(x)  // do nothing
#endif
