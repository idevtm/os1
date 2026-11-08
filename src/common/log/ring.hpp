#pragma once
#include <os1/log.h>
#include <stddef.h>
namespace os1::log
{
// Caller serializes storage. Two independent partitions reserve error capacity.
class Ring
{
    os1_log_record* records_ = nullptr;
    size_t capacity_ = 0, heads_[2]{}, counts_[2]{};
    uint64_t next_ = 1, overwritten_ = 0;
    size_t capacity(unsigned p) const
    {
        return p ? capacity_ / 8 : capacity_ - capacity_ / 8;
    }
    size_t offset(unsigned p) const
    {
        return p ? capacity(0) : 0;
    }
    const os1_log_record& at(unsigned p, size_t n) const
    {
        return records_[offset(p) + (heads_[p] + capacity(p) - counts_[p] + n) % capacity(p)];
    }

public:
    void initialize(os1_log_record* records, size_t count)
    {
        records_ = records;
        capacity_ = count >= 8 ? count : 0;
        heads_[0] = heads_[1] = counts_[0] = counts_[1] = 0;
        next_ = 1;
        overwritten_ = 0;
    }
    uint64_t latest() const
    {
        return next_ - 1;
    }
    uint64_t overwritten() const
    {
        return overwritten_;
    }
    size_t bytes() const
    {
        return capacity_ * sizeof(os1_log_record);
    }
    bool append(os1_log_record record)
    {
        if(!capacity_ || next_ == UINT64_MAX)
            return false;
        unsigned p = record.level >= OS1_LOG_WARNING;
        record.sequence = next_++;
        records_[offset(p) + heads_[p]] = record;
        heads_[p] = (heads_[p] + 1) % capacity(p);
        if(counts_[p] < capacity(p))
            ++counts_[p];
        else
            ++overwritten_;
        return true;
    }
    // Find a bounded next record. Filters are evaluated by the caller so each
    // query takes logarithmic time while holding the capture lock.
    bool next(uint64_t after, uint64_t through, os1_log_record& out) const
    {
        const os1_log_record* best = nullptr;
        for(unsigned p = 0; p < 2; ++p)
        {
            size_t lo = 0, hi = counts_[p];
            while(lo < hi)
            {
                size_t mid = lo + (hi - lo) / 2;
                if(at(p, mid).sequence <= after)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if(lo == counts_[p])
                continue;
            const auto& r = at(p, lo);
            if(r.sequence <= through && (!best || r.sequence < best->sequence))
                best = &r;
        }
        if(!best)
            return false;
        out = *best;
        return true;
    }
};
inline bool matches(const os1_log_record& r, const os1_log_query& q)
{
    return (!q.command_id || r.command_id == q.command_id) && (!q.pid || r.pid == q.pid) &&
           r.level >= q.min_level && (!q.kind || r.kind == q.kind);
}
inline bool valid(const os1_log_query& q)
{
    return q.version == 1 && q.size == sizeof(q) && q.min_level <= OS1_LOG_ERROR &&
           q.kind <= OS1_LOG_TEXT && (!q.through || q.after <= q.through);
}
}  // namespace os1::log
