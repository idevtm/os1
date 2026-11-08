#include "platform/acpica_ec.hpp"

#include "sync/atomic.hpp"
extern "C"
{
// ACPICA internal headers require its public types first.
// clang-format off
#include "acpi.h"
#include "accommon.h"
#include "acevents.h"
#include "acnamesp.h"
    // clang-format on
}
namespace
{
constexpr unsigned kMaxControllers = 4;
constexpr unsigned kPollBudget = 10000;  // At most 100 ms of explicit polling delay per access.
struct Controller
{
    ACPI_HANDLE device{};
    uint16_t data{}, command{};
    volatile bool busy{};
    ACPI_STATUS fault{};
};
Controller controllers[kMaxControllers];
unsigned count;
bool initialized;
ACPI_STATUS initialization_status;
const char* initialization_error = "none";
#if defined(OS1_HOST_TEST)
AcpicaEcTestIo test_io{};
#endif
ACPI_STATUS read_port(uint16_t port, uint8_t& value)
{
#if defined(OS1_HOST_TEST)
    if(test_io.read)
        return test_io.read(test_io.context, port, value);
#endif
    UINT32 data = 0;
    const auto status = AcpiOsReadPort(port, &data, 8);
    value = static_cast<uint8_t>(data);
    return status;
}
void stall(unsigned microseconds)
{
#if defined(OS1_HOST_TEST)
    if(test_io.stall)
    {
        test_io.stall(test_io.context, microseconds);
        return;
    }
#endif
    AcpiOsStall(microseconds);
}
ACPI_STATUS write_port(uint16_t port, uint8_t value)
{
    ACPI_STATUS status;
#if defined(OS1_HOST_TEST)
    if(test_io.write)
        status = test_io.write(test_io.context, port, value);
    else
#endif
        status = AcpiOsWritePort(port, value, 8);
    // The interface may take one microsecond to assert IBF after a host write.
    stall(1);
    return status;
}
ACPI_STATUS wait_status(Controller& ec, uint8_t mask, bool set, unsigned& budget)
{
    for(;;)
    {
        uint8_t value = 0;
        auto status = read_port(ec.command, value);
        if(ACPI_FAILURE(status))
            return status;
        if(bool(value & mask) == set)
            return AE_OK;
        if(!budget)
            return AE_TIME;
        --budget;
        stall(10);
    }
}
ACPI_STATUS transfer(
    Controller& ec, unsigned function, uint8_t address, uint8_t& value, unsigned& budget)
{
    auto status = wait_status(ec, 2, false, budget);
    if(ACPI_FAILURE(status))
        return status;
    // Drain stale responses without allowing an unresponsive device to trap us.
    for(unsigned attempt = 0;; ++attempt)
    {
        uint8_t flags = 0, discarded = 0;
        status = read_port(ec.command, flags);
        if(ACPI_FAILURE(status))
            return status;
        if(!(flags & 1))
            break;
        if(attempt == 16)
            return AE_TIME;
        status = read_port(ec.data, discarded);
        if(ACPI_FAILURE(status))
            return status;
    }
    status = write_port(ec.command, function == ACPI_READ ? 0x80 : 0x81);
    if(ACPI_FAILURE(status))
        return status;
    status = wait_status(ec, 2, false, budget);
    if(ACPI_FAILURE(status))
        return status;
    status = write_port(ec.data, address);
    if(ACPI_FAILURE(status))
        return status;
    if(function == ACPI_READ)
    {
        status = wait_status(ec, 1, true, budget);
        return ACPI_FAILURE(status) ? status : read_port(ec.data, value);
    }
    status = wait_status(ec, 2, false, budget);
    if(ACPI_FAILURE(status))
        return status;
    status = write_port(ec.data, value);
    return ACPI_FAILURE(status) ? status : wait_status(ec, 2, false, budget);
}
ACPI_STATUS access(
    Controller& ec, unsigned function, uint64_t address, unsigned width, uint64_t& value)
{
    if((function != ACPI_READ && function != ACPI_WRITE) ||
       (width != 8 && width != 16 && width != 32 && width != 64) || address > 255 ||
       width / 8 > 256 - address)
        return AE_BAD_PARAMETER;
    bool expected = false;
    if(!atomic_compare_exchange_strong(&ec.busy, &expected, true))
        return AE_TIME;
    struct Release
    {
        Controller& ec;
        ~Release()
        {
            atomic_store_release(&ec.busy, false);
        }
    } release{ec};
    if(ACPI_FAILURE(ec.fault))
        return ec.fault;
    uint64_t result = 0;
    unsigned budget = kPollBudget;
    for(unsigned byte = 0; byte < width / 8; ++byte)
    {
        uint8_t data = static_cast<uint8_t>(value >> (8 * byte));
        auto status = transfer(ec, function, static_cast<uint8_t>(address + byte), data, budget);
        if(ACPI_FAILURE(status))
        {
            ec.fault = status;  // Protocol state can be uncertain; require reboot before retry.
            return status;
        }
        result |= uint64_t(data) << (8 * byte);
    }
    if(function == ACPI_READ)
        value = result;
    return AE_OK;
}
ACPI_STATUS region_access(UINT32 function,
                          ACPI_PHYSICAL_ADDRESS address,
                          UINT32 width,
                          UINT64* value,
                          void* context,
                          void*)
{
    if(!value || !context)
        return AE_BAD_PARAMETER;
    uint64_t data = *value;
    const auto status = access(*static_cast<Controller*>(context), function, address, width, data);
    if(ACPI_SUCCESS(status) && function == ACPI_READ)
        *value = data;
    return status;
}
ACPI_STATUS region_setup(ACPI_HANDLE, UINT32 function, void* context, void** region_context)
{
    *region_context = function == ACPI_REGION_ACTIVATE ? context : nullptr;
    return AE_OK;
}
ACPI_STATUS fail(ACPI_STATUS status, const char* detail)
{
    initialization_error = detail;
    return status;
}
ACPI_STATUS discover(ACPI_HANDLE device, UINT32, void*, void**)
{
    ACPI_OBJECT state{};
    ACPI_BUFFER state_buffer{sizeof(state), &state};
    char sta_name[] = "_STA";
    auto state_status = AcpiEvaluateObject(device, sta_name, nullptr, &state_buffer);
    if(state_status != AE_NOT_FOUND)
    {
        if(ACPI_FAILURE(state_status))
            return fail(state_status, "device-status-query");
        if(state.Type != ACPI_TYPE_INTEGER)
            return fail(AE_TYPE, "device-status-type");
        if(!(state.Integer.Value & ACPI_STA_DEVICE_ENABLED))
            return AE_OK;
    }
    if(count == kMaxControllers)
        return fail(AE_LIMIT, "too-many-controllers");
    ACPI_OBJECT global_lock{};
    ACPI_BUFFER glk_buffer{sizeof(global_lock), &global_lock};
    char glk_name[] = "_GLK";
    auto status = AcpiEvaluateObject(device, glk_name, nullptr, &glk_buffer);
    if(status != AE_NOT_FOUND)
    {
        if(ACPI_FAILURE(status))
            return fail(status, "global-lock-query");
        if(global_lock.Type != ACPI_TYPE_INTEGER)
            return fail(AE_TYPE, "global-lock-type");
        if(global_lock.Integer.Value)
            return fail(AE_SUPPORT, "global-lock-required");
    }
    alignas(ACPI_RESOURCE) uint8_t bytes[512]{};
    ACPI_BUFFER resources{sizeof(bytes), bytes};
    status = AcpiGetCurrentResources(device, &resources);
    if(ACPI_FAILURE(status))
        return fail(status, "resources-query");
    if(resources.Length > sizeof(bytes))
        return fail(AE_BAD_DATA, "resources-length");
    uint16_t ports[2]{};
    unsigned port_count = 0;
    bool ended = false;
    for(size_t offset = 0; offset + ACPI_RS_SIZE_NO_DATA <= resources.Length;)
    {
        auto* resource = reinterpret_cast<ACPI_RESOURCE*>(bytes + offset);
        if(resource->Length < ACPI_RS_SIZE_NO_DATA ||
           resource->Length > resources.Length - offset ||
           resource->Length % alignof(ACPI_RESOURCE))
            return fail(AE_BAD_DATA, "resources-length");
        if(resource->Type == ACPI_RESOURCE_TYPE_END_TAG)
        {
            ended = true;
            break;
        }
        uint16_t port = 0;
        if(resource->Type == ACPI_RESOURCE_TYPE_IO)
        {
            if(resource->Length < ACPI_RS_SIZE(ACPI_RESOURCE_IO) ||
               resource->Data.Io.Minimum != resource->Data.Io.Maximum ||
               resource->Data.Io.AddressLength != 1)
                return fail(AE_BAD_DATA, "invalid-io-resource");
            port = resource->Data.Io.Minimum;
        }
        else if(resource->Type == ACPI_RESOURCE_TYPE_FIXED_IO)
        {
            if(resource->Length < ACPI_RS_SIZE(ACPI_RESOURCE_FIXED_IO) ||
               resource->Data.FixedIo.AddressLength != 1)
                return fail(AE_BAD_DATA, "invalid-fixed-io-resource");
            port = resource->Data.FixedIo.Address;
        }
        else if(resource->Type != ACPI_RESOURCE_TYPE_IRQ &&
                resource->Type != ACPI_RESOURCE_TYPE_EXTENDED_IRQ)
            return fail(AE_SUPPORT, "unsupported-resource");
        if(resource->Type == ACPI_RESOURCE_TYPE_IO || resource->Type == ACPI_RESOURCE_TYPE_FIXED_IO)
        {
            if(!port || port_count == 2)
                return fail(AE_BAD_DATA, "invalid-port-count");
            ports[port_count++] = port;
        }
        offset += resource->Length;
    }
    if(!ended || port_count != 2 || ports[0] == ports[1])
        return fail(AE_BAD_DATA, "invalid-ports");
    for(unsigned i = 0; i < count; ++i)
        for(auto port : ports)
            if(port == controllers[i].data || port == controllers[i].command)
                return fail(AE_ALREADY_EXISTS, "overlapping-ports");
    auto& ec = controllers[count++];
    ec.device = device;
    ec.data = ports[0];
    ec.command = ports[1];
    return AE_OK;
}
struct Connection
{
    ACPI_HANDLE device;
    bool direct_region{};
};
ACPI_STATUS connect_region(ACPI_HANDLE handle, UINT32, void* context, void**)
{
    // Preserve ACPICA's region connection bookkeeping, but propagate AML errors.
    auto* node = AcpiNsValidateHandle(handle);
    if(!node)
        return AE_BAD_PARAMETER;
    auto* object = AcpiNsGetAttachedObject(node);
    if(!object || object->Region.SpaceId != ACPI_ADR_SPACE_EC)
        return AE_OK;
    auto& connection = *static_cast<Connection*>(context);
    if(node->Parent == connection.device)
        connection.direct_region = true;
    return AcpiEvExecuteRegMethod(object, ACPI_REG_CONNECT);
}
ACPI_STATUS connect_regions(ACPI_HANDLE device)
{
    Connection connection{device};
    auto status = AcpiWalkNamespace(
        ACPI_TYPE_REGION, device, ACPI_UINT32_MAX, connect_region, nullptr, &connection, nullptr);
    if(ACPI_FAILURE(status) || connection.direct_region)
        return status;
    // Match ACPICA's EC orphan _REG behavior, retaining its evaluation status.
    ACPI_HANDLE method{};
    char name[] = "_REG";
    status = AcpiGetHandle(device, name, &method);
    if(status == AE_NOT_FOUND)
        return AE_OK;
    if(ACPI_FAILURE(status))
        return status;
    ACPI_OBJECT arguments[2]{};
    arguments[0].Type = arguments[1].Type = ACPI_TYPE_INTEGER;
    arguments[0].Integer.Value = ACPI_ADR_SPACE_EC;
    arguments[1].Integer.Value = ACPI_REG_CONNECT;
    ACPI_OBJECT_LIST list{2, arguments};
    return AcpiEvaluateObject(method, nullptr, &list, nullptr);
}
}  // namespace
uint32_t acpica_ec_initialize()
{
    if(initialized)
        return initialization_status;
    initialized = true;
    initialization_error = "device-discovery";
    char hid[] = "PNP0C09";
    initialization_status = AcpiGetDevices(hid, discover, nullptr, nullptr);
    if(ACPI_FAILURE(initialization_status))
        return initialization_status;
    for(unsigned i = 0; i < count; ++i)
    {
        initialization_status = AcpiInstallAddressSpaceHandlerNo_Reg(
            controllers[i].device, ACPI_ADR_SPACE_EC, region_access, region_setup, &controllers[i]);
        if(ACPI_FAILURE(initialization_status))
            return fail(initialization_status, "handler-install");
    }
    for(unsigned i = 0; i < count; ++i)
    {
        initialization_status = connect_regions(controllers[i].device);
        if(ACPI_FAILURE(initialization_status))
            return fail(initialization_status, "region-connect");
        if(ACPI_FAILURE(controllers[i].fault))
        {
            initialization_status = controllers[i].fault;
            return fail(initialization_status, "region-connect-transport");
        }
    }
    initialization_error = "none";
    return AE_OK;
}
const char* acpica_ec_last_error()
{
    return initialization_error;
}
void acpica_ec_reset()
{
    count = 0;
    initialized = false;
    initialization_status = AE_OK;
    initialization_error = "none";
    for(auto& ec : controllers)
        ec = {};
#if defined(OS1_HOST_TEST)
    test_io = {};
#endif
}
#if defined(OS1_HOST_TEST)
void acpica_ec_set_test_io(AcpicaEcTestIo io)
{
    test_io = io;
}
uint32_t acpica_ec_test_access(
    unsigned index, unsigned function, uint64_t address, unsigned width, uint64_t& value)
{
    return index < count ? access(controllers[index], function, address, width, value)
                         : AE_NOT_EXIST;
}
#endif
