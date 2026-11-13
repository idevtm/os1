#include "drivers/display/reflection.hpp"

#include <os1/resource.h>
#include <os1/vfs.h>

#include "freestanding/string.hpp"

namespace native_display
{
namespace
{
constexpr unsigned display_base = OS1_HARDWARE_DISPLAY_ROOT;
constexpr unsigned gm206_root = display_base + 7;
constexpr unsigned outputs_root = display_base + 8;
constexpr unsigned scanouts_root = display_base + 9;
constexpr unsigned output_base = display_base + 0x20;
constexpr unsigned scanout_base = display_base + 0x140;
constexpr unsigned mode_base = display_base + 0x180;

struct DisplayNode
{
    unsigned parent;
    const char* name;
    unsigned type;
    uint64_t value;
    const char* text;
    unsigned first = 0, count = 0;
    int index = -1;
};

DisplayNode object_node(
    unsigned parent, const char* name, unsigned first, unsigned count, int index = -1)
{
    return {parent, name, OS1_HARDWARE_OBJECT, 0, "", first, count, index};
}

DisplayNode integer_node(unsigned parent, const char* name, uint64_t value)
{
    return {parent, name, OS1_HARDWARE_INTEGER, value, ""};
}

DisplayNode describe_display(unsigned id, const Reflection& display)
{
    const auto available = display.available;
    if(id == display_base)
    {
        auto root = object_node(0, "display", display_base + 1, available ? 6 : 1);
        root.text = display.bochs ? "QEMU runtime display" : "Fixed firmware display";
        return root;
    }
    if(id == display_base + 1)
        return integer_node(display_base, "available", available);
    if(available && id < gm206_root)
    {
        const char* names[] = {"driver", "width", "height", "bits_per_pixel", "mode_count"};
        const uint64_t values[] = {
            0, display.width, display.height, display.bits_per_pixel, display.mode_count};
        const auto index = id - display_base - 2;
        return index == 0 ? DisplayNode{display_base,
                                        names[0],
                                        OS1_HARDWARE_STRING,
                                        0,
                                        display.bochs ? "bochs-display" : "firmware-framebuffer"}
                          : integer_node(display_base, names[index], values[index]);
    }
    if(!display.gm206)
        return {};
    if(id == gm206_root)
        return object_node(display_base, "gm206", outputs_root, 2);
    if(id == outputs_root)
        return object_node(gm206_root, "outputs", output_base, display.gm206->output_count);
    if(id == scanouts_root)
        return object_node(gm206_root, "scanouts", scanout_base, display.gm206->head_count);
    if(id >= output_base && id < scanout_base)
    {
        const unsigned index = (id - output_base) % gm206::channels;
        const unsigned field = (id - output_base) / gm206::channels;
        if(index >= display.gm206->output_count)
            return {};
        const auto& out = display.gm206->outputs[index];
        const auto parent = output_base + index;
        switch(field)
        {
            case 0:
                return object_node(outputs_root, "output_", parent + gm206::channels, 6, index);
            case 1:
                return integer_node(parent, "aux_channel", out.channel);
            case 2:
                return integer_node(parent, "dpcd_revision", out.dpcd_revision);
            case 3:
                return integer_node(parent, "max_link_rate_code", out.max_link_rate);
            case 4:
                return integer_node(parent, "max_lanes", out.max_lanes);
            case 5:
                return integer_node(parent, "edid_blocks", out.edid_blocks);
            case 6:
                return object_node(
                    parent, "modes", mode_base + index * gm206::mode_limit, out.mode_count);
            default:
                return {};
        }
    }
    if(id >= scanout_base && id < mode_base)
    {
        const unsigned index = (id - scanout_base) % gm206::heads;
        const unsigned field = (id - scanout_base) / gm206::heads;
        if(index >= display.gm206->head_count)
            return {};
        const auto& head = display.gm206->scanouts[index];
        const auto parent = scanout_base + index;
        const char* names[] = {
            "head", "sor", "link", "horizontal_total", "vertical_total", "pixel_hz"};
        const unsigned values[] = {head.index,
                                   head.sor,
                                   head.link,
                                   head.horizontal_total,
                                   head.vertical_total,
                                   head.pixel_hz};
        if(field == 0)
            return object_node(scanouts_root, "scanout_", parent + gm206::heads, 6, index);
        if(field <= 6)
            return integer_node(parent, names[field - 1], values[field - 1]);
    }
    if(id >= mode_base)
    {
        constexpr auto entries = gm206::channels * gm206::mode_limit;
        const unsigned slot = (id - mode_base) % entries;
        const unsigned field = (id - mode_base) / entries;
        const unsigned output = slot / gm206::mode_limit, index = slot % gm206::mode_limit;
        if(output >= display.gm206->output_count ||
           index >= display.gm206->outputs[output].mode_count)
            return {};
        const auto& mode = display.gm206->outputs[output].modes[index];
        const unsigned parent = mode_base + slot;
        const char* names[] = {"width",
                               "height",
                               "millihz",
                               "pixel_hz",
                               "preferred",
                               "horizontal_total",
                               "vertical_total",
                               "positive_hsync",
                               "positive_vsync",
                               "horizontal_sync_start",
                               "horizontal_sync_end",
                               "vertical_sync_start",
                               "vertical_sync_end"};
        const unsigned values[] = {mode.width,
                                   mode.height,
                                   mode.millihz,
                                   mode.pixel_hz,
                                   mode.preferred,
                                   mode.horizontal_total,
                                   mode.vertical_total,
                                   mode.positive_hsync,
                                   mode.positive_vsync,
                                   mode.horizontal_sync_start,
                                   mode.horizontal_sync_end,
                                   mode.vertical_sync_start,
                                   mode.vertical_sync_end};
        if(field == 0)
            return object_node(
                output_base + output + 6 * gm206::channels, "mode_", parent + entries, 13, index);
        if(field <= 13)
            return integer_node(parent, names[field - 1], values[field - 1]);
    }
    return {};
}

unsigned display_child(unsigned parent, const DisplayNode& node, unsigned index)
{
    if(parent >= mode_base)
        return node.first + index * gm206::channels * gm206::mode_limit;
    if(parent >= scanout_base)
        return node.first + index * gm206::heads;
    if(parent >= output_base && parent < output_base + gm206::channels)
        return node.first + index * gm206::channels;
    return node.first + index;
}

void emit_display_node(Os1HardwareNode& output, unsigned id, const DisplayNode& node)
{
    output.id = id;
    output.parent = node.parent;
    output.type = node.type;
    output.integer = node.value;
    freestanding::copy_string(output.name, sizeof(output.name), node.name);
    freestanding::copy_string(output.text, sizeof(output.text), node.text);
    if(node.index >= 0)
    {
        unsigned end = 0;
        while(output.name[end])
            ++end;
        if(node.index >= 10)
            output.name[end++] = char('0' + node.index / 10);
        output.name[end++] = char('0' + node.index % 10);
        output.name[end] = 0;
    }
}
}  // namespace

long query_reflection(Os1HardwareQuery& q, const Reflection& display)
{
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
    const auto generation = display.generation;
    if(q.generation && q.generation != generation)
        return OS1_ERR_CANCELLED;
    const auto node = describe_display(q.id, display);
    if(!node.name)
        return OS1_ERR_NOT_FOUND;
    if(q.operation == OS1_HARDWARE_DESCRIBE)
    {
        emit_display_node(q.nodes[q.count++], q.id, node);
        q.end = 1;
    }
    else
    {
        const bool append_gm206 = q.id == display_base && display.gm206 != nullptr;
        const unsigned count = node.count + append_gm206;
        if(q.start > count)
            return OS1_ERR_INVALID;
        for(unsigned index = q.start; index < count && q.count < OS1_HARDWARE_PAGE; ++index)
        {
            const auto id = index == node.count ? gm206_root : display_child(q.id, node, index);
            emit_display_node(q.nodes[q.count++], id, describe_display(id, display));
        }
        q.end = q.start + q.count == count;
    }
    q.generation = generation;
    q.next = q.start + q.count;
    return 0;
}
}  // namespace native_display
