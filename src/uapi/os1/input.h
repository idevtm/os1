#pragma once
#include <stdint.h>
#define OS1_INPUT_INTERFACE_ID UINT64_C(0x4f5331494e505431)
#define OS1_KEYBOARD_INTERFACE_ID UINT64_C(0x4f53314b45594231)
#define OS1_POINTER_INTERFACE_ID UINT64_C(0x4f5331504f494e31)
#define OS1_INPUT_SUBSCRIPTION_INTERFACE_ID UINT64_C(0x4f53314953554231)
#define OS1_INPUT_VERSION 1u
#define OS1_OBJECT_INPUT_DEVICE 11u
#define OS1_OBJECT_INPUT_SUBSCRIPTION 12u
#define OS1_OBJECT_POINTER 13u
#define OS1_INPUT_LIST 1u
#define OS1_INPUT_OPEN 2u
#define OS1_INPUT_GET_STATE 1u
#define OS1_INPUT_SUBSCRIBE 2u
#define OS1_INPUT_TRY_READ 1u
#define OS1_INPUT_UNSUBSCRIBE 2u
#define OS1_INPUT_KEYBOARD 1u
#define OS1_INPUT_POINTER 2u
#define OS1_INPUT_CHANGED 1u
#define OS1_INPUT_REMOVED 2u
#define OS1_INPUT_LOST 4u
#define OS1_INPUT_COARSE_CLOCK 8u
#define OS1_INPUT_DEVICE_LIMIT 64u
#define OS1_INPUT_EVENT_LIMIT 256u
#define OS1_INPUT_SUBSCRIPTION_LIMIT 128u
struct os1_input_selector
{
    uint64_t device;
};
// list returns the next identity greater than after, or an empty result.
struct os1_input_list
{
    uint64_t after;
};
struct os1_input_device
{
    uint64_t device;
    uint32_t kind, connected, catalog_id, reserved;
    char name[64];
};
// Keys use USB HID usage page 7. Relative pointer totals use modulo-2^64 arithmetic.
struct os1_input_state
{
    uint64_t device, sequence, time_ns;
    uint64_t x, y, wheel;
    uint8_t keys[32];
    uint32_t buttons, connected, flags, reserved;
};
struct os1_input_event
{
    uint64_t sequence, time_ns, device, lost;
    int32_t dx, dy, wheel;
    uint32_t buttons, flags, kind;
    uint8_t keys[32];
};
#ifdef __cplusplus
static_assert(sizeof(os1_input_device) == 88);
static_assert(sizeof(os1_input_state) == 96);
static_assert(sizeof(os1_input_event) == 88);
#endif
