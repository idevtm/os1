#include <os1/vfs.hpp>
int main(int argc, char** argv)
{
    if(argc < 2)
    {
        os1::user::write(2, "usage: cat <path>...\n", sizeof("usage: cat <path>...\n") - 1);
        return 1;
    }
    char bytes[1024];
    for(int i = 1; i < argc; ++i)
    {
        const long file = os1::user::open(argv[i], OS1_RIGHT_READ);
        if(file < 0)
        {
            os1::user::write(2, "cat: cannot open file\n", sizeof("cat: cannot open file\n") - 1);
            return 1;
        }
        for(;;)
        {
            long count = os1::user::read(file, bytes, sizeof(bytes));
            if(count < 0)
            {
                os1::user::close(file);
                return 1;
            }
            if(!count)
                break;
            size_t done = 0;
            while(done < static_cast<size_t>(count))
            {
                long written = os1::user::write(1, bytes + done, count - done);
                if(written <= 0)
                {
                    os1::user::close(file);
                    return 1;
                }
                done += written;
            }
        }
        os1::user::close(file);
    }
    return 0;
}
