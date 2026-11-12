// Bounded collection of firmware memory ranges for the BootInfo handoff.
#pragma once

#include "handoff/boot_info.hpp"

class BootMemoryMap
{
public:
    explicit BootMemoryMap(std::span<BootMemoryRegion> storage) : storage_(storage) {}

    bool add(const BootMemoryRegion& region)
    {
        if(!region.length)
            return true;
        if(region.length > UINT64_MAX - region.physical_start)
            return false;

        if(count_ && !truncated_)
        {
            auto& previous = storage_[count_ - 1];
            if(previous.type == region.type && previous.attributes == region.attributes)
            {
                if(previous.physical_start + previous.length == region.physical_start)
                {
                    previous.length += region.length;
                    return true;
                }
                if(region.physical_start + region.length == previous.physical_start)
                {
                    previous.physical_start = region.physical_start;
                    previous.length += region.length;
                    return true;
                }
            }
        }
        if(count_ == storage_.size())
        {
            if(!truncated_)
            {
                truncated_ = true;
                for(const auto& retained : storage_)
                    if(!boot_memory_region_is_usable(retained))
                        exclude_reserved_range(retained);
            }
            if(!boot_memory_region_is_usable(region))
                exclude_reserved_range(region);
            return true;
        }
        storage_[count_++] = region;
        return true;
    }

    [[nodiscard]] bool truncated() const
    {
        return truncated_;
    }

    std::span<const BootMemoryRegion> finish() const
    {
        return {storage_.data(), count_};
    }

private:
    void exclude_reserved_range(const BootMemoryRegion& reserved)
    {
        const uint64_t reserved_end = reserved.physical_start + reserved.length;
        for(auto& region : std::span<BootMemoryRegion>(storage_.data(), count_))
        {
            if(!boot_memory_region_is_usable(region))
                continue;
            const uint64_t end = region.physical_start + region.length;
            if(region.physical_start >= reserved_end || end <= reserved.physical_start)
                continue;
            if(region.physical_start < reserved.physical_start)
                region.length = reserved.physical_start - region.physical_start;
            else if(end > reserved_end)
            {
                region.physical_start = reserved_end;
                region.length = end - reserved_end;
            }
            else
                region.type = BootMemoryType::Reserved;
        }
    }

    std::span<BootMemoryRegion> storage_;
    size_t count_ = 0;
    bool truncated_ = false;
};
