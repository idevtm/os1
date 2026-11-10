// Generic synchronous block-I/O wrappers and request completion helpers.
#include "storage/block_device.hpp"

#include "ipc/channel.hpp"
#include "proc/thread.hpp"

namespace
{
constexpr uint64_t kInterruptFlag = 1ull << 9;

[[nodiscard]] bool can_sleep_current_thread()
{
#ifdef OS1_HOST_TEST
    return false;
#else
    Thread* thread = current_thread();
    return (nullptr != thread) && (nullptr != thread->process);
#endif
}

[[nodiscard]] uint64_t read_rflags()
{
    uint64_t flags = 0;
#ifndef OS1_HOST_TEST
    asm volatile("pushfq; popq %0" : "=r"(flags));
#endif
    return flags;
}

void cpu_pause()
{
#ifndef OS1_HOST_TEST
    asm volatile("pause" : : : "memory");
#endif
}

void relax_block_wait()
{
    if(0 != (read_rflags() & kInterruptFlag))
    {
#ifndef OS1_HOST_TEST
        asm volatile("hlt" : : : "memory");
#endif
        return;
    }

    cpu_pause();
}

bool submit_sync_request(BlockDevice& device,
                         BlockOperation operation,
                         uint64_t sector,
                         void* buffer,
                         uint32_t sector_count)
{
    for(;;)
    {
        BlockRequest request{};
        request.operation = operation;
        request.sector = sector;
        request.buffer = buffer;
        request.sector_count = sector_count;
        auto submit = operation == BlockOperation::Flush ? device.flush : device.submit;
        if(submit(device, request))
        {
            return block_request_wait(request) && block_request_succeeded(request);
        }
        if(BlockRequestStatus::Busy != request.status)
        {
            return false;
        }

        block_retry_wait(can_sleep_current_thread() ? current_thread() : nullptr,
                         0 != (read_rflags() & kInterruptFlag));
    }
}
}  // namespace

void block_retry_wait(Thread* thread, bool interrupts_enabled)
{
    if(interrupts_enabled && thread && thread->process && !thread->user_mode)
    {
        // Busy has no completion producer. An AP may have no periodic timer.
        kernel_sleep_for(thread, 1'000'000);
        return;
    }
#ifndef OS1_HOST_TEST
    // Syscall kernel frames cannot park through the kernel-thread scheduler path.
    ipi_poll_tlb_shootdown();
#endif
    cpu_pause();
}

void block_request_complete(BlockRequest& request,
                            BlockRequestStatus status,
                            uint32_t bytes_transferred)
{
    request.status = status;
    request.bytes_transferred = bytes_transferred;
    (void)completion_signal(request.completion);
}

bool block_request_wait(BlockRequest& request)
{
    while(!completion_done(request.completion))
    {
        if(can_sleep_current_thread())
        {
            Thread* thread = current_thread();
            if((ThreadState::Blocked != thread->state) ||
               (ThreadWaitReason::BlockIo != thread->wait.reason) ||
               (&request.completion != thread->wait.block_io.completion))
            {
                block_current_thread_on_block_io(&request.completion);
            }
        }

        if(!completion_done(request.completion))
        {
            relax_block_wait();
        }
    }

    asm volatile("" : : : "memory");
    return true;
}

namespace
{
[[nodiscard]] uint32_t chunk_sector_count(const BlockDevice& device, uint32_t remaining)
{
    const uint32_t cap =
        (0u != device.max_sectors_per_request) ? device.max_sectors_per_request : 1u;
    return (remaining < cap) ? remaining : cap;
}
}  // namespace

bool block_read_sync(BlockDevice& device, uint64_t sector, void* buffer, uint32_t sector_count)
{
    if((nullptr == device.submit) || (nullptr == buffer) || (0 == sector_count) ||
       !device.sector_size || sector > device.sector_count ||
       sector_count > device.sector_count - sector)
    {
        return false;
    }

    uint32_t remaining = sector_count;
    uint64_t cursor = sector;
    auto* dst = static_cast<uint8_t*>(buffer);
    while(remaining > 0u)
    {
        const uint32_t batch = chunk_sector_count(device, remaining);
        if(!submit_sync_request(device, BlockOperation::Read, cursor, dst, batch))
        {
            return false;
        }
        cursor += batch;
        dst += static_cast<size_t>(batch) * static_cast<size_t>(device.sector_size);
        remaining -= batch;
    }
    return true;
}

bool block_write_sync(BlockDevice& device,
                      uint64_t sector,
                      const void* buffer,
                      uint32_t sector_count)
{
    if((nullptr == device.submit) || (nullptr == buffer) || (0 == sector_count) ||
       !device.sector_size || sector > device.sector_count ||
       sector_count > device.sector_count - sector)
    {
        return false;
    }

    uint32_t remaining = sector_count;
    uint64_t cursor = sector;
    const auto* src = static_cast<const uint8_t*>(buffer);
    while(remaining > 0u)
    {
        const uint32_t batch = chunk_sector_count(device, remaining);
        if(!submit_sync_request(
               device, BlockOperation::Write, cursor, const_cast<uint8_t*>(src), batch))
        {
            return false;
        }
        cursor += batch;
        src += static_cast<size_t>(batch) * static_cast<size_t>(device.sector_size);
        remaining -= batch;
    }
    return true;
}
bool block_flush_sync(BlockDevice& device)
{
    if(!device.flush)
        return false;
    return submit_sync_request(device, BlockOperation::Flush, 0, nullptr, 0);
}
