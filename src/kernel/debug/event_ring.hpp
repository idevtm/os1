#pragma once

#include <os1/observe.h>
#include <stddef.h>
#include <stdint.h>

namespace kernel_event
{
constexpr uint32_t kEventRingCapacity = OS1_OBSERVE_EVENT_RING_CAPACITY;

class KernelEventRing
{
public:
    void reset();

    Os1ObserveEventRecord append(uint64_t tick_count,
                                 uint32_t type,
                                 uint32_t flags,
                                 uint32_t cpu,
                                 uint64_t pid,
                                 uint64_t tid,
                                 uint64_t arg0,
                                 uint64_t arg1,
                                 uint64_t arg2,
                                 uint64_t arg3);

    [[nodiscard]] uint32_t snapshot(Os1ObserveEventRecord* records, uint32_t max_records) const;

    [[nodiscard]] uint32_t count() const;
    [[nodiscard]] uint64_t next_sequence() const;

private:
    Os1ObserveEventRecord records_[kEventRingCapacity]{};
    uint64_t next_sequence_ = 1;
    uint32_t head_ = 0;
    uint32_t count_ = 0;
};

void record(uint32_t type,
            uint32_t flags = 0,
            uint64_t arg0 = 0,
            uint64_t arg1 = 0,
            uint64_t arg2 = 0,
            uint64_t arg3 = 0);

void record_subject(uint32_t type,
                    uint32_t flags,
                    uint64_t pid,
                    uint64_t tid,
                    uint64_t arg0 = 0,
                    uint64_t arg1 = 0,
                    uint64_t arg2 = 0,
                    uint64_t arg3 = 0);

bool snapshot_all(Os1ObserveEventRecord* records, uint32_t capacity, uint32_t& count);

}  // namespace kernel_event
