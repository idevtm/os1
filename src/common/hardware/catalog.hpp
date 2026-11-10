#pragma once
#include "hardware/tree.hpp"

namespace os1::hardware
{
// Boot-thread publication only. Queries use this bounded catalog after freeze.
struct Catalog
{
    static constexpr unsigned capacity = 256;
    struct Entry
    {
        unsigned id{};
        Record record{};
    };
    Entry entries[capacity]{};
    unsigned used{}, dropped{}, counts[UsbPort + 1]{};
    bool frozen{};
    unsigned add(unsigned kind, const Record& record)
    {
        if(frozen || kind < UsbDevice || kind > UsbPort)
            return 0;
        if(used == capacity)
        {
            ++dropped;
            return 0;
        }
        unsigned id = object_id(kind, counts[kind]++);
        entries[used++] = {id, record};
        return id;
    }
    Record* edit(unsigned id)
    {
        if(frozen || !id)
            return nullptr;
        for(unsigned i = 0; i < used; ++i)
            if(entries[i].id == id)
                return &entries[i].record;
        return nullptr;
    }
    bool read(unsigned kind, unsigned index, Record& record) const
    {
        unsigned id = object_id(kind, index);
        for(unsigned i = 0; i < used; ++i)
            if(entries[i].id == id)
            {
                record = entries[i].record;
                return true;
            }
        return false;
    }
    int interfaces(unsigned device, const void* bytes, unsigned length)
    {
        if(frozen || !device)
            return 0;
        auto invalid = [&]() {
            ++dropped;
            return -1;
        };
        auto* b = static_cast<const uint8_t*>(bytes);
        if(!b || length < 9 || b[0] < 9 || b[1] != 2)
            return invalid();
        unsigned total = b[2] | unsigned(b[3]) << 8;
        if(total < 9 || total > length)
            return invalid();
        for(unsigned at = 0; at < total;)
        {
            unsigned size = b[at];
            if(size < 2 || size > total - at || (b[at + 1] == 4 && size < 9))
                return invalid();
            at += size;
        }
        unsigned added = 0;
        for(unsigned at = 0; at < total; at += b[at])
        {
            if(b[at + 1] != 4)
                continue;
            Record r{};
            r.backing = device;
            r.known = 0x7f;
            r.category = b[at + 5] == 3   ? OS1_HARDWARE_INPUT
                         : b[at + 5] == 8 ? OS1_HARDWARE_STORAGE
                                          : OS1_HARDWARE_OTHER;
            copy(r.name, sizeof(r.name), "USB interface");
            copy(r.state, sizeof(r.state), "unbound");
            r.values[0] = b[at + 2];
            r.values[1] = b[at + 3];
            r.values[2] = b[at + 5];
            r.values[3] = b[at + 6];
            r.values[4] = b[at + 7];
            r.values[5] = b[at + 4];
            r.values[6] = b[5];
            if(add(UsbInterface, r))
                ++added;
        }
        return added;
    }
    unsigned bind_interface(
        unsigned device, unsigned config, unsigned number, const char* driver, bool ready)
    {
        if(frozen || !device)
            return 0;
        for(unsigned i = 0; i < used; ++i)
        {
            auto& e = entries[i];
            auto& r = e.record;
            if((e.id - record_base) >> 16 != UsbInterface || r.backing != device ||
               r.values[0] != number || r.values[1] || r.values[6] != config)
                continue;
            copy(r.driver, sizeof(r.driver), driver);
            copy(r.state, sizeof(r.state), ready ? "ready" : "failed");
            return e.id;
        }
        return 0;
    }
};
}  // namespace os1::hardware
