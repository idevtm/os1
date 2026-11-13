#include <os1/resource.h>
#include <os1/vfs.h>

#include "driver/gm206.h"
#include "freestanding/string.hpp"

namespace gm206
{
namespace
{
struct Reflection
{
    bool available;
    const char *driver, *description;
    uint32_t width, height, bits_per_pixel, mode_count;
    uint64_t generation;
    const Snapshot* gm206;
    const Identity* identity;
    uint64_t framebuffer_address;
    uint32_t framebuffer_pitch, framebuffer_format;
};
using Gm206Identity = Identity;
constexpr unsigned display_base = OS1_HARDWARE_DISPLAY_ROOT;
constexpr unsigned gm206_root = OS1_HARDWARE_GM206_ROOT;
constexpr unsigned diagnostics = OS1_HARDWARE_GM206_DIAGNOSTICS;
constexpr unsigned gpu_root = display_base + 11;
constexpr unsigned boot_root = display_base + 12;
constexpr unsigned boot_fields = display_base + 0xa00;
constexpr unsigned gpu_fields = display_base + 0x900;
constexpr unsigned edid_fields = display_base + 0x920;
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
        root.text = display.description;
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
        return index == 0
                   ? DisplayNode{display_base, names[0], OS1_HARDWARE_STRING, 0, display.driver}
                   : integer_node(display_base, names[index], values[index]);
    }
    if(id == gm206_root && (display.identity || display.gm206))
        return object_node(display_base, "gm206", outputs_root, display.gm206 ? 5 : 4);
    if(id == diagnostics)
        return {gm206_root, "diagnostics", OS1_HARDWARE_METHOD, 0, "()"};
    if(id == gpu_root)
        return object_node(gm206_root, "gpu", gpu_fields, 17);
    if(id >= gpu_fields && id < gpu_fields + 17)
    {
        const Gm206Identity empty{};
        const auto& gpu = display.identity ? *display.identity : empty;
        if(id == gpu_fields)
            return {gpu_root, "probe_status", OS1_HARDWARE_STRING, 0, gpu.status};
        const char* names[] = {"vendor_id",
                               "device_id",
                               "subsystem_vendor_id",
                               "subsystem_device_id",
                               "pci_revision",
                               "pci_segment",
                               "pci_bus",
                               "pci_slot",
                               "pci_function",
                               "bar0_bytes",
                               "boot0",
                               "display_resources",
                               "aux_present_mask",
                               "aux_enabled_mask",
                               "dpcd_failed_mask",
                               "boot0_read"};
        const auto* probe = display.gm206;
        const uint64_t values[] = {gpu.vendor,
                                   gpu.device,
                                   gpu.subsystem_vendor,
                                   gpu.subsystem_device,
                                   gpu.revision,
                                   gpu.segment,
                                   gpu.bus,
                                   gpu.slot,
                                   gpu.function,
                                   gpu.bar0_bytes,
                                   display.identity ? gpu.boot0
                                   : probe          ? probe->boot0
                                                    : 0,
                                   probe ? probe->resources : 0,
                                   probe ? probe->aux_present_mask : 0u,
                                   probe ? probe->aux_enabled_mask : 0u,
                                   probe ? probe->dpcd_failed_mask : 0u,
                                   gpu.boot0_read || probe};
        const auto field = id - gpu_fields - 1;
        return integer_node(gpu_root, names[field], values[field]);
    }
    if(id == outputs_root)
        return object_node(
            gm206_root, "outputs", output_base, display.gm206 ? display.gm206->output_count : 0);
    if(id == scanouts_root)
        return object_node(
            gm206_root, "scanouts", scanout_base, display.gm206 ? display.gm206->head_count : 0);
    if(!display.gm206)
        return {};
    const auto& boot = display.gm206->boot;
    const char* boot_names[] = {"route_register",
                                "route_value",
                                "route_matched",
                                "sor",
                                "link",
                                "head",
                                "sor_control",
                                "link_config_read",
                                "link_rate_code",
                                "lane_count",
                                "lane_count_raw",
                                "link_status_read",
                                "link_status_raw",
                                "framebuffer_address",
                                "framebuffer_pitch",
                                "framebuffer_format"};
    uint64_t link_status = 0;
    if(boot.link_status_read)
        for(unsigned i = 0; i < sizeof(boot.link_status); ++i)
            link_status |= uint64_t(boot.link_status[i]) << (8 * i);
    const uint64_t boot_values[] = {board_route,
                                    boot.route,
                                    boot.route_matched,
                                    boot.sor,
                                    boot.link,
                                    boot.head_index,
                                    boot.sor_control,
                                    boot.link_config_read,
                                    boot.link_config_read ? boot.link_config[0] : 0u,
                                    boot.link_config_read ? boot.link_config[1] & 31u : 0u,
                                    boot.link_config_read ? boot.link_config[1] : 0u,
                                    boot.link_status_read,
                                    link_status,
                                    display.framebuffer_address,
                                    display.framebuffer_pitch,
                                    display.framebuffer_format};
    constexpr unsigned base_count = sizeof(boot_names) / sizeof(*boot_names);
    constexpr unsigned core_count = sizeof(boot_registers) / sizeof(*boot_registers);
    constexpr unsigned head_count = sizeof(head_registers) / sizeof(*head_registers);
    constexpr unsigned channel_count = sizeof(base_registers) / sizeof(*base_registers);
    const unsigned boot_count =
        base_count + core_count + (boot.route_matched ? head_count + channel_count : 0);
    if(id == boot_root)
        return object_node(gm206_root, "boot_state", boot_fields, boot_count);
    if(id >= boot_fields)
    {
        unsigned field = id - boot_fields;
        if(field >= boot_count)
            return {};
        if(field < base_count)
            return integer_node(boot_root, boot_names[field], boot_values[field]);
        field -= base_count;
        if(field < core_count)
            return integer_node(boot_root, boot_registers[field].name, boot.core[field]);
        field -= core_count;
        if(field < head_count)
            return integer_node(boot_root, head_registers[field].name, boot.head[field]);
        field -= head_count;
        return integer_node(boot_root, base_registers[field].name, boot.base[field]);
    }
    if(id >= edid_fields)
    {
        const unsigned index = (id - edid_fields) / 16, field = (id - edid_fields) % 16;
        if(index >= display.gm206->output_count)
            return {};
        const auto& out = display.gm206->outputs[index];
        const auto parent = output_base + index + 11 * channels;
        if(field == 0)
            return integer_node(parent, "read_blocks", out.edid_read_blocks);
        if(field <= 8 && (field - 1) / 4 < out.edid_read_blocks)
            return {parent, "hex_", OS1_HARDWARE_STRING, 0, "", 0, 0, int(field - 1)};
        return {};
    }
    if(id >= output_base && id < scanout_base)
    {
        const unsigned index = (id - output_base) % channels;
        const unsigned field = (id - output_base) / channels;
        if(index >= display.gm206->output_count)
            return {};
        const auto& out = display.gm206->outputs[index];
        const auto parent = output_base + index;
        switch(field)
        {
            case 0:
                return object_node(outputs_root, "output_", parent + channels, 11, index);
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
                return object_node(parent, "modes", mode_base + index * mode_limit, out.mode_count);
            case 7:
                return {parent,
                        "manufacturer",
                        OS1_HARDWARE_STRING,
                        0,
                        out.edid_blocks ? out.manufacturer : ""};
            case 8:
                return {parent,
                        "monitor_name",
                        OS1_HARDWARE_STRING,
                        0,
                        out.edid_blocks ? out.name : ""};
            case 9:
                return {parent,
                        "monitor_serial_text",
                        OS1_HARDWARE_STRING,
                        0,
                        out.edid_blocks ? out.serial_text : ""};
            case 10:
                return integer_node(
                    parent,
                    "product_code",
                    out.edid_blocks ? unsigned(out.edid[10]) | (unsigned(out.edid[11]) << 8) : 0);
            case 11:
                return object_node(
                    parent, "edid", edid_fields + index * 16, 1 + 4 * out.edid_read_blocks);
            default:
                return {};
        }
    }
    if(id >= scanout_base && id < mode_base)
    {
        const unsigned index = (id - scanout_base) % heads;
        const unsigned field = (id - scanout_base) / heads;
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
            return object_node(scanouts_root, "scanout_", parent + heads, 6, index);
        if(field <= 6)
            return integer_node(parent, names[field - 1], values[field - 1]);
    }
    if(id >= mode_base)
    {
        constexpr auto entries = channels * mode_limit;
        const unsigned slot = (id - mode_base) % entries;
        const unsigned field = (id - mode_base) / entries;
        const unsigned output = slot / mode_limit, index = slot % mode_limit;
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
                output_base + output + 6 * channels, "mode_", parent + entries, 13, index);
        if(field <= 13)
            return integer_node(parent, names[field - 1], values[field - 1]);
    }
    return {};
}

