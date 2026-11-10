#pragma once
#include <os1/hardware.h>
#include <os1/observe.h>

#include <os1/syscall.hpp>

#include "hardware/presentation.hpp"
#include "hardware/tree.hpp"

namespace os1::user::hardware_tools
{
struct Printer
{
    bool verbose{}, tree{}, failed{};
    bool (*output)(void*, const char*, unsigned){};
    void* output_context{};
    unsigned seen[4096]{}, used{};
    uint64_t generation{};
    void text(const char* s)
    {
        size_t n = 0;
        while(s[n])
            ++n;
        if(output)
        {
            if(!failed && !output(output_context, s, n))
                failed = true;
            return;
        }
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
    void number(uint64_t value, unsigned base = 10, unsigned width = 0)
    {
        os1::hardware::presentation::number(*this, value, base, width);
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
        q.generation = generation;
        long status = observe(OS1_OBSERVE_HARDWARE, &q, sizeof(q));
        if(status < 0)
            failed = true;
        else
            generation = q.generation;
        return status;
    }
    struct Fields
    {
        uint64_t values[40]{}, known{};
        char name[96]{}, state[96]{}, driver[96]{};
        bool has(unsigned field) const
        {
            return (known >> field) & 1;
        }
    };
    bool fields(unsigned id, Fields& out)
    {
        Os1HardwareQuery q{};
        unsigned start = 0;
        do
        {
            if(query(q, id, OS1_HARDWARE_CHILDREN, start) < 0)
                return false;
            for(unsigned i = 0; i < q.count; ++i)
            {
                auto& f = q.nodes[i];
                unsigned field = f.id & 63;
                if(field >= 40 || f.type == OS1_HARDWARE_OBJECT)
                    continue;
                out.known |= uint64_t(1) << field;
                if(f.type == OS1_HARDWARE_INTEGER)
                    out.values[field] = f.integer;
                else if(field >= 2 && field <= 4)
                    os1::hardware::copy(field == 2   ? out.name
                                        : field == 3 ? out.state
                                                     : out.driver,
                                        96,
                                        f.text);
            }
            start = q.next;
        } while(!q.end);
        return true;
    }
    void integer(unsigned kind, unsigned field, uint64_t value)
    {
        using namespace os1::hardware;
        bool hex =
            (kind == Pci && field >= 12 && field <= 17) ||
            (kind == UsbDevice && (field == 8 || field == 9 || (field >= 11 && field <= 13))) ||
            (kind == UsbInterface && field >= 10 && field <= 12) ||
            (kind == Partition && field == 9);
        if(kind == UsbDevice && field == 10)
            presentation::usb_version(*this, value);
        else if(kind == UsbDevice && field == 14)
            presentation::speed(*this, value);
        else if(hex)
        {
            text("0x");
            number(value,
                   16,
                   ((kind == Pci && field <= 13) || (kind == UsbDevice && field <= 9)) ? 4 : 2);
        }
        else
            number(value);
        if((kind == Block && field == 10) || (kind == Partition && field == 12) ||
           (kind == Filesystem && field == 10) || (kind == Memory && field >= 8 && field <= 17))
        {
            text(" (");
            presentation::capacity(*this, value);
            text(")");
        }
    }
    void value(const Fields& f, unsigned kind, unsigned field, const char* label)
    {
        if(!f.has(field))
            return;
        text(" ");
        text(label);
        text("=");
        integer(kind, field, f.values[field]);
    }
    void summary(unsigned kind, const Fields& f)
    {
        using namespace os1::hardware;
        if(kind == Pci)
        {
            text(" ");
            number(f.values[8], 16, 4);
            text(":");
            number(f.values[9], 16, 2);
            text(":");
            number(f.values[10], 16, 2);
            text(".");
            number(f.values[11], 16);
            text(" ");
            text(presentation::pci_class(f.values[14], f.values[15]));
            text(" [");
            number(f.values[12], 16, 4);
            text(":");
            number(f.values[13], 16, 4);
            text("]");
            text(" class=");
            number(f.values[14], 16, 2);
            text(":");
            number(f.values[15], 16, 2);
            text(":");
            number(f.values[16], 16, 2);
        }
        else if(kind == UsbDevice)
        {
            if(f.has(8) && f.has(9))
            {
                text(" [");
                number(f.values[8], 16, 4);
                text(":");
                number(f.values[9], 16, 4);
                text("]");
            }
            else
                text(" identity=unknown");
            value(f, kind, 10, "usb");
            value(f, kind, 11, "class");
            if(f.has(14))
                value(f, kind, 14, "speed");
            else
                text(" speed=unknown");
            value(f, kind, 17, "port");
        }
        else if(kind == UsbInterface)
        {
            value(f, kind, 8, "number");
            value(f, kind, 9, "alternate");
            value(f, kind, 10, "class");
            if(f.has(10))
            {
                text(" ");
                text(presentation::usb_class(f.values[10]));
            }
            value(f, kind, 13, "endpoints");
        }
        else if(kind == UsbPort)
        {
            value(f, kind, 8, "port");
            value(f, kind, 9, "connected");
        }
        else if(kind == Block)
        {
            value(f, kind, 10, "size_bytes");
            value(f, kind, 9, "sector_size");
        }
        else if(kind == Partition)
        {
            value(f, kind, 8, "number");
            value(f, kind, 9, "type");
            value(f, kind, 12, "size_bytes");
        }
        else if(kind == Filesystem)
        {
            text(" name=");
            text(f.name);
            value(f, kind, 10, "size_bytes");
            value(f, kind, 8, "block_size");
        }
        else if(kind == Processor)
            value(f, kind, 8, "apic_id");
        else
        {
            text(" name=");
            text(f.name);
        }
        if(kind == Pci || kind == UsbDevice)
        {
            text(" name=");
            text(f.name);
        }
        if(kind == Memory)
            value(f, kind, 8, "usable_bytes");
        if(kind == CpuIdentity)
        {
            value(f, kind, 8, "family");
            value(f, kind, 9, "model");
            value(f, kind, 10, "stepping");
        }
        if(kind == Pci || kind == Acpi)
            value(f, kind, 39, kind == Pci ? "firmware_id" : "device_id");
        if(f.state[0])
        {
            text(" state=");
            text(f.state);
        }
        if(f.driver[0])
        {
            text(" driver=");
            text(f.driver);
        }
        text("\n");
    }
    // Keep the large render buffers outside recursive traversal frames.
    __attribute__((noinline)) bool render(unsigned id, unsigned depth, bool usb_controllers_only)
    {
        unsigned kind = (id - os1::hardware::record_base) >> 16;
        Fields f{};
        if(!fields(id, f))
            return false;
        if(usb_controllers_only &&
           (kind != os1::hardware::Pci || f.values[14] != 12 || f.values[15] != 3))
            return false;
        if(depth >= 16 || used == 4096)
        {
            text("[traversal limit]\n");
            failed = true;
            return false;
        }
        Os1HardwareQuery q{};
        if(query(q, id, OS1_HARDWARE_DESCRIBE) < 0)
            return false;
        indent(depth);
        text(q.nodes[0].name);
        text(" ");
        text(q.nodes[0].text);
        if(verbose)
        {
            text(" id=");
            number(id);
        }
        summary(kind, f);
        unsigned start = 0;
        while(verbose)
        {
            if(query(q, id, OS1_HARDWARE_CHILDREN, start) < 0)
                return false;
            for(unsigned i = 0; i < q.count; ++i)
            {
                auto& field = q.nodes[i];
                if(field.type == OS1_HARDWARE_OBJECT ||
                   (field.type == OS1_HARDWARE_STRING && !field.text[0]))
                    continue;
                indent(depth + 1);
                text(field.name);
                text("=");
                if(field.type == OS1_HARDWARE_STRING)
                    text(field.text);
                else
                    integer(kind, field.id & 63, field.integer);
                text("\n");
            }
            if(q.end)
                break;
            start = q.next;
        }
        return true;
    }
    void node(unsigned id, unsigned depth, bool usb_controllers_only = false)
    {
        if(failed)
            return;
        for(unsigned i = 0; i < used; ++i)
            if(seen[i] == id)
                return;
        if(!render(id, depth, usb_controllers_only))
            return;
        seen[used++] = id;
        if(tree)
        {
            collection(id + 6, depth + 1);
            collection(id + 34, depth + 1);
        }
    }
    void collection(unsigned id, unsigned depth = 0, bool usb_controllers_only = false)
    {
        unsigned start = 0;
        Os1HardwareQuery q{};
        do
        {
            if(query(q, id, OS1_HARDWARE_CHILDREN, start) < 0)
                return;
            for(unsigned i = 0; i < q.count && !failed; ++i)
                node(q.nodes[i].id, depth, usb_controllers_only);
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
    printer.text("platform boot snapshot; USB attachments updated at runtime\n");
    if(mode == 0)
    {
        printer.collection(OS1_HARDWARE_PLATFORM);
        printer.collection(OS1_HARDWARE_TOPOLOGY);
        printer.collection(OS1_HARDWARE_PROCESSORS);
        printer.collection(OS1_HARDWARE_ACPI);
        // Include storage with an unknown attachment after capture truncation.
        printer.collection(OS1_HARDWARE_BLOCKS);
    }
    else if(mode == 1)
        printer.collection(OS1_HARDWARE_PCI);
    else if(mode == 2)
    {
        if(printer.tree)
            printer.collection(OS1_HARDWARE_PCI, 0, true);
        // Also retain records whose parent was lost during bounded capture.
        printer.collection(OS1_HARDWARE_USB);
    }
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
