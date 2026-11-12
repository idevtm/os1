#pragma once
#include <os1/file_tools.hpp>
#include <os1/vfs.hpp>

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
inline bool rich_help(Output& output, int fd, const Metadata& api)
{
    static p::Metadata m, scratch;
    int found = parameter_metadata(fd, m, scratch);
    if(found < 0)
        return false;
    if(!found)
        return true;
    p::Binding binding{};
    if(!parameter_binding(fd, m, binding) || binding.interface_id != api.interface_id ||
       binding.interface_version != api.interface_version)
        return false;
    output.print("rich parameters profile=1 schema=");
    output.number(m.schema_id);
    output.print(" version=");
    output.number(m.schema_version);
    output.print("\n");
    if(binding.constructor)
    {
        output.print("constructor input=");
        rich_text(output, m, m.types[m.root].text.name);
        output.print("\n");
    }
    for(unsigned i = 0; i < binding.member_count; ++i)
    {
        unsigned id = binding.members[i];
        if(!p::binds(binding, api, id))
            return false;
        for(unsigned j = 0; j < api.member_count; ++j)
            if(api.members[j].id == id)
            {
                output.print("method ");
                output.print(api.members[j].name);
                output.print(" input=");
                rich_text(output, m, m.types[m.root].text.name);
                output.print("\n");
            }
    }
    for(unsigned i = 0; i < m.type_count; ++i)
    {
        const auto& t = m.types[i];
        output.print("type ");
        rich_text(output, m, t.text.name);
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
            output.print(t.kind == p::Kind::Enum      ? "value "
                         : t.kind == p::Kind::Variant ? "case "
                                                      : "field ");
            rich_text(output, m, e.text.name);
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
    return true;
}
inline constexpr uint32_t image_rights = OS1_RIGHT_READ | OS1_RIGHT_SEEK | OS1_RIGHT_INSPECT;
inline int inspect_handle(int fd)
{
    Output output;
    os1_file_info info{};
    if(fd < 0 || file_info(fd, &info) < 0)
    {
        output.print("intf: cannot open file\n");
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
    bool valid = read_elf(reader, info.byte_size, m);
    if(!valid)
    {
        output.print("intf: invalid or absent interface metadata\n");
        return 1;
    }
    output.print("class ");
    output.print(m.class_name);
    output.print("\ninterface ");
    output.print(m.interface_name);
    output.print(" id=");
    output.number(m.interface_id);
    output.print(" version=");
    output.number(m.interface_version);
    output.print("\n");
    for(unsigned i = 0; i < m.type_count; ++i)
    {
        const auto& t = m.types[i];
        output.print("type ");
        output.number(i + 1);
        output.print(" ");
        output.print(t.name);
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
            output.print(": ");
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
        output.print(a.kind == Method        ? "method "
                     : a.kind == PropertyGet ? "property get "
                     : a.kind == PropertySet ? "property set "
                                             : "event ");
        output.print(a.name);
        output.print(" id=");
        output.number(a.id);
        output.print(" input=");
        output.number(a.input);
        output.print(" output=");
        output.number(a.output);
        output.print(a.flags ? " implemented\n" : " metadata-only\n");
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
        output.print("intf: invalid Buffer metadata\n");
        return 1;
    }
    for(unsigned i = 0; i < buffers.count; ++i)
    {
        const auto& b = buffers.members[i];
        output.print("Buffer method=");
        output.number(b.member);
        output.print(" mode=exclusive input=loan reference=attachment,offset,length output=");
        output.print(b.output == os1::buffer_metadata::return_input ? "return_same\n"
                                                                    : "move_distinct\n");
    }
    os1::constructor::Metadata ctor{};
    const int found = constructor_metadata(fd, ctor);
    bool rich_valid = rich_help(output, fd, m);
    if(!rich_valid)
    {
        output.print("intf: invalid rich parameter metadata\n");
        return 1;
    }
    if(found < 0)
    {
        output.print("intf: invalid constructor metadata\n");
        return 1;
    }
    if(found)
    {
        output.print("constructor schema=");
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
            output.print(": ");
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
        files::message("intf: cannot write inspection output\n");
        return 1;
    }
    return 0;
}
inline int inspect_file(const char* path)
{
    files::Handle image(open(path, image_rights));
    return inspect_handle(image.fd);
}
}  // namespace os1::user::image_inspection
