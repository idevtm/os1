#include <os1/image_inspection.hpp>
int main(int argc, char** argv)
{
    if(argc != 2)
    {
        os1::user::image_inspection::Output{}.print("usage: intf /bin/textprocessor\n");
        return 1;
    }
    return os1::user::image_inspection::inspect_file(argv[1]);
}
