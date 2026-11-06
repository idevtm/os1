#include <os1/vfs.hpp>
int main(int argc, char** argv)
{
    char bytes[1024];
    for(int i = argc < 2 ? 0 : 1; i < argc; ++i)
    {
        const bool standard = !i || (argv[i][0] == '-' && !argv[i][1]);
        const long file = standard ? 0 : os1::user::open(argv[i], OS1_RIGHT_READ);
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
                if(!standard)
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
                    if(!standard)
                        os1::user::close(file);
                    return 1;
                }
                done += written;
            }
        }
        if(!standard)
            os1::user::close(file);
    }
    return 0;
}
