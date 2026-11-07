#pragma once
#include <stdint.h>
#define OS1_CONTEXT_VERSION 1u
#define OS1_CONTEXT_NODES 48u
#define OS1_CONTEXT_TEXT_BYTES 2048u
#define OS1_CONTEXT_DEPTH 8u
#define OS1_VALUE_RECORD 1u
#define OS1_VALUE_LIST 2u
#define OS1_VALUE_STRING 3u
#define OS1_VALUE_INTEGER 4u
#define OS1_VALUE_BOOLEAN 5u
// Plain data only. Parent indexes precede children. Text is counted, never a pointer.
struct os1_context_node
{
    uint16_t kind, parent, name, name_bytes;
    uint16_t text, text_bytes;
    uint32_t reserved;
    int64_t integer;
};
struct os1_context_data
{
    uint32_t version, count, text_bytes, reserved;
    struct os1_context_node nodes[OS1_CONTEXT_NODES];
    char text[OS1_CONTEXT_TEXT_BYTES];
};
