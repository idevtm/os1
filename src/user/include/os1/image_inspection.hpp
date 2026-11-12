#pragma once
#include <os1/file_tools.hpp>
#include <os1/vfs.hpp>

#include "discovery/presentation.hpp"
#include "interface/buffer_metadata.hpp"
#include "interface/metadata.hpp"
namespace os1::user::image_inspection
{
using namespace os1::interface;
struct Output
{
    bool good = true;

    void put(const char* text, size_t size)
    {
        if(good)
            good = files::put(1, text, size);
    }

    void print(const char* text)
    {
        put(text, files::length(text));
    }

    void kind(const char* kind, const char* qualifier = nullptr)
    {
        os1::discovery::listing_kind(
            [this](const char* text) {
                print(text);
                return good;
            },
            kind,
            qualifier);
    }

    void number(uint64_t value)
    {
        char digits[21];
        unsigned first = sizeof(digits);
        do
        {
            digits[--first] = '0' + value % 10;
            value /= 10;
        } while(value);
        put(digits + first, sizeof(digits) - first);
    }
};
inline const char* scalar(unsigned kind)
{
    return kind == U8 ? "u8" : kind == U32 ? "u32" : "u64";
}
namespace p = os1::parameters;
inline void rich_text(Output& output, const p::Metadata& m, p::Text t)
{
    output.put(m.strings + t.offset, t.size);
}
inline void documentation(Output& output, const p::Metadata& m, const p::Documentation& text)
{
    if(text.summary.size)
    {
        output.print(" -- ");
        rich_text(output, m, text.summary);
    }
    if(text.description.size)
    {
        output.print(" ");
        rich_text(output, m, text.description);
    }
    if(text.unit.size)
    {
        output.print(" unit=");
        rich_text(output, m, text.unit);
    }
    if(text.format.size)
    {
        output.print(" format=");
        rich_text(output, m, text.format);
    }
}
inline const char* rich_kind(p::Kind k)
{
    switch(k)
    {
        case p::Kind::Unsigned:
            return "unsigned";
        case p::Kind::Signed:
            return "signed";
        case p::Kind::Boolean:
            return "bool";
        case p::Kind::Enum:
            return "enum";
        case p::Kind::String:
            return "string";
        case p::Kind::Record:
            return "struct";
        case p::Kind::Optional:
            return "optional";
        case p::Kind::Variant:
            return "variant";
        case p::Kind::List:
            return "list";
    }
    return "invalid";
}
// Metadata was fully validated; decode each constant using the exact shared wire
// grammar and size the canonical printer before writing into bounded storage.
inline void rich_default(Output& output, const p::Metadata& m, const p::Entry& e)
{
    static p::Value v;
    static char text[4096];
    p::Diagnostic d{};
    p::initialize(v, e.type);
    if(!p::detail::decode_node(
           m, e.type, m.defaults + e.default_offset, e.default_size, v, 0, 0, d))
        return;
    p::detail::Printer measured{m, v, nullptr};
    measured.value(0);
    if(measured.size > sizeof(text))
    {
        output.print("<too long>");
        return;
    }
    p::detail::Printer printer{m, v, text};
    printer.value(0);
    output.put(text, printer.size);
}
inline int read_rich(int fd, const Metadata& api, p::Metadata& m, p::Binding& binding)
{
    static p::Metadata scratch;
    int found = parameter_metadata(fd, m, scratch);
    if(found < 0)
        return -1;
    if(!found)
        return 0;
    if(!parameter_binding(fd, m, binding) ||
       (api.interface_id ? binding.interface_id != api.interface_id ||
                               binding.interface_version != api.interface_version
                         : !binding.constructor || binding.member_count))
        return -1;
    for(unsigned i = 0; i < binding.member_count; ++i)
        if(!p::binds(binding, api, binding.members[i]))
            return -1;
    return 1;
}
inline void rich_help(Output& output,
                      const Metadata& api,
                      const p::Metadata& m,
                      const p::Binding& binding)
{
    output.print("parameters [schema] profile=1 schema=");
    output.number(m.schema_id);
    output.print(" version=");
    output.number(m.schema_version);
    output.print("\n");
    if(binding.constructor)
    {
        output.print("constructor [constructor] input=");
        rich_text(output, m, m.types[m.root].text.name);
        output.print("\n");
    }
    for(unsigned i = 0; i < binding.member_count; ++i)
    {
        unsigned id = binding.members[i];
        for(unsigned j = 0; j < api.member_count; ++j)
            if(api.members[j].id == id)
            {
                output.print(api.members[j].name);
                output.kind("method");
                output.print(" input=");
                rich_text(output, m, m.types[m.root].text.name);
                output.print("\n");
            }
    }
    for(unsigned i = 0; i < m.type_count; ++i)
    {
        const auto& t = m.types[i];
        rich_text(output, m, t.text.name);
        output.kind("type");
        output.print(" ");
        output.print(rich_kind(t.kind));
        if(t.kind == p::Kind::Optional || t.kind == p::Kind::List)
        {
            output.print(" element=");
            rich_text(output, m, m.types[t.element].text.name);
        }
        if(t.width)
        {
            output.print(" bits=");
            output.number(t.width * 8);
        }
        if(t.kind == p::Kind::Unsigned)
        {
            output.print(" range=");
            output.number(t.minimum);
            output.print("..");
            output.number(t.maximum);
        }
        if(t.kind == p::Kind::String || t.kind == p::Kind::List)
        {
            output.print(" bound=");
            output.number(t.bound);
        }
        documentation(output, m, t.text);
        output.print("\n");
        for(unsigned j = 0; j < t.count; ++j)
        {
            const auto& e = m.entries[t.first + j];
            output.print("  ");
            rich_text(output, m, e.text.name);
            output.kind(t.kind == p::Kind::Enum      ? "value"
                        : t.kind == p::Kind::Variant ? "case"
                                                     : "field");
            output.print(" id=");
            output.number(e.id);
            if(e.type != p::no_index)
            {
                output.print(": ");
                rich_text(output, m, m.types[e.type].text.name);
                if(t.kind == p::Kind::Record)
                {
                    if(e.default_size)
                    {
                        output.print(" default=");
                        rich_default(output, m, e);
                    }
                    else
                        output.print(m.types[e.type].kind == p::Kind::Optional ? " omitted=null"
                                                                               : " required");
                    if(m.types[e.type].kind == p::Kind::Optional)
                        output.print(" nullable");
                }
            }
            else if(t.kind == p::Kind::Enum)
            {
                output.print(" wire=");
                output.number(e.number);
            }
            else
                output.print(" unit");
            documentation(output, m, e.text);
            output.print("\n");
        }
    }
    output.print(
        "CLI: field:value field.case options:{limit:10} label:null; quoted strings preserve "
        "spaces.\n");
}
inline constexpr uint32_t image_rights = OS1_RIGHT_READ | OS1_RIGHT_SEEK | OS1_RIGHT_INSPECT;
inline int inspect_handle(int fd, const char* path)
{
    Output output;
    os1_file_info info{};
    if(fd < 0 || file_info(fd, &info) < 0)
    {
        output.print("inspect: cannot open file\n");
        return 1;
    }
    Metadata m{};
    auto reader = [&](uint64_t offset, void* bytes, size_t size) {
        if(offset > INT64_MAX || seek(fd, offset) < 0)
            return false;
        auto* p = static_cast<uint8_t*>(bytes);
        while(size)
        {
            long n = read(fd, p, size);
            if(n <= 0)
                return false;
            p += n;
            size -= n;
        }
        return true;
    };
    const int interface_found = read_section(reader, info.byte_size, ".os.interface", m, validate);
    if(interface_found < 0)
    {
        output.print("inspect: invalid interface metadata\n");
        return 1;
    }
    os1::buffer_metadata::Metadata buffers{};
    const int buffer_found = read_section(reader,
                                          info.byte_size,
                                          ".os.buffers",
                                          buffers,
                                          [&](const os1::buffer_metadata::Metadata& value) {
                                              return os1::buffer_metadata::validate(value, m);
                                          });
    if(buffer_found < 0)
    {
        output.print("inspect: invalid Buffer metadata\n");
        return 1;
    }
    os1::constructor::Metadata ctor{};
    const int found = constructor_metadata(fd, ctor);
    static p::Metadata parameters;
    p::Binding binding{};
    const int rich_found = read_rich(fd, m, parameters, binding);
    if(rich_found < 0)
    {
        output.print("inspect: invalid rich parameter metadata\n");
        return 1;
    }
    if(found < 0)
    {
        output.print("inspect: invalid constructor metadata\n");
        return 1;
    }
    if(!interface_found && !rich_found && !found)
    {
        output.print("inspect: no class metadata\n");
        return 1;
    }
    output.print(interface_found ? m.class_name : path);
    output.kind("class");
    output.print("\n");
    if(interface_found)
    {
        output.print(m.interface_name);
        output.kind("interface");
        output.print(" id=");
        output.number(m.interface_id);
        output.print(" version=");
        output.number(m.interface_version);
        output.print("\n");
    }
    for(unsigned i = 0; i < m.type_count; ++i)
    {
        const auto& t = m.types[i];
        output.print(t.name);
        output.kind("type");
        output.print(" id=");
        output.number(i + 1);
        output.print(" size=");
        output.number(t.size);
        if(t.kind != Record)
        {
            output.print(" ");
            output.print(scalar(t.kind));
        }
        output.print("\n");
        for(unsigned j = 0; j < t.field_count; ++j)
        {
            const auto& f = m.fields[t.first_field + j];
            output.print("  ");
            output.print(f.name);
            output.kind("field");
            output.print(" ");
            output.print(scalar(f.kind));
            output.print("[");
            output.number(f.count);
            output.print("] offset=");
            output.number(f.offset);
            output.print("\n");
        }
    }
    for(unsigned i = 0; i < m.member_count; ++i)
    {
        const auto& a = m.members[i];
        output.print(a.name);
        output.kind(a.kind == Method  ? "method"
                    : a.kind == Event ? "event"
                                      : "property",
                    a.kind == PropertyGet   ? "get"
                    : a.kind == PropertySet ? "set"
                                            : nullptr);
        output.print(" id=");
        output.number(a.id);
        output.print(" input=");
        output.number(a.input);
        output.print(" output=");
        output.number(a.output);
        output.print(a.flags ? " implemented\n" : " metadata-only\n");
    }
    for(unsigned i = 0; i < buffers.count; ++i)
    {
        const auto& b = buffers.members[i];
        output.print("Buffer [transfer] method=");
        output.number(b.member);
        output.print(" mode=exclusive input=loan reference=attachment,offset,length output=");
        output.print(b.output == os1::buffer_metadata::return_input ? "return_same\n"
                                                                    : "move_distinct\n");
    }
    if(rich_found)
        rich_help(output, m, parameters, binding);
    if(found)
    {
        output.print("constructor [constructor] schema=");
        output.number(ctor.schema_id);
        output.print(" version=");
        output.number(ctor.schema_version);
        output.print(" size=");
        output.number(ctor.payload_bytes);
        output.print("\n");
        for(unsigned i = 0; i < ctor.field_count; ++i)
        {
            const auto& f = ctor.fields[i];
            output.print("  ");
            output.print(f.name);
            output.kind("field");
            output.print(" ");
            output.print(scalar(f.kind));
            output.print(" offset=");
            output.number(f.offset);
            output.print(" default=");
            output.number(f.default_value);
            output.print(" cli=--");
            output.print(f.option);
            output.print("\n");
        }
    }
    if(!output.good)
    {
        files::message("inspect: cannot write inspection output\n");
        return 1;
    }
    return 0;
}
}  // namespace os1::user::image_inspection
