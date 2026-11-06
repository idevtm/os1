#include <os1/tmpfs.hpp>
#include <os1/vfs.hpp>
namespace u = os1::user;
namespace fs = os1::user::fs;
bool same(const char* a, const char* b)
{
    while(*a && *a == *b)
    {
        ++a;
        ++b;
    }
    return *a == *b;
}
int fail(const char* operation, long result)
{
    unsigned length = 0;
    while(operation[length])
        ++length;
    u::write(2, operation, length);
    char digits[24];
    unsigned n = 0;
    unsigned long v = result < 0 ? -result : result;
    do
    {
        digits[n++] = char('0' + v % 10);
        v /= 10;
    } while(v);
    u::write(2, " error -", 8);
    while(n)
        u::write(2, &digits[--n], 1);
    u::write(2, "\n", 1);
    return 1;
}
int main(int argc, char** argv)
{
    if(argc != 2)
    {
        u::write(1, "diskutil: seed | verify | sync\n", 31);
        return 1;
    }
    long directory = u::open("/disk", fs::directory_rights, fs::descendant_rights);
    if(directory < 0)
        return fail("open disk", directory);
    long client = fs::bind(directory, true, fs::directory_rights);
    if(client < 0)
        return fail("bind disk", client);
    if(same(argv[1], "sync"))
    {
        os1_ipc_result result{};
        long r = fs::call(client, true, OS1_DIRECTORY_SYNC, nullptr, 0, result);
        if(r < 0)
            return fail("sync", r);
        u::write(1, "diskutil sync ok\n", 17);
        return 0;
    }
    bool seed = same(argv[1], "seed");
    if(!seed && !same(argv[1], "verify"))
        return fail("argument", OS1_ERR_INVALID);
    long file = fs::open(client, "persistent", fs::file_rights, 0, seed ? OS1_OBJECT_FILE : 0);
    if(seed && file == OS1_ERR_EXISTS)
        file = fs::open(client, "persistent", fs::file_rights);
    if(file < 0)
        return fail("open persistent", file);
    long f = fs::bind(file, false, fs::file_rights);
    if(f < 0)
        return fail("bind file", f);
    if(seed)
    {
        long r = fs::truncate(f, 0);
        if(r < 0)
            return fail("truncate", r);
    }
    uint8_t bytes[256];
    // Cross direct/indirect and former tmpfs size limits. Check exact bytes after reboot.
    for(unsigned offset = 0; offset < 131072; offset += sizeof(bytes))
    {
        if(seed)
            for(unsigned i = 0; i < sizeof(bytes); ++i)
                bytes[i] = uint8_t((offset + i) * 37 + (offset + i) / 251);
        long r = seed ? fs::write(f, bytes, sizeof(bytes)) : fs::read(f, bytes, sizeof(bytes));
        if(r != sizeof(bytes))
            return fail(seed ? "write" : "read", r);
        if(!seed)
            for(unsigned i = 0; i < sizeof(bytes); ++i)
                if(bytes[i] != uint8_t((offset + i) * 37 + (offset + i) / 251))
                    return fail("compare", OS1_ERR_IO);
    }
    if(!seed && fs::read(f, bytes, 1) != 0)
        return fail("file length", OS1_ERR_IO);
    if(seed)
    {
        long r = fs::sync(f);
        if(r < 0)
            return fail("sync", r);
    }
    u::write(1, seed ? "diskutil seed ok\n" : "diskutil verify ok\n", seed ? 17 : 19);
    return 0;
}
