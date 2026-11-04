#include <os1/vfs.hpp>
static bool output(const char* text, size_t count)
{
    while(count)
    {
        long n = os1::user::write(1, text, count);
        if(n <= 0)
            return false;
        text += n;
        count -= n;
    }
    return true;
}
int main(int argc, char** argv)
{
    if(argc > 2)
    {
        os1::user::write(2, "usage: ls [path]\n", sizeof("usage: ls [path]\n") - 1);
        return 1;
    }
    long directory = os1::user::open(argc == 2 ? argv[1] : ".", OS1_RIGHT_ENUMERATE);
    if(directory < 0)
    {
        os1::user::write(
            2, "ls: cannot open directory\n", sizeof("ls: cannot open directory\n") - 1);
        return 1;
    }
    alignas(8) uint8_t bytes[1024];
    for(;;)
    {
        long size = os1::user::read_directory(directory, bytes, sizeof(bytes));
        if(size < 0)
        {
            os1::user::close(directory);
            return 1;
        }
        if(!size)
            break;
        size_t offset = 0;
        while(offset < static_cast<size_t>(size))
        {
            if(size - offset < sizeof(os1_directory_record))
                return 1;
            auto* record = reinterpret_cast<const os1_directory_record*>(bytes + offset);
            if(record->version != OS1_VFS_ABI_VERSION || record->reserved || !record->name_bytes ||
               record->name_bytes > OS1_NAME_MAX || record->record_bytes % 8 ||
               record->record_bytes < sizeof(*record) + record->name_bytes ||
               record->record_bytes > size - offset)
                return 1;
            const auto* name = bytes + offset + sizeof(*record);
            for(unsigned i = 0; i < record->name_bytes; ++i)
            {
                unsigned c = name[i];
                if(c >= 0x21 && c <= 0x7e && c != '\\')
                {
                    char ch = c;
                    if(!output(&ch, 1))
                        return 1;
                }
                else
                {
                    constexpr char hex[] = "0123456789abcdef";
                    const char escaped[] = {'\\', 'x', hex[c >> 4], hex[c & 15]};
                    if(!output(escaped, 4))
                        return 1;
                }
            }
            if(record->kind == OS1_OBJECT_DIRECTORY && !output("/", 1))
                return 1;
            if(!output("\n", 1))
                return 1;
            offset += record->record_bytes;
        }
    }
    return os1::user::close(directory) < 0;
}
