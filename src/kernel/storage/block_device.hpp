// Generic block-device request facade above concrete storage drivers.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sync/wait_queue.hpp"

enum class BlockOperation : uint8_t
{
    Read = 0,
    Write = 1,
    Flush = 2,
};

enum class BlockRequestStatus : uint8_t
{
    Pending = 0,
    Success = 1,
    DeviceError = 2,
    Invalid = 3,
    Timeout = 4,
    Busy = 5,
};

struct BlockRequest
{
    BlockOperation operation = BlockOperation::Read;
    uint64_t sector = 0;
    void* buffer = nullptr;
    uint32_t sector_count = 0;
    Completion completion{"block-request"};
    volatile BlockRequestStatus status = BlockRequestStatus::Pending;
    uint32_t bytes_transferred = 0;
    void* driver_context = nullptr;
};

struct BlockDevice;

using BlockSubmit = bool (*)(BlockDevice& device, BlockRequest& request);
using BlockFlush = bool (*)(BlockDevice& device, BlockRequest& request);

struct BlockDevice
{
    const char* name = nullptr;
    uint64_t sector_count = 0;
    uint32_t sector_size = 0;
    uint32_t max_sectors_per_request = 0;
    uint32_t queue_depth = 0;
    void* driver_state = nullptr;
    BlockSubmit submit = nullptr;
    BlockFlush flush = nullptr;
    uint32_t catalog_id = 0;
    bool read_only = false;
};

inline bool block_request_succeeded(const BlockRequest& request)
{
    return completion_done(request.completion) && (BlockRequestStatus::Success == request.status);
}

struct Thread;
// Busy has no completion producer. Only interrupt-enabled kernel workers can park.
void block_retry_wait(Thread*, bool interrupts_enabled);

void block_request_complete(BlockRequest& request,
                            BlockRequestStatus status,
                            uint32_t bytes_transferred = 0);
bool block_request_wait(BlockRequest& request);
bool block_read_sync(BlockDevice& device, uint64_t sector, void* buffer, uint32_t sector_count);
bool block_write_sync(BlockDevice& device,
                      uint64_t sector,
                      const void* buffer,
                      uint32_t sector_count);

bool block_flush_sync(BlockDevice& device);
