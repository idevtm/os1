#pragma once
#include "hardware/test_run.hpp"

namespace os1::hardware::test_run
{
// IO supplies position(), seek(offset), and write(bytes, count).
// A failed stream leaves an incomplete archive; callers must report persistence failure.
template<typename IO>
class ArchiveStream
{
    IO& io;
    const char* name;
    long start;
    unsigned bytes{}, used{};
    bool failed{}, finished{};
    char buffer[4096]{};

    bool flush()
    {
        if(failed)
            return false;
        if(used && !io.write(buffer, used))
            failed = true;
        used = 0;
        return !failed;
    }

public:
    ArchiveStream(IO& output, const char* member) : io(output), name(member), start(io.position())
    {
        char header[512]{};
        failed = start < 0 || !tar_header(name, 0, header) || !io.write(header, sizeof(header));
    }
    bool write(const char* data, unsigned size)
    {
        if(failed || finished || size > 262144 - bytes)
        {
            failed = true;
            return false;
        }
        bytes += size;
        while(size)
        {
            unsigned chunk = size < sizeof(buffer) - used ? size : sizeof(buffer) - used;
            for(unsigned i = 0; i < chunk; ++i)
                buffer[used + i] = data[i];
            used += chunk;
            size -= chunk;
            data += chunk;
            if(used == sizeof(buffer) && !flush())
                return false;
        }
        return true;
    }
    bool finish()
    {
        if(finished || !flush())
            return false;
        finished = true;
        char header[512]{}, padding[512]{};
        unsigned pad = (512 - bytes % 512) % 512;
        if((pad && !io.write(padding, pad)) || !tar_header(name, bytes, header))
            return false;
        long end = io.position();
        return end >= 0 && io.seek(start) && io.write(header, sizeof(header)) && io.seek(end);
    }
};
}  // namespace os1::hardware::test_run
