#pragma once
#include <os1/resource.h>
#include <stdint.h>
#define OS1_IPC_VERSION 1u
#define OS1_IPC_PAYLOAD_MAX 256u
#define OS1_IPC_OPERATION_LIMIT 8u
#define OS1_OBJECT_IPC_CLIENT 4u
#define OS1_OBJECT_IPC_PROVIDER 5u
#define OS1_OBJECT_OPERATION 6u
#define OS1_INTERFACE_IPC 16u
#define OS1_INTERFACE_OPERATION 32u
#define OS1_ERR_PEER_CLOSED (-17L)
#define OS1_ERR_OUTCOME_UNKNOWN (-18L)
#define OS1_ERR_STALE_REPLY (-19L)
#define OS1_IPC_CREATE 1u
#define OS1_IPC_READY 2u
#define OS1_IPC_INFO 3u
#define OS1_IPC_SUBMIT 4u
#define OS1_IPC_RECEIVE 5u
#define OS1_IPC_REPLY 6u
#define OS1_IPC_RESULT 7u
#define OS1_IPC_WAIT 8u
// ipc(operation, fd, argument). CREATE requires fd=0; WAIT requires argument=0.
// Creation returns both CLOEXEC entries or neither. All reserved bytes are zero.
struct os1_ipc_pair
{
    int32_t client, provider;
};
struct os1_ipc_info
{
    uint32_t version, size;
    uint64_t generation, interface_id;
    uint32_t interface_version, ready;
};
// On SUBMIT generation/request_id are zero. RECEIVE returns a scoped receipt.
struct os1_ipc_message
{
    uint32_t version, size;
    uint64_t generation, request_id, interface_id;
    uint32_t interface_version, member, payload_bytes, reserved;
    uint8_t payload[OS1_IPC_PAYLOAD_MAX];
};
// RESULT returns transport_status separately from the provider's application status.
// REPLY requires transport_status=0. Payload tail and reserved fields must be zero.
struct os1_ipc_result
{
    uint32_t version, size;
    uint64_t generation, request_id;
    int32_t transport_status, application_status;
    uint32_t payload_bytes, reserved;
    uint8_t payload[OS1_IPC_PAYLOAD_MAX];
};

#define OS1_IPC_CLOCK 9u
#define OS1_IPC_WAIT_UNTIL 10u
#define OS1_IPC_SUBSCRIBE 11u
#define OS1_IPC_EMIT 12u
#define OS1_IPC_EVENT_READ 13u
#define OS1_IPC_UNSUBSCRIBE 14u
#define OS1_IPC_STOP_CHILD 15u
#define OS1_OBJECT_SUBSCRIPTION 7u
#define OS1_INTERFACE_SUBSCRIPTION 64u
#define OS1_ERR_TIMED_OUT (-20L)
#define OS1_IPC_SUBSCRIPTION_LIMIT 8u
#define OS1_IPC_EVENT_LIMIT 8u
#define OS1_PROVIDER_STOP_STATUS 125
// CLOCK: fd=argument=0, returns monotonic nanoseconds. STOP_CHILD: fd=0,
// argument=child PID; only the parent of a restricted provider is authorized.
struct os1_ipc_deadline
{
    uint32_t version, size;
    uint64_t nanoseconds;
};
struct os1_ipc_subscription
{
    uint32_t version, size, member, reserved;
};
struct os1_ipc_event
{
    uint32_t version, size, member, payload_bytes;
    uint64_t sequence, dropped;
    uint8_t payload[OS1_IPC_PAYLOAD_MAX];
};
// EMIT requires sequence=dropped=0. EVENT_READ returns sequence and lost events
// since the previous successful read. Unsubscribe clears queued notifications.

// Extended operations preserve the original byte-only record ABI.
#define OS1_IPC_SUBMIT_CAPS 16u
#define OS1_IPC_RECEIVE_CAPS 17u
#define OS1_IPC_REPLY_CAPS 18u
#define OS1_IPC_RESULT_CAPS 19u
#define OS1_IPC_CAP_LIMIT 4u
struct os1_ipc_capability
{
    int32_t fd;
    uint32_t rights, descendant_rights, reserved;
};
struct os1_ipc_capabilities
{
    uint32_t count, reserved;
    os1_ipc_capability entries[OS1_IPC_CAP_LIMIT];
};
struct os1_ipc_message_caps
{
    os1_ipc_message message;
    os1_ipc_capabilities capabilities;
};
struct os1_ipc_result_caps
{
    os1_ipc_result result;
    os1_ipc_capabilities capabilities;
};
// Input fds are sender-local and require DELEGATE. Output fds are receiver-local,
// CLOEXEC, and copied (never moved). Unused entries and reserved fields are zero.
// RESULT_CAPS claims attachments once; subsequent reads return count=0.
