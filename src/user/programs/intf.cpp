#include <os1/vfs.hpp>

#include "interface/buffer_metadata.hpp"
#include "interface/metadata.hpp"
using namespace os1::user;
using namespace os1::interface;
void print(const char* s)
{
    unsigned n = 0;
    while(s[n])
        ++n;
    write(1, s, n);
}
void number(uint64_t n)
{
    char b[21];
    unsigned i = sizeof(b);
    do
    {
        b[--i] = '0' + n % 10;
        n /= 10;
    } while(n);
    write(1, b + i, sizeof(b) - i);
}
const char* scalar(unsigned kind)
{
    return kind == U8 ? "u8" : kind == U32 ? "u32" : "u64";
}
namespace p = os1::parameters;
void rich_text(const p::Metadata& m, p::Text t)
{
    write(1, m.strings + t.offset, t.size);
}
void documentation(const p::Metadata& m, const p::Documentation& text)
{
    if(text.summary.size)
    {
        print(" -- ");
        rich_text(m, text.summary);
    }
    if(text.description.size)
    {
        print(" ");
        rich_text(m, text.description);
    }
    if(text.unit.size)
    {
        print(" unit=");
        rich_text(m, text.unit);
    }
    if(text.format.size)
    {
        print(" format=");
        rich_text(m, text.format);
    }
}
const char* rich_kind(p::Kind k)
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
void rich_default(const p::Metadata& m, const p::Entry& e)
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
        print("<too long>");
        return;
    }
    p::detail::Printer printer{m, v, text};
    printer.value(0);
    write(1, text, printer.size);
}
bool rich_help(int fd, const Metadata& api)
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
    print("rich parameters profile=1 schema=");
    number(m.schema_id);
    print(" version=");
    number(m.schema_version);
    print("\n");
    if(binding.constructor)
    {
        print("constructor input=");
        rich_text(m, m.types[m.root].text.name);
        print("\n");
    }
    for(unsigned i = 0; i < binding.member_count; ++i)
    {
        unsigned id = binding.members[i];
        if(!p::binds(binding, api, id))
            return false;
        for(unsigned j = 0; j < api.member_count; ++j)
            if(api.members[j].id == id)
            {
                print("method ");
                print(api.members[j].name);
                print(" input=");
                rich_text(m, m.types[m.root].text.name);
                print("\n");
            }
    }
    for(unsigned i = 0; i < m.type_count; ++i)
    {
        const auto& t = m.types[i];
        print("type ");
        rich_text(m, t.text.name);
        print(" ");
        print(rich_kind(t.kind));
        if(t.kind == p::Kind::Optional || t.kind == p::Kind::List)
        {
            print(" element=");
            rich_text(m, m.types[t.element].text.name);
        }
        if(t.width)
        {
            print(" bits=");
            number(t.width * 8);
        }
        if(t.kind == p::Kind::Unsigned)
        {
            print(" range=");
            number(t.minimum);
            print("..");
            number(t.maximum);
        }
        if(t.kind == p::Kind::String || t.kind == p::Kind::List)
        {
            print(" bound=");
            number(t.bound);
        }
        documentation(m, t.text);
        print("\n");
        for(unsigned j = 0; j < t.count; ++j)
        {
            const auto& e = m.entries[t.first + j];
            print("  ");
            print(t.kind == p::Kind::Enum      ? "value "
                  : t.kind == p::Kind::Variant ? "case "
                                               : "field ");
            rich_text(m, e.text.name);
            print(" id=");
            number(e.id);
            if(e.type != p::no_index)
            {
                print(": ");
                rich_text(m, m.types[e.type].text.name);
                if(t.kind == p::Kind::Record)
                {
                    if(e.default_size)
                    {
                        print(" default=");
                        rich_default(m, e);
                    }
                    else
                        print(m.types[e.type].kind == p::Kind::Optional ? " omitted=null"
                                                                        : " required");
                    if(m.types[e.type].kind == p::Kind::Optional)
                        print(" nullable");
                }
            }
            else if(t.kind == p::Kind::Enum)
            {
                print(" wire=");
                number(e.number);
            }
            else
                print(" unit");
            documentation(m, e.text);
            print("\n");
        }
    }
    print(
        "CLI: field:value field.case options:{limit:10} label:null; quoted strings preserve "
        "spaces.\n");
    return true;
}
int main(int argc, char** argv)
{
    if(argc != 2)
    {
        print("usage: intf /disk/bin/textprocessor\n");
        return 1;
    }
    long fd = open(argv[1], OS1_RIGHT_READ | OS1_RIGHT_SEEK | OS1_RIGHT_INSPECT);
    os1_file_info info{};
    if(fd < 0 || file_info(fd, &info) < 0)
    {
        print("intf: cannot open file\n");
        if(fd >= 0)
            close(fd);
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
        close(fd);
        print("intf: invalid or absent interface metadata\n");
        return 1;
    }
    print("class ");
    print(m.class_name);
    print("\ninterface ");
    print(m.interface_name);
    print(" id=");
    number(m.interface_id);
    print(" version=");
    number(m.interface_version);
    print("\n");
    for(unsigned i = 0; i < m.type_count; ++i)
    {
        const auto& t = m.types[i];
        print("type ");
        number(i + 1);
        print(" ");
        print(t.name);
        print(" size=");
        number(t.size);
        if(t.kind != Record)
        {
            print(" ");
            print(scalar(t.kind));
        }
        print("\n");
        for(unsigned j = 0; j < t.field_count; ++j)
        {
            const auto& f = m.fields[t.first_field + j];
            print("  ");
            print(f.name);
            print(": ");
            print(scalar(f.kind));
            print("[");
            number(f.count);
            print("] offset=");
            number(f.offset);
            print("\n");
        }
    }
    for(unsigned i = 0; i < m.member_count; ++i)
    {
        const auto& a = m.members[i];
        print(a.kind == Method        ? "method "
              : a.kind == PropertyGet ? "property get "
              : a.kind == PropertySet ? "property set "
                                      : "event ");
        print(a.name);
        print(" id=");
        number(a.id);
        print(" input=");
        number(a.input);
        print(" output=");
        number(a.output);
        print(a.flags ? " implemented\n" : " metadata-only\n");
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
        close(fd);
        print("intf: invalid Buffer metadata\n");
        return 1;
    }
    for(unsigned i = 0; i < buffers.count; ++i)
    {
        const auto& b = buffers.members[i];
        print("Buffer method=");
        number(b.member);
        print(" mode=exclusive input=loan reference=attachment,offset,length output=");
        print(b.output == os1::buffer_metadata::return_input ? "return_same\n" : "move_distinct\n");
    }
    os1::constructor::Metadata ctor{};
    const int found = constructor_metadata(fd, ctor);
    bool rich_valid = rich_help(fd, m);
    close(fd);
    if(!rich_valid)
    {
        print("intf: invalid rich parameter metadata\n");
        return 1;
    }
    if(found < 0)
    {
        print("intf: invalid constructor metadata\n");
        return 1;
    }
    if(found)
    {
        print("constructor schema=");
        number(ctor.schema_id);
        print(" version=");
        number(ctor.schema_version);
        print(" size=");
        number(ctor.payload_bytes);
        print("\n");
        for(unsigned i = 0; i < ctor.field_count; ++i)
        {
            const auto& f = ctor.fields[i];
            print("  ");
            print(f.name);
            print(": ");
            print(scalar(f.kind));
            print(" offset=");
            number(f.offset);
            print(" default=");
            number(f.default_value);
            print(" cli=--");
            print(f.option);
            print("\n");
        }
    }
    return 0;
}
