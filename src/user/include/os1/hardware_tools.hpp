#pragma once
#include <os1/hardware.h>
#include <os1/observe.h>

#include <os1/syscall.hpp>

#include "hardware/tree.hpp"

namespace os1::user::hardware_tools
{
struct Printer
{
    bool verbose{}, tree{}, failed{};
    unsigned seen[4096]{}, used{};
    void text(const char* s)
    {
        size_t n = 0;
        while(s[n])
            ++n;
        while(n && !failed)
        {
            long written = os1::user::write(1, s, n);
            if(written <= 0)
            {
                failed = true;
                return;
            }
            s += written;
            n -= written;
        }
    }
    void number(uint64_t v, unsigned base = 10)
    {
        char b[65]{};
        unsigned n = 64;
        do
        {
            b[--n] = "0123456789abcdef"[v % base];
            v /= base;
        } while(v);
        text(b + n);
    }
    void indent(unsigned depth)
    {
        for(unsigned i = 0; i < depth; ++i)
            text("  ");
    }
    long query(Os1HardwareQuery& q, unsigned id, unsigned operation, unsigned start = 0)
    {
        q = {};
        q.version = OS1_HARDWARE_VERSION;
        q.operation = operation;
        q.id = id;
        q.start = start;
        long status = observe(OS1_OBSERVE_HARDWARE, &q, sizeof(q));
        if(status < 0)
            failed = true;
        return status;
    }
    void node(unsigned id, unsigned depth)
    {
        if(failed)
            return;
        if(depth >= 16 || used == 4096)
        {
            text("[traversal limit]\n");
            failed = true;
            return;
        }
        Os1HardwareQuery q{};
        if(query(q, id, OS1_HARDWARE_DESCRIBE) < 0)
            return;
        indent(depth);
        text(q.nodes[0].name);
        text(" id=");
        number(id);
        for(unsigned i = 0; i < used; ++i)
            if(seen[i] == id)
            {
                text(" [reference]\n");
                return;
            }
        seen[used++] = id;
        text(" ");
        text(q.nodes[0].text);
        text("\n");
        unsigned start = 0;
        do
        {
            if(query(q, id, OS1_HARDWARE_CHILDREN, start) < 0)
                return;
            for(unsigned i = 0; i < q.count; ++i)
            {
                auto& f = q.nodes[i];
                if(f.type == OS1_HARDWARE_OBJECT)
                    continue;
                unsigned field = f.id & 63;
                if(!verbose && field != 2 && field != 3 && field != 4 && field != 5 &&
                   field != 37 && field != 35 && (field < 8 || field > 12))
                    continue;
                if(f.type == OS1_HARDWARE_STRING && !f.text[0])
                    continue;
                indent(depth + 1);
                text(f.name);
                text("=");
                if(f.type == OS1_HARDWARE_STRING)
                    text(f.text);
                else
                {
                    number(f.integer);
                }
                text("\n");
            }
            start = q.next;
        } while(!q.end && !failed);
        if(tree)
        {
            collection(id + 6, depth + 1);   // Physical attachment.
            collection(id + 34, depth + 1);  // Logical consumers.
        }
    }
    void collection(unsigned id, unsigned depth = 0)
    {
        unsigned start = 0;
        Os1HardwareQuery q{};
        do
        {
            if(query(q, id, OS1_HARDWARE_CHILDREN, start) < 0)
                return;
            // Recursion has its own query storage; preserve this page.
            for(unsigned i = 0; i < q.count && !failed; ++i)
                node(q.nodes[i].id, depth);
            start = q.next;
        } while(!q.end && !failed);
    }
};
inline bool equal(const char* a, const char* b)
{
    while(*a && *a == *b)
    {
        ++a;
        ++b;
    }
    return *a == *b;
}
inline int run(int argc, char** argv, unsigned mode)
{
    static Printer printer;
    printer.tree = mode == 0 || mode == 3;
    for(int i = 1; i < argc; ++i)
    {
        if(equal(argv[i], "-v"))
            printer.verbose = true;
        else if(equal(argv[i], "-t"))
            printer.tree = true;
        else if(equal(argv[i], "--help"))
        {
            printer.text("usage: ");
            printer.text(argv[0]);
            printer.text(
                " [-v] [-t]\nRead the passive boot catalog. -v: all fields; -t: relationships.\n");
            return printer.failed;
        }
        else
        {
            os1::user::write(2, "unsupported option; use --help\n", 29);
            return 2;
        }
    }
    printer.text("boot snapshot; cached observations, not current hotplug state\n");
    if(mode == 0)
    {
        printer.collection(OS1_HARDWARE_TOPOLOGY);
        printer.collection(OS1_HARDWARE_PROCESSORS);
        printer.collection(OS1_HARDWARE_ACPI);
        // Include storage with an unknown attachment after capture truncation.
        printer.collection(OS1_HARDWARE_BLOCKS);
    }
    else if(mode == 1)
        printer.collection(OS1_HARDWARE_PCI);
    else if(mode == 2)
        printer.collection(OS1_HARDWARE_USB);
    else
        printer.collection(OS1_HARDWARE_BLOCKS);
    Os1HardwareQuery q{};
    if(printer.query(q, OS1_HARDWARE_CATALOG_DROPPED, OS1_HARDWARE_DESCRIBE) == 0 &&
       q.nodes[0].integer)
    {
        printer.text("catalog records dropped=");
        printer.number(q.nodes[0].integer);
        printer.text("\n");
        printer.failed = true;
    }
    if(printer.failed)
    {
        os1::user::write(2, "hardware inspection incomplete\n", 30);
        return 1;
    }
    return 0;
}
}  // namespace os1::user::hardware_tools