unsigned display_child(unsigned parent, const DisplayNode& node, unsigned index)
{
    if(parent == gpu_root ||
       (parent >= output_base + 11 * channels && parent < output_base + 12 * channels))
        return node.first + index;
    if(parent >= mode_base)
        return node.first + index * channels * mode_limit;
    if(parent >= scanout_base)
        return node.first + index * heads;
    if(parent >= output_base && parent < output_base + channels)
        return node.first + index * channels;
    return node.first + index;
}

void emit_display_node(Os1HardwareNode& output,
                       unsigned id,
                       const DisplayNode& node,
                       const Reflection& display)
{
    output.id = id;
    output.parent = node.parent;
    output.type = node.type;
    output.integer = node.value;
    freestanding::copy_string(output.name, sizeof(output.name), node.name);
    freestanding::copy_string(output.text, sizeof(output.text), node.text);
    if(id >= edid_fields && node.index >= 0)
    {
        const auto& out = display.gm206->outputs[(id - edid_fields) / 16];
        constexpr char digits[] = "0123456789abcdef";
        for(unsigned i = 0; i < 32; ++i)
        {
            const auto byte = out.edid[unsigned(node.index) * 32 + i];
            output.text[2 * i] = digits[byte >> 4];
            output.text[2 * i + 1] = digits[byte & 15];
        }
    }
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

long reflect(Os1HardwareQuery& q,
             const os1_display_summary& summary,
             const Snapshot* snapshot,
             const Identity* identity)
{
    const Reflection display{summary.available,
                             summary.driver,
                             summary.description,
                             summary.width,
                             summary.height,
                             summary.bits_per_pixel,
                             summary.mode_count,
                             summary.generation,
                             snapshot,
                             identity,
                             summary.framebuffer_address,
                             summary.framebuffer_pitch,
                             summary.framebuffer_format};
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
        emit_display_node(q.nodes[q.count++], q.id, node, display);
        q.end = 1;
    }
    else
    {
        const bool append_gm206 = q.id == display_base && (display.identity || display.gm206);
        const unsigned count = node.count + append_gm206;
        if(q.start > count)
            return OS1_ERR_INVALID;
        for(unsigned index = q.start; index < count && q.count < OS1_HARDWARE_PAGE; ++index)
        {
            const auto id = index == node.count ? gm206_root : display_child(q.id, node, index);
            emit_display_node(q.nodes[q.count++], id, describe_display(id, display), display);
        }
        q.end = q.start + q.count == count;
    }
    q.generation = generation;
    q.next = q.start + q.count;
    return 0;
}
}  // namespace gm206
