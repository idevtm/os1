#include "syscall/resource.hpp"

#include "buffer/buffer.hpp"
#include "mm/user_copy.hpp"
#include "syscall/vfs.hpp"

long sys_resource(
    PageFrameContainer& frames, Thread* thread, uint64_t fd, uint64_t operation, uint64_t argument)
{
    if(!thread || !thread->process)
        return OS1_ERR_INVALID;
    if(fd >= OS1_DESCRIPTOR_LIMIT)
        return OS1_ERR_BAD_DESCRIPTOR;
    if((operation >= OS1_RESOURCE_DUPLICATE_SCOPED && operation <= OS1_RESOURCE_FILE_INFO) ||
       (operation >= OS1_RESOURCE_CONTEXT_OPEN && operation <= OS1_RESOURCE_TREE_STEP))
        return sys_vfs(frames, thread, static_cast<int>(fd), operation, argument);
    if(operation >= OS1_RESOURCE_BUFFER_CREATE && operation <= OS1_RESOURCE_BUFFER_UNMAP)
        return sys_buffer(frames, thread, fd, operation, argument);
    auto& table = thread->process->descriptors;
    switch(operation)
    {
        case OS1_RESOURCE_CLOSE:
            if(argument)
                return OS1_ERR_INVALID;
            return descriptor_close(table, static_cast<int>(fd));
        case OS1_RESOURCE_DUPLICATE: {
            os1_duplicate_request request{};
            if(!copy_from_user(frames, thread, argument, &request, sizeof(request)))
                return OS1_ERR_FAULT;
            return descriptor_duplicate(table, static_cast<int>(fd), request);
        }
        case OS1_RESOURCE_QUERY: {
            os1_resource_info info{};
            const long status = descriptor_query(table, static_cast<int>(fd), info);
            if(status < 0)
                return status;
            return copy_to_user(frames, thread, argument, &info, sizeof(info)) ? 0 : OS1_ERR_FAULT;
        }
        case OS1_RESOURCE_SET_DESCRIPTOR_FLAGS:
            if(argument > UINT32_MAX)
                return OS1_ERR_INVALID;
            return descriptor_set_flags(
                table, static_cast<int>(fd), static_cast<uint32_t>(argument));
        case OS1_RESOURCE_SET_STATUS_FLAGS:
            if(argument > UINT32_MAX)
                return OS1_ERR_INVALID;
            return descriptor_set_status(
                table, static_cast<int>(fd), static_cast<uint32_t>(argument));
        default:
            return OS1_ERR_UNSUPPORTED;
    }
}
