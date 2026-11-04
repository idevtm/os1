#include "fs/cpio_newc.hpp"

namespace
{
constexpr uint32_t kCpioModeTypeMask = 0170000u;
constexpr uint32_t kCpioModeRegular = 0100000u;
constexpr char kCpioNewcMagic[] = "070701";

struct CpioNewcHeader
{
    char magic[6];
    char inode[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];
    char devmajor[8];
    char devminor[8];
    char rdevmajor[8];
    char rdevminor[8];
    char namesize[8];
    char check[8];
} __attribute__((packed));

[[nodiscard]] bool checked_add(uint64_t left, uint64_t right, uint64_t& result)
{
    if(left > (~0ull - right))
    {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] bool align_up_checked(uint64_t value, uint64_t alignment, uint64_t& result)
{
    if(0 == alignment)
    {
        return false;
    }
    uint64_t biased = 0;
    if(!checked_add(value, alignment - 1, biased))
    {
        return false;
    }
    result = biased & ~(alignment - 1);
    return true;
}

[[nodiscard]] bool magic_matches(const CpioNewcHeader& header)
{
    for(size_t i = 0; i < 6; ++i)
    {
        if(header.magic[i] != kCpioNewcMagic[i])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool parse_hex(const char* text, size_t digits, uint32_t& value)
{
    value = 0;
    if(nullptr == text)
    {
        return false;
    }

    for(size_t i = 0; i < digits; ++i)
    {
        uint32_t nibble = 0;
        if((text[i] >= '0') && (text[i] <= '9'))
        {
            nibble = static_cast<uint32_t>(text[i] - '0');
        }
        else if((text[i] >= 'a') && (text[i] <= 'f'))
        {
            nibble = static_cast<uint32_t>(text[i] - 'a' + 10);
        }
        else if((text[i] >= 'A') && (text[i] <= 'F'))
        {
            nibble = static_cast<uint32_t>(text[i] - 'A' + 10);
        }
        else
        {
            return false;
        }
        value = (value << 4) | nibble;
    }
    return true;
}

const char* normalize_archive_path(const char* path)
{
    if(nullptr == path)
    {
        return nullptr;
    }
    while(('.' == path[0]) && ('/' == path[1]))
    {
        path += 2;
    }
    while('/' == *path)
    {
        ++path;
    }
    return path;
}

[[nodiscard]] bool is_regular_entry(uint32_t mode)
{
    return (mode & kCpioModeTypeMask) == kCpioModeRegular;
}
}  // namespace

bool for_each_cpio_newc_file(const uint8_t* archive,
                             uint64_t archive_size,
                             CpioNewcFileVisitor visitor,
                             void* context)
{
    if((nullptr == archive) || (nullptr == visitor))
    {
        return false;
    }

    uint64_t offset = 0;
    while(offset <= archive_size)
    {
        uint64_t header_end = 0;
        if(!checked_add(offset, sizeof(CpioNewcHeader), header_end) ||
           (header_end > archive_size))
        {
            return false;
        }

        const auto* header = reinterpret_cast<const CpioNewcHeader*>(archive + offset);
        if(!magic_matches(*header))
        {
            return false;
        }

        uint32_t mode = 0;
        uint32_t name_size = 0;
        uint32_t file_size = 0;
        if(!parse_hex(header->mode, 8, mode) || !parse_hex(header->namesize, 8, name_size) ||
           !parse_hex(header->filesize, 8, file_size))
        {
            return false;
        }
        if(0 == name_size)
        {
            return false;
        }

        uint64_t name_end = 0;
        if(!checked_add(header_end, name_size, name_end) || (name_end > archive_size))
        {
            return false;
        }

        const char* name = reinterpret_cast<const char*>(archive + header_end);
        if(0 != name[name_size - 1])
        {
            return false;
        }

        uint64_t file_offset = 0;
        if(!align_up_checked(name_end, 4, file_offset) || (file_offset > archive_size))
        {
            return false;
        }

        uint64_t file_end = 0;
        if(!checked_add(file_offset, file_size, file_end) || (file_end > archive_size))
        {
            return false;
        }

        if(cpio_newc_paths_equal(name, "TRAILER!!!"))
        {
            return true;
        }

        const uint8_t* file_data = archive + file_offset;
        if(is_regular_entry(mode) && !visitor(name, file_data, file_size, context))
        {
            return false;
        }

        uint64_t next_offset = 0;
        if(!align_up_checked(file_end, 4, next_offset) || (next_offset <= offset))
        {
            return false;
        }
        offset = next_offset;
    }

    return false;
}

bool cpio_newc_paths_equal(const char* archive_name, const char* wanted)
{
    const char* normalized_archive = normalize_archive_path(archive_name);
    const char* normalized_wanted = normalize_archive_path(wanted);
    if((nullptr == normalized_archive) || (nullptr == normalized_wanted))
    {
        return false;
    }

    for(size_t index = 0;; ++index)
    {
        if(normalized_archive[index] != normalized_wanted[index])
        {
            return false;
        }
        if(0 == normalized_archive[index])
        {
            return true;
        }
    }
}

void copy_cpio_newc_path(char* destination, size_t destination_size, const char* archive_name)
{
    if((nullptr == destination) || (0 == destination_size))
    {
        return;
    }

    const char* normalized = normalize_archive_path(archive_name);
    size_t index = 0;
    if((nullptr != normalized) && (0 != normalized[0]))
    {
        if(index < (destination_size - 1))
        {
            destination[index++] = '/';
        }
        size_t source_index = 0;
        while(((index + 1) < destination_size) && normalized[source_index])
        {
            destination[index++] = normalized[source_index++];
        }
    }
    while(index < destination_size)
    {
        destination[index++] = 0;
    }
}

bool for_each_cpio_newc_entry(const uint8_t* archive,
                              uint64_t size,
                              CpioNewcEntryVisitor visit,
                              void* context)
{
    if(!archive || !visit)
        return false;
    uint64_t offset = 0;
    while(offset <= size && size - offset >= sizeof(CpioNewcHeader))
    {
        const auto& h = *reinterpret_cast<const CpioNewcHeader*>(archive + offset);
        if(!magic_matches(h))
            return false;
        uint32_t fields[13]{};
        const char* text = reinterpret_cast<const char*>(&h) + 6;
        for(unsigned i = 0; i < 13; ++i)
            if(!parse_hex(text + i * 8, 8, fields[i]))
                return false;
        const uint32_t mode = fields[1], links = fields[4], bytes = fields[6], names = fields[11];
        if(!names || names > 4097 || fields[12])
            return false;
        const uint64_t name_start = offset + sizeof(h);
        if(names > size - name_start)
            return false;
        const char* name = reinterpret_cast<const char*>(archive + name_start);
        if(name[names - 1])
            return false;
        for(uint32_t i = 0; i + 1 < names; ++i)
            if(!name[i])
                return false;
        uint64_t data_start = 0, next = 0;
        if(!align_up_checked(name_start + names, 4, data_start) || data_start > size ||
           bytes > size - data_start || !align_up_checked(data_start + bytes, 4, next) ||
           next > size)
            return false;
        constexpr char trailer[] = "TRAILER!!!";
        bool end = names == sizeof(trailer);
        for(size_t i = 0; end && i < sizeof(trailer); ++i)
            end = name[i] == trailer[i];
        if(end)
        {
            if(bytes || mode)
                return false;
            for(uint64_t i = next; i < size; ++i)
                if(archive[i])
                    return false;
            return true;
        }
        const uint32_t kind = mode & 0170000u;
        if((kind != 0100000u && kind != 0040000u) || (kind == 0100000u && links != 1) ||
           (kind == 0040000u && bytes))
            return false;
        if(!visit(name, names - 1, kind, archive + data_start, bytes, context))
            return false;
        offset = next;
    }
    return false;
}
