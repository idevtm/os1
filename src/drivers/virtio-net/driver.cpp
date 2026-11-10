// Device-specific virtio-net behavior. The kernel supplies generic queue services.
#include "driver/abi.h"
#include "log_build_identity.hpp"

namespace
{
[[gnu::used, gnu::section(".os1_driver")]] const char identity[] = "virtio-net:" OS1_LOG_IMAGE;
const os1_driver_host* host;
void* transport;
uint8_t* rx;
uint8_t* tx;
uint32_t rx_capacity, tx_capacity;
uint8_t mac[6];
uint32_t tx_done, rx_seen, last_rx_bytes, cache_entries;
struct Arp
{
    uint8_t ip[4];
    uint8_t mac[6];
    bool present;
} cache[8];
uint32_t cache_next;
constexpr uint8_t local_ip[] = {10, 0, 2, 15}, gateway_ip[] = {10, 0, 2, 2};
constexpr unsigned header_bytes = 12, arp_bytes = 42;
void copy(uint8_t* out, const uint8_t* in, unsigned n)
{
    for(unsigned i = 0; i < n; ++i)
        out[i] = in[i];
}
bool equal(const uint8_t* a, const uint8_t* b, unsigned n)
{
    for(unsigned i = 0; i < n; ++i)
        if(a[i] != b[i])
            return false;
    return true;
}
uint16_t be16(const uint8_t* p)
{
    return uint16_t((unsigned(p[0]) << 8) | p[1]);
}
void put16(uint8_t* p, uint16_t n)
{
    p[0] = uint8_t(n >> 8);
    p[1] = uint8_t(n);
}
void remember(const uint8_t* ip, const uint8_t* address)
{
    unsigned index = 8;
    for(unsigned i = 0; i < 8; ++i)
        if(cache[i].present && equal(ip, cache[i].ip, 4))
        {
            index = i;
            break;
        }
    if(index == 8)
        index = cache_next++ % 8;
    if(!cache[index].present)
        __atomic_add_fetch(&cache_entries, 1, __ATOMIC_RELEASE);
    copy(cache[index].ip, ip, 4);
    copy(cache[index].mac, address, 6);
    cache[index].present = true;
}
void interrupt(void*)
{
    uint32_t bytes;
    while(host->complete(transport, 0, &bytes) > 0)
    {
        if(bytes >= header_bytes + 14 && bytes <= rx_capacity)
        {
            const uint8_t* p = rx + header_bytes;
            uint16_t type = be16(p + 12);
            host->received(bytes - header_bytes, type);
            if(bytes >= header_bytes + arp_bytes && type == 0x806 && be16(p + 14) == 1 &&
               be16(p + 16) == 0x800 && p[18] == 6 && p[19] == 4)
            {
                remember(p + 28, p + 22);
                if(be16(p + 20) == 2 && equal(p, mac, 6) && equal(p + 32, mac, 6) &&
                   equal(p + 28, gateway_ip, 4) && equal(p + 38, local_ip, 4))
                {
                    __atomic_store_n(&last_rx_bytes, bytes - header_bytes, __ATOMIC_RELAXED);
                    __atomic_store_n(&rx_seen, 1, __ATOMIC_RELEASE);
                }
            }
        }
        if(!host->submit(transport, 0, rx_capacity))
            break;
    }
    while(host->complete(transport, 1, &bytes) > 0)
        __atomic_store_n(&tx_done, 1, __ATOMIC_RELEASE);
}
int stop()
{
    return transport ? host->stop(transport) : 1;
}
int probe(uint32_t pci_index)
{
    if(transport)
        return 0;  // The first profile admits one instance, without pretending to bind others.
    transport = host->open(pci_index, 6, (UINT64_C(1) << 32) | (UINT64_C(1) << 5));
    if(!transport)
        return 0;
    rx = host->queue(transport, 0, 1, &rx_capacity);
    tx = host->queue(transport, 1, 0, &tx_capacity);
    if(!rx || !tx || rx_capacity < header_bytes + arp_bytes ||
       tx_capacity < header_bytes + arp_bytes || !host->read_config(transport, 0, mac, 6) ||
       !host->interrupt(transport, interrupt, nullptr) ||
       !host->submit(transport, 0, rx_capacity) || !host->start(transport))
    {
        stop();
        return 0;
    }
    host->log("virtio-net: ready (disk module)");
    return 1;
}
bool wait(uint32_t* flag)
{
    uint64_t begin = host->now_ns();
    while(!__atomic_load_n(flag, __ATOMIC_ACQUIRE))
    {
        if(host->now_ns() - begin >= UINT64_C(2000000000))
            return false;
        asm volatile("pause");
    }
    return true;
}
int check()
{
    if(!transport)
        return 0;
    for(unsigned i = 0; i < header_bytes + arp_bytes; ++i)
        tx[i] = 0;
    auto* p = tx + header_bytes;
    for(unsigned i = 0; i < 6; ++i)
        p[i] = 255;
    copy(p + 6, mac, 6);
    put16(p + 12, 0x806);
    put16(p + 14, 1);
    put16(p + 16, 0x800);
    p[18] = 6;
    p[19] = 4;
    put16(p + 20, 1);
    copy(p + 22, mac, 6);
    copy(p + 28, local_ip, 4);
    copy(p + 38, gateway_ip, 4);
    __atomic_store_n(&tx_done, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rx_seen, 0, __ATOMIC_RELEASE);
    if(!host->submit(transport, 1, header_bytes + arp_bytes) || !wait(&tx_done) || !wait(&rx_seen))
    {
        host->log("virtio-net: module ARP check failed");
        return 0;
    }
    // The receive event is published only after a valid reply entered the cache.
    host->checked(__atomic_load_n(&last_rx_bytes, __ATOMIC_ACQUIRE),
                  0x806,
                  __atomic_load_n(&cache_entries, __ATOMIC_ACQUIRE));
    return 1;
}
const os1_driver_exports exports = {OS1_DRIVER_ABI, sizeof(os1_driver_exports), probe, check, stop};
}  // namespace
extern "C" const os1_driver_exports* os1_driver_init(const os1_driver_host* api)
{
    if(!api || api->version != OS1_DRIVER_ABI || api->bytes != sizeof(*api))
        return nullptr;
    host = api;
    return &exports;
}
