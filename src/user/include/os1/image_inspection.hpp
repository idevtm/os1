#pragma once
#include <os1/inspect_syntax.hpp>
#include <os1/vfs.hpp>

#include "interface/buffer_metadata.hpp"
#include "interface/metadata.hpp"
namespace os1::user::image_inspection
{
using namespace os1::interface;
using inspect_syntax::Output;
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
inline void rich_default(Output& output,
                         const p::Metadata& m,
                         const p::Entry& e,
                         bool declaration = false)
{
    static p::Value v;
    static char text[4096];
    p::Diagnostic d{};
    p::initialize(v, e.type);
    if(!p::detail::decode_node(
           m, e.type, m.defaults + e.default_offset, e.default_size, v, 0, 0, d))
        return;
    p::detail::Printer measured{m, v, nullptr, 0, declaration};
    measured.value(0);
    if(measured.size > sizeof(text))
    {
        output.print("<too long>");
        return;
    }
    p::detail::Printer printer{m, v, text, 0, declaration};
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
inline void details_view(Output& output,
                         const char* path,
                         const Metadata& m,
                         const os1::buffer_metadata::Metadata& buffers,
                         const os1::constructor::Metadata* ctor,
                         const p::Metadata* parameters,
                         const p::Binding& binding)
{
    output.print(m.interface_id ? m.class_name : path);
    output.kind("class");
    output.print("\n");
    if(m.interface_id)
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
    if(parameters)
        rich_help(output, m, *parameters, binding);
    if(ctor)
    {
        output.print("constructor [constructor] schema=");
        output.number(ctor->schema_id);
        output.print(" version=");
        output.number(ctor->schema_version);
        output.print(" size=");
        output.number(ctor->payload_bytes);
        output.print("\n");
        for(unsigned i = 0; i < ctor->field_count; ++i)
        {
            const auto& f = ctor->fields[i];
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
}

inline void type_ref(Output& out, const p::Metadata& m, unsigned id)
{
    const auto& type = m.types[id];
    if(type.kind == p::Kind::Optional)
    {
        type_ref(out, m, type.element);
        out.print("?");
    }
    else
        rich_text(out, m, type.text.name);
}

inline void rich_parameter(Output& out, const p::Metadata& m, const p::Entry& e)
{
    rich_text(out, m, e.text.name);
    out.print(": ");
    type_ref(out, m, e.type);
    if(e.default_size)
    {
        out.print(" = ");
        rich_default(out, m, e, true);
    }
}

inline void rich_arguments(Output& out, const p::Metadata& m)
{
    const auto& root = m.types[m.root];
    if(root.kind != p::Kind::Record)
    {
        out.print("input: ");
        type_ref(out, m, m.root);
        return;
    }
    for(unsigned i = 0; i < root.count; ++i)
    {
        if(i)
            out.print(", ");
        rich_parameter(out, m, m.entries[root.first + i]);
    }
}

inline const char* declaration_scalar(unsigned kind)
{
    return kind == U8 ? "U8" : kind == U32 ? "U32" : "U64";
}

inline void declare_rich_types(Output& out, const p::Metadata& m)
{
    using K = p::Kind;
    for(unsigned i = 0; i < m.type_count && out.good; ++i)
    {
        const auto& t = m.types[i];
        const bool group = t.kind == K::Record || t.kind == K::Enum || t.kind == K::Variant;
        out.print(t.kind == K::Record    ? "    record "
                  : t.kind == K::Enum    ? "    enum "
                  : t.kind == K::Variant ? "    variant "
                                         : "    type ");
        rich_text(out, m, t.text.name);
        if(t.kind == K::Enum)
        {
            out.print(": U");
            out.number(t.width * 8);
        }
        if(group)
        {
            out.print(" {\n");
            for(unsigned j = 0; j < t.count; ++j)
            {
                const auto& e = m.entries[t.first + j];
                out.print(t.kind == K::Variant ? "        case " : "        ");
                if(t.kind == K::Record)
                    rich_parameter(out, m, e);
                else
                {
                    rich_text(out, m, e.text.name);
                    if(t.kind == K::Enum)
                    {
                        out.print(" = ");
                        out.number(e.number);
                    }
                    else if(e.type != p::no_index)
                    {
                        out.print(": ");
                        type_ref(out, m, e.type);
                    }
                }
                out.print("\n");
            }
            out.print("    }\n");
            continue;
        }
        out.print(" = ");
        switch(t.kind)
        {
            case K::Unsigned:
            case K::Signed:
                out.print(t.kind == K::Signed ? "I" : "U");
                out.number(t.width * 8);
                out.print(" range ");
                if(t.kind == K::Signed)
                {
                    out.integer(p::signed_value(t.minimum));
                    out.print("..");
                    out.integer(p::signed_value(t.maximum));
                }
                else
                {
                    out.number(t.minimum);
                    out.print("..");
                    out.number(t.maximum);
                }
                break;
            case K::Boolean:
                out.print("Bool");
                break;
            case K::String:
                out.print("String<");
                out.number(t.bound);
                out.print(">");
                break;
            case K::Optional:
                type_ref(out, m, i);
                break;
            case K::List:
                out.print("List<");
                type_ref(out, m, t.element);
                out.print(", ");
                out.number(t.bound);
                out.print(">");
                break;
            default:
                break;
        }
        out.print("\n");
    }
}

inline void declarations(Output& out,
                         const char* path,
                         const Metadata& m,
                         const os1::constructor::Metadata* ctor,
                         const p::Metadata* parameters,
                         const p::Binding& binding)
{
    if(m.interface_id)
    {
        out.heading("class", m.class_name);
        out.member("implements", m.interface_name);
        out.print("\n");
    }
    else
    {
        out.print("class ");
        out.quoted(path, files::length(path));
        out.print(" {\n");
    }
    if(parameters && binding.constructor)
    {
        out.print("    constructor(");
        rich_arguments(out, *parameters);
        out.print(")\n");
    }
    if(ctor)
    {
        out.print("    constructor(");
        for(unsigned i = 0; i < ctor->field_count; ++i)
        {
            const auto& f = ctor->fields[i];
            if(i)
                out.print(", ");
            out.print(f.name);
            out.print(": ");
            out.print(declaration_scalar(f.kind));
            out.print(" = ");
            out.number(f.default_value);
        }
        out.print(")\n");
    }
    auto type = [&](unsigned id) { return id ? m.types[id - 1].name : "Void"; };
    bool emitted[max_members]{};
    for(unsigned i = 0; i < m.member_count; ++i)
    {
        if(emitted[i])
            continue;
        const auto& a = m.members[i];
        if(a.kind == PropertyGet || a.kind == PropertySet)
        {
            const unsigned property_type = a.kind == PropertyGet ? a.output : a.input;
            bool get = a.kind == PropertyGet, set = !get;
            for(unsigned j = i + 1; j < m.member_count; ++j)
            {
                const auto& b = m.members[j];
                if(a.kind != b.kind && (b.kind == PropertyGet || b.kind == PropertySet) &&
                   property_type == (b.kind == PropertyGet ? b.output : b.input) &&
                   os1::discovery::same(a.name, b.name))
                {
                    get = set = true;
                    emitted[j] = true;
                }
            }
            out.member("property", a.name);
            out.print(": ");
            out.print(property_type ? type(property_type) : "unavailable");
            out.print(get && set ? " { get set }" : get ? " { get }" : " { set }");
        }
        else
        {
            out.member(a.kind == Method ? "method" : "event", a.name);
            out.print("(");
            if(parameters && p::binds(binding, m, a.id))
                rich_arguments(out, *parameters);
            else if(a.input)
            {
                out.print(a.kind == Event ? "update: " : "input: ");
                out.print(type(a.input));
            }
            out.print(")");
            if(a.kind == Method)
            {
                out.print(" -> ");
                out.print(type(a.output));
            }
        }
        out.print("\n");
    }
    for(unsigned i = 0; i < m.type_count; ++i)
    {
        const auto& t = m.types[i];
        out.member(t.kind == Record ? "record" : "type", t.name);
        if(t.kind != Record)
        {
            out.print(" = ");
            out.print(declaration_scalar(t.kind));
            out.print("\n");
            continue;
        }
        out.print(" {\n");
        for(unsigned j = 0; j < t.field_count; ++j)
        {
            const auto& field = m.fields[t.first_field + j];
            out.print("        ");
            out.print(field.name);
            out.print(": ");
            if(field.count > 1)
                out.print("Array<");
            out.print(declaration_scalar(field.kind));
            if(field.count > 1)
            {
                out.print(", ");
                out.number(field.count);
                out.print(">");
            }
            out.print("\n");
        }
        out.print("    }\n");
    }
    if(parameters)
        declare_rich_types(out, *parameters);
    out.print("}\n");
}
inline constexpr uint32_t image_rights = OS1_RIGHT_READ | OS1_RIGHT_SEEK | OS1_RIGHT_INSPECT;
inline int inspect_handle(int fd, const char* path, bool details)
{
    Output output{1};
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
    if(details)
        details_view(output,
                     path,
                     m,
                     buffers,
                     found ? &ctor : nullptr,
                     rich_found ? &parameters : nullptr,
                     binding);
    else
        declarations(
            output, path, m, found ? &ctor : nullptr, rich_found ? &parameters : nullptr, binding);
    if(!output.good)
    {
        files::message("inspect: cannot write inspection output\n");
        return 1;
    }
    return 0;
}
}  // namespace os1::user::image_inspection
