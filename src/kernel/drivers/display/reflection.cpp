#include "drivers/display/reflection.hpp"

#include <os1/resource.h>
#include <os1/vfs.h>

#include "freestanding/string.hpp"

namespace native_display
{
long query_reflection(Os1HardwareQuery& q, const os1_display_summary& display)
{
    constexpr unsigned root = OS1_HARDWARE_DISPLAY_ROOT;
    if(q.version != OS1_HARDWARE_VERSION || q.reserved || q.next || q.count || q.end ||
       (q.operation != OS1_HARDWARE_DESCRIBE && q.operation != OS1_HARDWARE_CHILDREN) ||
       (q.operation == OS1_HARDWARE_DESCRIBE && q.start))
        return OS1_ERR_INVALID;
    for(const auto& node : q.nodes)
    {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&node);
        for(unsigned i = 0; i < sizeof(node); ++i)
            if(bytes[i])
                return OS1_ERR_INVALID;
    }
    if(q.generation && q.generation != display.generation)
        return OS1_ERR_CANCELLED;
    // The root has the availability node. An available display also has five value nodes.
    const unsigned children = display.available ? 6 : 1;
    if(q.id < root || q.id > root + children)
        return OS1_ERR_NOT_FOUND;
    const auto emit = [&](unsigned id) {
        const char* names[] = {
            "display", "available", "driver", "width", "height", "bits_per_pixel", "mode_count"};
        const uint64_t values[] = {0,
                                   display.available,
                                   0,
                                   display.width,
                                   display.height,
                                   display.bits_per_pixel,
                                   display.mode_count};
        auto& node = q.nodes[q.count++];
        const unsigned field = id - root;
        node.id = id;
        node.parent = field ? root : 0;
        node.type = field == 0   ? OS1_HARDWARE_OBJECT
                    : field == 2 ? OS1_HARDWARE_STRING
                                 : OS1_HARDWARE_INTEGER;
        node.integer = values[field];
        freestanding::copy_string(node.name, sizeof(node.name), names[field]);
        freestanding::copy_string(node.text,
                                  sizeof(node.text),
                                  field == 0   ? display.description
                                  : field == 2 ? display.driver
                                               : "");
    };
    if(q.operation == OS1_HARDWARE_DESCRIBE)
    {
        emit(q.id);
        q.end = 1;
    }
    else
    {
        const unsigned count = q.id == root ? children : 0;
        if(q.start > count)
            return OS1_ERR_INVALID;
        for(unsigned index = q.start; index < count && q.count < OS1_HARDWARE_PAGE; ++index)
            emit(root + 1 + index);
        q.end = q.start + q.count == count;
    }
    q.generation = display.generation;
    q.next = q.start + q.count;
    return 0;
}
}  // namespace native_display
