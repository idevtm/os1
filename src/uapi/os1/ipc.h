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
