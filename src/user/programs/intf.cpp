#include <os1/vfs.hpp>

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
int main(int argc, char** argv)
{
    if(argc != 2)
    {
        print("usage: intf /bin/textprocessor\n");
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
    os1::constructor::Metadata ctor{};
    const int found = constructor_metadata(fd, ctor);
    close(fd);
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
