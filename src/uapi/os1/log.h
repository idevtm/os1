#pragma once
#include <stdint.h>
#define OS1_LOG_INTERFACE_ID UINT64_C(0x4f53314c4f475631)
#define OS1_LOG_VERSION 1u
#define OS1_LOG_READ 1u
#define OS1_LOG_INFO 2u
#define OS1_LOG_CONTEXT_OPEN 1u
#define OS1_LOG_COMMAND_BEGIN 2u
#define OS1_LOG_COMMAND_END 3u
#define OS1_LOG_HISTORY_READ 4u
#define OS1_LOG_CONTEXT_INFO 5u
#define OS1_LOG_HISTORY_BYTES (64u * 1024u)
#define OS1_LOG_HISTORY_TOTAL (256u * 1024u)
#define OS1_LOG_COARSE 1u
#define OS1_LOG_TRUNCATED 2u
#define OS1_LOG_SUPPRESSED 4u
#define OS1_LOG_PENDING 8u
#define OS1_LOG_REPLACED 16u
#define OS1_LOG_INTERACTIVE 32u
#define OS1_LOG_INFO_LEVEL 1u
#define OS1_LOG_WARNING 2u
#define OS1_LOG_ERROR 3u
#define OS1_LOG_EVENT 1u
#define OS1_LOG_SYSCALL_BEGIN 2u
#define OS1_LOG_SYSCALL_END 3u
#define OS1_LOG_IPC_BEGIN 4u
#define OS1_LOG_IPC_END 5u
#define OS1_LOG_COMMAND 6u
#define OS1_LOG_PROCESS_START 7u
#define OS1_LOG_PROCESS_EXIT 8u
#define OS1_LOG_TEXT 9u
struct os1_log_record
{
    uint64_t sequence, time_ns, session_id, command_id, pid, request_id;
    uint64_t interface_id, arg0, arg1, parent_command_id, duration_ns;
    int64_t status;
    uint32_t kind, level, flags, cpu, text_bytes, reserved;
    char text[136];
};
struct os1_log_query
{
    uint32_t version, size;
    uint64_t after, through, command_id, pid;
    uint32_t min_level, kind;
};
struct os1_log_info
{
    uint32_t version, size;
    uint64_t boot_id, latest, overwritten, dropped, capacity_bytes;
    uint64_t clock_resolution_ns, wall_time_ns;
    uint32_t diagnostic, history_slots;
};
struct os1_log_command
{
    uint32_t version, size, flags, reserved;
    int64_t status;
    uint64_t command_id;
    char text[1024], cwd[512];
};
struct os1_log_history_record
{
    struct os1_log_record record;
    char text[1024], cwd[512];
    uint64_t completed_ns;
    uint8_t reserved[248];
};
struct os1_log_history_query
{
    struct os1_log_query query;
    struct os1_log_history_record result;
};
#ifdef __cplusplus
static_assert(sizeof(os1_log_record) == 256);
static_assert(sizeof(os1_log_history_record) == 2048);

#endif
