// Direct-map-backed slab allocator for kernel small objects.
#include "mm/kmem.hpp"

#include "debug/event_ring.hpp"
#include "debug/debug.hpp"
#include "handoff/memory_layout.h"
#include "sync/smp.hpp"
#include "util/align.hpp"
#include "util/memory.h"

#if !defined(OS1_HOST_TEST)
#include "core/panic.hpp"
#endif

[[nodiscard]] constexpr uint64_t align_up_constexpr(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

#if defined(OS1_KMEM_DEBUG)
constexpr bool kKmemDebugEnabled = true;
#else
constexpr bool kKmemDebugEnabled = false;
#endif

constexpr uint16_t kKmemDebugRedzoneBytes = 8;
constexpr size_t kKmemDebugBitmapWordCount = 8;
constexpr size_t kKmemDebugBitmapBitCount = kKmemDebugBitmapWordCount * 64u;
constexpr uint8_t kKmemAllocPoisonByte = 0xA5u;
constexpr uint8_t kKmemFreePoisonByte = 0x5Au;
constexpr uint8_t kKmemRedzoneByte = 0xFDu;
constexpr uint8_t kKmemPagePoisonByte = 0xDDu;

[[nodiscard]] constexpr uint16_t kmem_payload_offset_for_layout(uint16_t alignment)
{
    return kKmemDebugEnabled
               ? static_cast<uint16_t>(align_up_constexpr(kKmemDebugRedzoneBytes, alignment))
               : 0u;
}

[[nodiscard]] constexpr uint16_t kmem_slot_stride_for_layout(uint16_t slot_size,
                                                             uint16_t alignment,
                                                             uint16_t payload_offset)
{
    return kKmemDebugEnabled
               ? static_cast<uint16_t>(align_up_constexpr(
                     static_cast<uint64_t>(payload_offset) + slot_size + kKmemDebugRedzoneBytes,
                     alignment))
               : slot_size;
}

enum class SlabListKind : uint8_t
{
    None = 0,
    Empty,
    Partial,
    Full,
};

struct SlabPageHeader
{
    uint32_t magic = 0;
    uint16_t object_size = 0;
    uint16_t object_count = 0;
    uint16_t free_count = 0;
    uint16_t first_object_offset = 0;
    uint16_t slot_stride = 0;
    uint16_t payload_offset = 0;
    uint8_t list_kind = static_cast<uint8_t>(SlabListKind::None);
    uint8_t reserved = 0;
    uint64_t physical_page = 0;
    KmemCache* cache = nullptr;
    void* free_list = nullptr;
    SlabPageHeader* prev = nullptr;
    SlabPageHeader* next = nullptr;
#if defined(OS1_KMEM_DEBUG)
    uint64_t allocation_bitmap[kKmemDebugBitmapWordCount] = {};
#endif
};

struct KmemCache
{
    constexpr KmemCache()
        : name(nullptr),
          object_size(0),
          slot_size(0),
          payload_offset(0),
          slot_stride(0),
          alignment(0),
          lock("kmem-cache"),
          builtin(false),
          active(false)
    {
    }

    constexpr KmemCache(const char* cache_name, uint16_t size, uint16_t cache_alignment)
        : name(cache_name),
          object_size(size),
          slot_size(size),
          payload_offset(kmem_payload_offset_for_layout(cache_alignment)),
          slot_stride(kmem_slot_stride_for_layout(
              size,
              cache_alignment,
              kmem_payload_offset_for_layout(cache_alignment))),
          alignment(cache_alignment),
          lock(cache_name),
          builtin(true),
          active(true)
    {
    }

    const char* name;
    uint16_t object_size;
    uint16_t slot_size;
    uint16_t payload_offset;
    uint16_t slot_stride;
    uint16_t alignment;
    Spinlock lock;
    bool builtin;
    bool active;
    SlabPageHeader* empty = nullptr;
    SlabPageHeader* partial = nullptr;
    SlabPageHeader* full = nullptr;
    uint32_t slab_count = 0;
    uint32_t live_objects = 0;
    uint32_t peak_live_objects = 0;
    uint64_t alloc_count = 0;
    uint64_t free_count = 0;
    uint64_t failed_alloc_count = 0;
    uint64_t slab_growth_count = 0;
    uint64_t slab_return_count = 0;
    uintptr_t debug_last_alloc_caller = 0;
    KmemCache* registry_next = nullptr;
};

namespace
{
constexpr uint32_t kSlabMagic = 0x4B4D454Du;
constexpr uint32_t kLargeAllocationMagic = 0x4B4D454Cu;
constexpr size_t kKeepEmptySlabsPerCache = 1;
constexpr size_t kMinKmallocAlignment = 16;
constexpr size_t kMaxSlabObjectSize = 1024;
constexpr size_t kMaxNamedCacheAlignment = 64;
constexpr size_t kMaxNamedCaches = 32;

enum class AllocationKind : uint8_t
{
    None = 0,
    Slab,
    Large,
};

static_assert(sizeof(SlabPageHeader) < kPageSize);

struct LargeAllocationHeader
{
    uint32_t magic = 0;
    uint32_t page_count = 0;
    uint16_t payload_offset = 0;
    uint16_t reserved = 0;
    uint64_t physical_base = 0;
    size_t requested_size = 0;
    size_t usable_size = 0;
};

static_assert(sizeof(LargeAllocationHeader) < kPageSize);

using CacheLockGuard = IrqSpinGuard<Spinlock>;

constexpr uint16_t cache_alignment_for_size(uint16_t object_size)
{
    if(object_size < kMinKmallocAlignment)
    {
        return kMinKmallocAlignment;
    }
    if(object_size > 64u)
    {
        return 64u;
    }
    return object_size;
}

[[nodiscard]] bool add_overflow_size(size_t left, size_t right, size_t& result)
{
    return __builtin_add_overflow(left, right, &result);
}

[[nodiscard]] bool multiply_overflow_size(size_t left, size_t right, size_t& result)
{
    return __builtin_mul_overflow(left, right, &result);
}

[[nodiscard]] uint64_t allocation_page_base(const void* ptr)
{
    return align_down(reinterpret_cast<uint64_t>(ptr), static_cast<uint64_t>(kPageSize));
}

[[nodiscard]] AllocationKind allocation_kind_from_magic(uint32_t magic)
{
    if(kSlabMagic == magic)
    {
        return AllocationKind::Slab;
    }
    if(kLargeAllocationMagic == magic)
    {
        return AllocationKind::Large;
    }
    return AllocationKind::None;
}

[[nodiscard]] uint16_t large_payload_offset()
{
    return static_cast<uint16_t>(
        align_up(sizeof(LargeAllocationHeader), static_cast<uint64_t>(kMinKmallocAlignment)));
}

[[nodiscard]] void* large_payload(LargeAllocationHeader& header)
{
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&header) + header.payload_offset);
}

[[nodiscard]] const void* large_payload(const LargeAllocationHeader& header)
{
    return reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(&header) +
                                         header.payload_offset);
}

[[nodiscard]] bool large_layout_for_size(size_t requested_size,
                                         uint32_t& page_count,
                                         uint16_t& payload_offset,
                                         size_t& usable_size)
{
    payload_offset = large_payload_offset();

    size_t total_bytes = 0;
    if(add_overflow_size(requested_size, payload_offset, total_bytes))
    {
        return false;
    }

    size_t rounded_total = 0;
    if(add_overflow_size(total_bytes, kPageSize - 1u, rounded_total))
    {
        return false;
    }

    const size_t page_count_size = rounded_total / kPageSize;
    if((0u == page_count_size) || (page_count_size > static_cast<size_t>(~0u)))
    {
        return false;
    }

    page_count = static_cast<uint32_t>(page_count_size);
    usable_size = page_count_size * kPageSize - payload_offset;
    return usable_size >= requested_size;
}

OS1_BSP_ONLY PageFrameContainer* g_kmem_frames = nullptr;
OS1_BSP_ONLY bool g_kmem_ready = false;
Spinlock g_kmem_stats_lock{"kmem-stats"};
OS1_LOCKED_BY(g_kmem_stats_lock) KmemGlobalStats g_kmem_global_stats{};
OS1_BSP_ONLY Spinlock g_named_cache_registry_lock{"kmem-cache-registry"};
OS1_BSP_ONLY KmemCache* g_named_cache_head = nullptr;
OS1_BSP_ONLY size_t g_named_cache_count = 0;
OS1_BSP_ONLY KmemCache g_builtin_caches[] = {
    KmemCache{"kmalloc-16", 16, cache_alignment_for_size(16)},
    KmemCache{"kmalloc-32", 32, cache_alignment_for_size(32)},
    KmemCache{"kmalloc-64", 64, cache_alignment_for_size(64)},
    KmemCache{"kmalloc-128", 128, cache_alignment_for_size(128)},
    KmemCache{"kmalloc-256", 256, cache_alignment_for_size(256)},
    KmemCache{"kmalloc-512", 512, cache_alignment_for_size(512)},
    KmemCache{"kmalloc-1024", 1024, cache_alignment_for_size(1024)},
};
OS1_BSP_ONLY KmemCache g_named_cache_slots[kMaxNamedCaches] = {};

[[nodiscard]] constexpr size_t builtin_cache_count()
{
    return sizeof(g_builtin_caches) / sizeof(g_builtin_caches[0]);
}

[[nodiscard]] constexpr size_t pointer_alignment()
{
    return alignof(void*);
}

[[nodiscard]] bool is_power_of_two(size_t value)
{
    return (0u != value) && (0u == (value & (value - 1u)));
}

[[nodiscard]] bool compute_slot_size(size_t object_size, size_t alignment, uint16_t& slot_size)
{
    const uint64_t padded = align_up(static_cast<uint64_t>(object_size), static_cast<uint64_t>(alignment));
    if((0u == padded) || (padded > static_cast<uint64_t>(UINT16_MAX)))
    {
        return false;
    }

    slot_size = static_cast<uint16_t>(padded);
    return slot_size >= sizeof(void*);
}

[[nodiscard]] bool compute_cache_layout(size_t object_size,
                                        size_t alignment,
                                        uint16_t& slot_size,
                                        uint16_t& payload_offset,
                                        uint16_t& slot_stride)
{
    if(!compute_slot_size(object_size, alignment, slot_size))
    {
        return false;
    }

    payload_offset = kmem_payload_offset_for_layout(static_cast<uint16_t>(alignment));
    const uint64_t stride = kKmemDebugEnabled
                                ? align_up(static_cast<uint64_t>(payload_offset) + slot_size +
                                               kKmemDebugRedzoneBytes,
                                           static_cast<uint64_t>(alignment))
                                : slot_size;
    if((0u == stride) || (stride > static_cast<uint64_t>(UINT16_MAX)))
    {
        return false;
    }

    slot_stride = static_cast<uint16_t>(stride);
    return slot_stride >= slot_size;
}

[[nodiscard]] bool named_cache_parameters_valid(const char* name,
                                                size_t object_size,
                                                size_t alignment,
                                                uint16_t& cache_object_size,
                                                uint16_t& cache_slot_size,
                                                uint16_t& cache_payload_offset,
                                                uint16_t& cache_slot_stride,
                                                uint16_t& cache_alignment)
{
    if((nullptr == name) || ('\0' == *name) || (0u == object_size) || (object_size > static_cast<size_t>(UINT16_MAX)))
    {
        return false;
    }

    if(0u == alignment)
    {
        alignment = pointer_alignment();
    }

    if((alignment < pointer_alignment()) || (alignment > kMaxNamedCacheAlignment) || !is_power_of_two(alignment))
    {
        return false;
    }

    if(object_size < sizeof(void*))
    {
        return false;
    }

    cache_object_size = static_cast<uint16_t>(object_size);
    cache_alignment = static_cast<uint16_t>(alignment);
    if(!compute_cache_layout(
           object_size, alignment, cache_slot_size, cache_payload_offset, cache_slot_stride))
    {
        return false;
    }

    const uint64_t first_object_offset = align_up(sizeof(SlabPageHeader), static_cast<uint64_t>(cache_alignment));
    const uint64_t object_count =
        (kPageSize > first_object_offset) ? ((kPageSize - first_object_offset) / cache_slot_stride) : 0u;
    return (first_object_offset < kPageSize) && (object_count > 0u) &&
           (!kKmemDebugEnabled || (object_count <= kKmemDebugBitmapBitCount));
}

void configure_named_cache(KmemCache& cache,
                           const char* name,
                           uint16_t object_size,
                           uint16_t slot_size,
                           uint16_t payload_offset,
                           uint16_t slot_stride,
                           uint16_t alignment)
{
    cache.name = name;
    cache.object_size = object_size;
    cache.slot_size = slot_size;
    cache.payload_offset = payload_offset;
    cache.slot_stride = slot_stride;
    cache.alignment = alignment;
    cache.builtin = false;
    cache.active = true;
    cache.debug_last_alloc_caller = 0;
    cache.registry_next = nullptr;
}

void reset_named_cache_slot(KmemCache& cache)
{
    cache.name = nullptr;
    cache.object_size = 0;
    cache.slot_size = 0;
    cache.payload_offset = 0;
    cache.slot_stride = 0;
    cache.alignment = 0;
    cache.builtin = false;
    cache.active = false;
    cache.registry_next = nullptr;
    cache.empty = nullptr;
    cache.partial = nullptr;
    cache.full = nullptr;
    cache.slab_count = 0;
    cache.live_objects = 0;
    cache.peak_live_objects = 0;
    cache.alloc_count = 0;
    cache.free_count = 0;
    cache.failed_alloc_count = 0;
    cache.slab_growth_count = 0;
    cache.slab_return_count = 0;
    cache.debug_last_alloc_caller = 0;
}

[[nodiscard]] KmemCache* named_cache_from_index_locked(size_t index)
{
    KmemCache* cache = g_named_cache_head;
    while((nullptr != cache) && (index > 0u))
    {
        cache = cache->registry_next;
        --index;
    }
    return cache;
}

void detach_named_cache_locked(KmemCache* cache)
{
    if(nullptr == cache)
    {
        return;
    }

    KmemCache* previous = nullptr;
    KmemCache* current = g_named_cache_head;
    while(nullptr != current)
    {
        if(current == cache)
        {
            if(nullptr != previous)
            {
                previous->registry_next = current->registry_next;
            }
            else
            {
                g_named_cache_head = current->registry_next;
            }
            current->registry_next = nullptr;
            if(0u != g_named_cache_count)
            {
                --g_named_cache_count;
            }
            return;
        }

        previous = current;
        current = current->registry_next;
    }
}

[[nodiscard]] bool named_cache_is_registered_locked(const KmemCache* cache)
{
    const KmemCache* current = g_named_cache_head;
    while(nullptr != current)
    {
        if(current == cache)
        {
            return true;
        }
        current = current->registry_next;
    }
    return false;
}

void record_cache_failure(KmemCache& cache)
{
    CacheLockGuard guard(cache.lock);
    ++cache.failed_alloc_count;
}

[[nodiscard]] bool has_flag(KmallocFlags value, KmallocFlags flag)
{
    return 0u != (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag));
}

[[noreturn]] void kmem_halt()
{
#if defined(OS1_HOST_TEST)
    __builtin_trap();
#else
    halt_forever();
#endif
}

void increment_global_stat(uint64_t KmemGlobalStats::* counter)
{
    CacheLockGuard guard(g_kmem_stats_lock);
    ++(g_kmem_global_stats.*counter);
}

void record_slab_page_release()
{
    CacheLockGuard guard(g_kmem_stats_lock);
    if(0u != g_kmem_global_stats.slab_page_count)
    {
        --g_kmem_global_stats.slab_page_count;
    }
}

[[noreturn]] void kmem_corruption(const char* reason,
                                 const void* ptr,
                                 uint64_t header_address = 0,
                                 uint64_t physical_base = 0,
                                 const char* cache_name = nullptr,
                                 uint32_t expected_magic = 0,
                                 uint32_t observed_magic = 0)
{
    increment_global_stat(&KmemGlobalStats::corruption_count);
    kernel_event::record(OS1_KERNEL_EVENT_KMEM_CORRUPTION,
                         OS1_KERNEL_EVENT_FLAG_FAILURE,
                         reinterpret_cast<uint64_t>(ptr),
                         header_address,
                         physical_base,
                         (static_cast<uint64_t>(expected_magic) << 32) | observed_magic);
    debug("kmem: corruption: ")(reason)(" ptr=0x")(reinterpret_cast<uint64_t>(ptr), 16, 16)();
    if(0u != header_address)
    {
        debug("kmem: header=0x")(header_address, 16, 16)();
    }
    if(0u != physical_base)
    {
        debug("kmem: phys=0x")(physical_base, 16, 16)();
    }
    if(nullptr != cache_name)
    {
        debug("kmem: cache=")(cache_name)();
    }
    if((0u != expected_magic) || (0u != observed_magic))
    {
        debug("kmem: magic expected=0x")(expected_magic, 16, 8)(" observed=0x")(
            observed_magic, 16, 8)();
    }
    kmem_halt();
}

[[nodiscard]] uintptr_t capture_debug_caller()
{
#if defined(OS1_KMEM_DEBUG) && (defined(__clang__) || defined(__GNUC__))
    return reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#else
    return 0;
#endif
}

[[nodiscard]] uint8_t* slab_slot_base(SlabPageHeader& slab, uint16_t index)
{
    return reinterpret_cast<uint8_t*>(&slab) + slab.first_object_offset +
           static_cast<size_t>(index) * slab.slot_stride;
}

[[nodiscard]] const uint8_t* slab_slot_base(const SlabPageHeader& slab, uint16_t index)
{
    return reinterpret_cast<const uint8_t*>(&slab) + slab.first_object_offset +
           static_cast<size_t>(index) * slab.slot_stride;
}

[[nodiscard]] uint8_t* slab_slot_payload(SlabPageHeader& slab, uint16_t index)
{
    return slab_slot_base(slab, index) + slab.payload_offset;
}

[[nodiscard]] const uint8_t* slab_slot_payload(const SlabPageHeader& slab, uint16_t index)
{
    return slab_slot_base(slab, index) + slab.payload_offset;
}

[[nodiscard]] bool slot_index_for_pointer(const SlabPageHeader& slab, const void* ptr, uint16_t& index)
{
    const uintptr_t slab_address = reinterpret_cast<uintptr_t>(&slab);
    const uintptr_t object_address = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t first_payload = slab_address + slab.first_object_offset + slab.payload_offset;
    const uintptr_t slab_end = slab_address + kPageSize;
    if((object_address < first_payload) || (object_address >= slab_end))
    {
        return false;
    }

    const uintptr_t delta = object_address - first_payload;
    if((delta % slab.slot_stride) != 0u)
    {
        return false;
    }

    const uintptr_t slot_index = delta / slab.slot_stride;
    if(slot_index >= slab.object_count)
    {
        return false;
    }

    index = static_cast<uint16_t>(slot_index);
    return true;
}

void poison_physical_pages(uint64_t physical_base, uint32_t page_count)
{
#if defined(OS1_KMEM_DEBUG)
    memset(kernel_physical_pointer<uint8_t>(physical_base),
           kKmemPagePoisonByte,
           static_cast<uint64_t>(page_count) * kPageSize);
#else
    (void)physical_base;
    (void)page_count;
#endif
}

#if defined(OS1_KMEM_DEBUG)
[[nodiscard]] bool debug_slot_is_allocated(const SlabPageHeader& slab, uint16_t index)
{
    return (slab.allocation_bitmap[index / 64u] & (1ull << (index % 64u))) != 0u;
}

void debug_mark_slot_allocated(SlabPageHeader& slab, uint16_t index)
{
    slab.allocation_bitmap[index / 64u] |= (1ull << (index % 64u));
}

void debug_mark_slot_free(SlabPageHeader& slab, uint16_t index)
{
    slab.allocation_bitmap[index / 64u] &= ~(1ull << (index % 64u));
}

void debug_fill_redzones(SlabPageHeader& slab, uint16_t index)
{
    uint8_t* base = slab_slot_base(slab, index);
    uint8_t* payload = slab_slot_payload(slab, index);
    uint8_t* payload_end = payload + slab.object_size;
    uint8_t* slot_end = base + slab.slot_stride;
    if(payload > base)
    {
        memset(base, kKmemRedzoneByte, static_cast<size_t>(payload - base));
    }
    if(slot_end > payload_end)
    {
        memset(payload_end, kKmemRedzoneByte, static_cast<size_t>(slot_end - payload_end));
    }
}

[[nodiscard]] bool debug_verify_redzones(const SlabPageHeader& slab, uint16_t index)
{
    const uint8_t* base = slab_slot_base(slab, index);
    const uint8_t* payload = slab_slot_payload(slab, index);
    const uint8_t* payload_end = payload + slab.object_size;
    const uint8_t* slot_end = base + slab.slot_stride;
    for(const uint8_t* current = base; current < payload; ++current)
    {
        if(*current != kKmemRedzoneByte)
        {
            return false;
        }
    }
    for(const uint8_t* current = payload_end; current < slot_end; ++current)
    {
        if(*current != kKmemRedzoneByte)
        {
            return false;
        }
    }
    return true;
}

void debug_initialize_free_slot(SlabPageHeader& slab, uint16_t index, void* next)
{
    debug_fill_redzones(slab, index);
    uint8_t* payload = slab_slot_payload(slab, index);
    memset(payload, kKmemFreePoisonByte, slab.object_size);
    *reinterpret_cast<void**>(payload) = next;
    debug_mark_slot_free(slab, index);
}
#endif

[[nodiscard]] void* kmem_fail(const char* reason,
                              size_t size,
                              KmallocFlags flags,
                              bool large = false)
{
    {
        CacheLockGuard guard(g_kmem_stats_lock);
        ++g_kmem_global_stats.failed_alloc_count;
        if(large)
        {
            ++g_kmem_global_stats.failed_large_alloc_count;
        }
    }
    if(has_flag(flags, KmallocFlags::PanicOnFail))
    {
        debug("kmem: allocation failed: ")(reason)(" size=")(static_cast<uint64_t>(size))();
        kmem_halt();
    }
    return nullptr;
}

[[nodiscard]] SlabListKind slab_kind(const SlabPageHeader& slab)
{
    return static_cast<SlabListKind>(slab.list_kind);
}

[[nodiscard]] SlabPageHeader*& slab_list_head(KmemCache& cache, SlabListKind kind)
{
    switch(kind)
    {
        case SlabListKind::Empty:
            return cache.empty;
        case SlabListKind::Partial:
            return cache.partial;
        case SlabListKind::Full:
            return cache.full;
        case SlabListKind::None:
            kmem_corruption("invalid slab list", &cache, reinterpret_cast<uint64_t>(&cache), 0, cache.name);
    }

    kmem_corruption("invalid slab list", &cache, reinterpret_cast<uint64_t>(&cache), 0, cache.name);
}

void detach_slab(KmemCache& cache, SlabPageHeader* slab)
{
    const SlabListKind kind = slab_kind(*slab);
    if(SlabListKind::None == kind)
    {
        return;
    }

    SlabPageHeader*& head = slab_list_head(cache, kind);
    if(nullptr != slab->prev)
    {
        slab->prev->next = slab->next;
    }
    else
    {
        head = slab->next;
    }

    if(nullptr != slab->next)
    {
        slab->next->prev = slab->prev;
    }

    slab->prev = nullptr;
    slab->next = nullptr;
    slab->list_kind = static_cast<uint8_t>(SlabListKind::None);
}

void attach_slab(KmemCache& cache, SlabPageHeader* slab, SlabListKind kind)
{
    SlabPageHeader*& head = slab_list_head(cache, kind);
    slab->prev = nullptr;
    slab->next = head;
    if(nullptr != head)
    {
        head->prev = slab;
    }
    head = slab;
    slab->list_kind = static_cast<uint8_t>(kind);
}

void move_slab(KmemCache& cache, SlabPageHeader* slab, SlabListKind kind)
{
    if(slab_kind(*slab) == kind)
    {
        return;
    }

    detach_slab(cache, slab);
    attach_slab(cache, slab, kind);
}

[[nodiscard]] KmemCache* select_cache(size_t size)
{
    for(size_t index = 0; index < builtin_cache_count(); ++index)
    {
        if(size <= g_builtin_caches[index].object_size)
        {
            return &g_builtin_caches[index];
        }
    }
    return nullptr;
}

[[nodiscard]] SlabPageHeader* select_slab(KmemCache& cache)
{
    if(nullptr != cache.partial)
    {
        return cache.partial;
    }
    return cache.empty;
}

[[nodiscard]] SlabPageHeader* initialize_slab_page(KmemCache& cache, uint64_t physical_page)
{
    auto* slab = kernel_physical_pointer<SlabPageHeader>(physical_page);
    memset(slab, 0, kPageSize);

    const uint64_t first_object_offset = align_up(sizeof(SlabPageHeader), cache.alignment);
    const uint64_t object_count = (kPageSize - first_object_offset) / cache.slot_stride;
    if((first_object_offset >= kPageSize) || (0u == object_count) ||
       (kKmemDebugEnabled && (object_count > kKmemDebugBitmapBitCount)))
    {
        return nullptr;
    }

    slab->magic = kSlabMagic;
    slab->object_size = cache.slot_size;
    slab->object_count = static_cast<uint16_t>(object_count);
    slab->free_count = static_cast<uint16_t>(object_count);
    slab->first_object_offset = static_cast<uint16_t>(first_object_offset);
    slab->slot_stride = cache.slot_stride;
    slab->payload_offset = cache.payload_offset;
    slab->physical_page = physical_page;
    slab->cache = &cache;

    auto* object_base = reinterpret_cast<uint8_t*>(slab) + first_object_offset;
    void* free_list = nullptr;
    for(uint16_t index = slab->object_count; index > 0; --index)
    {
        const uint16_t slot_index = static_cast<uint16_t>(index - 1u);
        void* object = object_base + static_cast<size_t>(slot_index) * cache.slot_stride +
                       cache.payload_offset;
#if defined(OS1_KMEM_DEBUG)
        debug_initialize_free_slot(*slab, slot_index, free_list);
#else
        *reinterpret_cast<void**>(object) = free_list;
#endif
        free_list = object;
    }
    slab->free_list = free_list;
    return slab;
}

[[nodiscard]] bool grow_cache(KmemCache& cache)
{
    if(nullptr == g_kmem_frames)
    {
        return false;
    }

    uint64_t physical_page = 0;
    if(!g_kmem_frames->allocate(physical_page))
    {
        return false;
    }

    SlabPageHeader* slab = initialize_slab_page(cache, physical_page);
    if(nullptr == slab)
    {
        (void)g_kmem_frames->free(physical_page);
        return false;
    }

    CacheLockGuard guard(cache.lock);
    attach_slab(cache, slab, SlabListKind::Empty);
    ++cache.slab_count;
    ++cache.slab_growth_count;
    increment_global_stat(&KmemGlobalStats::slab_page_count);
    return true;
}

[[nodiscard]] bool pointer_in_slab(const SlabPageHeader& slab, const void* ptr)
{
    uint16_t index = 0;
    return slot_index_for_pointer(slab, ptr, index);
}

[[nodiscard]] bool pointer_on_free_list(const SlabPageHeader& slab, const void* ptr)
{
    void* current = slab.free_list;
    size_t visited = 0;
    while((nullptr != current) && (visited < slab.object_count))
    {
        uint16_t current_index = 0;
        if(!slot_index_for_pointer(slab, current, current_index))
        {
            kmem_corruption("free list pointer invalid",
                            current,
                            reinterpret_cast<uint64_t>(&slab),
                            slab.physical_page,
                            (nullptr != slab.cache) ? slab.cache->name : nullptr);
        }
#if defined(OS1_KMEM_DEBUG)
        if(debug_slot_is_allocated(slab, current_index))
        {
            kmem_corruption("allocated object on free list",
                            current,
                            reinterpret_cast<uint64_t>(&slab),
                            slab.physical_page,
                            (nullptr != slab.cache) ? slab.cache->name : nullptr);
        }
#endif
        if(current == ptr)
        {
            return true;
        }
        current = *reinterpret_cast<void**>(current);
        ++visited;
    }
    if(nullptr != current)
    {
        kmem_corruption("free list cycle",
                        ptr,
                        reinterpret_cast<uint64_t>(&slab),
                        slab.physical_page,
                        (nullptr != slab.cache) ? slab.cache->name : nullptr);
    }
    return false;
}

[[nodiscard]] bool pointer_is_large_allocation(const LargeAllocationHeader& header, const void* ptr)
{
    return large_payload(header) == ptr;
}

void snapshot_list(const SlabPageHeader* head,
                   uint32_t& slab_count,
                   uint32_t& total_object_count,
                   uint32_t& free_object_count)
{
    while(nullptr != head)
    {
        ++slab_count;
        total_object_count += head->object_count;
        free_object_count += head->free_count;
        head = head->next;
    }
}

[[nodiscard]] bool allocation_usable_size_internal(const void* ptr, size_t& size)
{
    if((nullptr == ptr) || !g_kmem_ready)
    {
        return false;
    }

    const uint64_t page_base = allocation_page_base(ptr);
    const auto kind = allocation_kind_from_magic(*reinterpret_cast<const uint32_t*>(page_base));
    switch(kind)
    {
        case AllocationKind::Slab:
        {
            const auto* slab = reinterpret_cast<const SlabPageHeader*>(page_base);
            if((nullptr == slab->cache) || !pointer_in_slab(*slab, ptr))
            {
                return false;
            }
            size = slab->object_size;
            return true;
        }
        case AllocationKind::Large:
        {
            const auto* large = reinterpret_cast<const LargeAllocationHeader*>(page_base);
            if(!pointer_is_large_allocation(*large, ptr))
            {
                return false;
            }
            size = large->usable_size;
            return true;
        }
        case AllocationKind::None:
            return false;
    }

    return false;
}

[[nodiscard]] void* allocate_large(size_t size, KmallocFlags flags)
{
    if(has_flag(flags, KmallocFlags::NoGrow) || has_flag(flags, KmallocFlags::Atomic))
    {
        return kmem_fail("large allocations require growth", size, flags, true);
    }

    uint32_t page_count = 0;
    uint16_t payload_offset = 0;
    size_t usable_size = 0;
    if(!large_layout_for_size(size, page_count, payload_offset, usable_size))
    {
        return kmem_fail("large allocation overflow", size, flags, true);
    }

    uint64_t physical_base = 0;
    if(!g_kmem_frames->allocate(physical_base, static_cast<unsigned>(page_count)))
    {
        return kmem_fail("out of physical pages", size, flags, true);
    }

    auto* header = kernel_physical_pointer<LargeAllocationHeader>(physical_base);
    if(has_flag(flags, KmallocFlags::Zero))
    {
        memset(header, 0, static_cast<uint64_t>(page_count) * kPageSize);
    }
    else
    {
        memset(header, 0, sizeof(*header));
    }

    header->magic = kLargeAllocationMagic;
    header->page_count = page_count;
    header->payload_offset = payload_offset;
    header->physical_base = physical_base;
    header->requested_size = size;
    header->usable_size = usable_size;

#if defined(OS1_KMEM_DEBUG)
    if(!has_flag(flags, KmallocFlags::Zero))
    {
        memset(large_payload(*header), kKmemAllocPoisonByte, usable_size);
    }
#endif

    {
        CacheLockGuard guard(g_kmem_stats_lock);
        ++g_kmem_global_stats.live_large_allocation_count;
        g_kmem_global_stats.live_large_allocation_bytes += size;
        ++g_kmem_global_stats.alloc_count;
    }
    return large_payload(*header);
}

void release_large_allocation(LargeAllocationHeader* header)
{
    if((nullptr == header) || (0u == header->page_count))
    {
        increment_global_stat(&KmemGlobalStats::invalid_free_count);
        kmem_corruption("invalid large allocation header",
                        header,
                        reinterpret_cast<uint64_t>(header),
                        (nullptr != header) ? header->physical_base : 0,
                        "large");
    }

    {
        CacheLockGuard guard(g_kmem_stats_lock);
        if(0u != g_kmem_global_stats.live_large_allocation_count)
        {
            --g_kmem_global_stats.live_large_allocation_count;
        }
        uint64_t& live_bytes = g_kmem_global_stats.live_large_allocation_bytes;
        live_bytes =
            (live_bytes < header->requested_size) ? 0u : live_bytes - header->requested_size;
        ++g_kmem_global_stats.free_count;
    }

    const uint64_t physical_base = header->physical_base;
    const uint32_t page_count = header->page_count;
#if defined(OS1_KMEM_DEBUG)
    poison_physical_pages(physical_base, page_count);
#else
    memset(header, 0, sizeof(*header));
#endif
    for(uint32_t page = 0; page < page_count; ++page)
    {
        if(!g_kmem_frames->free(physical_base + static_cast<uint64_t>(page) * kPageSize))
        {
            kmem_corruption("large allocation page free failed",
                            reinterpret_cast<void*>(large_payload_offset()),
                            reinterpret_cast<uint64_t>(header),
                            physical_base,
                            "large");
        }
    }
}

[[nodiscard]] void* allocate_from_slab_locked(KmemCache& cache,
                                              SlabPageHeader* slab,
                                              uintptr_t caller)
{
    if((nullptr == slab) || (nullptr == slab->free_list) || (0u == slab->free_count))
    {
        kmem_corruption("empty slab allocation",
                        slab,
                        reinterpret_cast<uint64_t>(slab),
                        (nullptr != slab) ? slab->physical_page : 0,
                        cache.name);
    }

    void* object = slab->free_list;
    uint16_t slot_index = 0;
    if(!slot_index_for_pointer(*slab, object, slot_index))
    {
        kmem_corruption("free list head invalid",
                        object,
                        reinterpret_cast<uint64_t>(slab),
                        slab->physical_page,
                        cache.name);
    }
#if defined(OS1_KMEM_DEBUG)
    if(debug_slot_is_allocated(*slab, slot_index))
    {
        kmem_corruption("allocated free list head",
                        object,
                        reinterpret_cast<uint64_t>(slab),
                        slab->physical_page,
                        cache.name);
    }
    if(!debug_verify_redzones(*slab, slot_index))
    {
        kmem_corruption("redzone corruption",
                        object,
                        reinterpret_cast<uint64_t>(slab),
                        slab->physical_page,
                        cache.name);
    }
#endif
    slab->free_list = *reinterpret_cast<void**>(object);
    --slab->free_count;

    ++cache.alloc_count;
    ++cache.live_objects;
    if(cache.peak_live_objects < cache.live_objects)
    {
        cache.peak_live_objects = cache.live_objects;
    }
#if defined(OS1_KMEM_DEBUG)
    debug_mark_slot_allocated(*slab, slot_index);
    cache.debug_last_alloc_caller = caller;
#endif
    increment_global_stat(&KmemGlobalStats::alloc_count);

    const SlabListKind target = (0u == slab->free_count) ? SlabListKind::Full : SlabListKind::Partial;
    move_slab(cache, slab, target);
    return object;
}

[[nodiscard]] bool free_object_locked(KmemCache& cache, SlabPageHeader* slab, void* ptr)
{
    uint16_t slot_index = 0;
    if(!slot_index_for_pointer(*slab, ptr, slot_index))
    {
        increment_global_stat(&KmemGlobalStats::invalid_free_count);
        kmem_corruption("pointer outside slab",
                        ptr,
                        reinterpret_cast<uint64_t>(slab),
                        slab->physical_page,
                        cache.name);
    }

#if defined(OS1_KMEM_DEBUG)
    if(!debug_slot_is_allocated(*slab, slot_index))
    {
        increment_global_stat(&KmemGlobalStats::invalid_free_count);
        kmem_corruption("double free",
                        ptr,
                        reinterpret_cast<uint64_t>(slab),
                        slab->physical_page,
                        cache.name);
    }
    if(!debug_verify_redzones(*slab, slot_index))
    {
        kmem_corruption("redzone corruption",
                        ptr,
                        reinterpret_cast<uint64_t>(slab),
                        slab->physical_page,
                        cache.name);
    }
#endif

    if(pointer_on_free_list(*slab, ptr))
    {
        increment_global_stat(&KmemGlobalStats::invalid_free_count);
        kmem_corruption("double free",
                        ptr,
                        reinterpret_cast<uint64_t>(slab),
                        slab->physical_page,
                        cache.name);
    }

    if(0u == cache.live_objects)
    {
        kmem_corruption("live object underflow",
                        ptr,
                        reinterpret_cast<uint64_t>(slab),
                        slab->physical_page,
                        cache.name);
    }

#if defined(OS1_KMEM_DEBUG)
    debug_initialize_free_slot(*slab, slot_index, slab->free_list);
    slab->free_list = ptr;
#else
    *reinterpret_cast<void**>(ptr) = slab->free_list;
    slab->free_list = ptr;
#endif
    ++slab->free_count;
    --cache.live_objects;
    ++cache.free_count;
    increment_global_stat(&KmemGlobalStats::free_count);
    if(slab->free_count > slab->object_count)
    {
        kmem_corruption("free count overflow",
                        ptr,
                        reinterpret_cast<uint64_t>(slab),
                        slab->physical_page,
                        cache.name);
    }

    const SlabListKind target = (slab->free_count == slab->object_count)
                                    ? SlabListKind::Empty
                                    : SlabListKind::Partial;
    move_slab(cache, slab, target);
    return (SlabListKind::Empty == target) && (nullptr != slab->next) &&
           (cache.slab_count > kKeepEmptySlabsPerCache);
}

void reset_cache(KmemCache& cache)
{
    cache.empty = nullptr;
    cache.partial = nullptr;
    cache.full = nullptr;
    cache.slab_count = 0;
    cache.live_objects = 0;
    cache.peak_live_objects = 0;
    cache.alloc_count = 0;
    cache.free_count = 0;
    cache.failed_alloc_count = 0;
    cache.slab_growth_count = 0;
    cache.slab_return_count = 0;
    cache.debug_last_alloc_caller = 0;
}

void snapshot_cache_locked(const KmemCache& cache, KmemCacheStats& snapshot)
{
    snapshot = {};
    snapshot.name = cache.name;
    snapshot.object_size = cache.object_size;
    snapshot.alignment = cache.alignment;
    snapshot.slab_count = cache.slab_count;
    snapshot.live_object_count = cache.live_objects;
    snapshot.peak_live_object_count = cache.peak_live_objects;
    snapshot.alloc_count = cache.alloc_count;
    snapshot.free_count = cache.free_count;
    snapshot.failed_alloc_count = cache.failed_alloc_count;
    snapshot.slab_growth_count = cache.slab_growth_count;
    snapshot.slab_return_count = cache.slab_return_count;
    snapshot.total_object_count = 0;
    snapshot.free_object_count = 0;
    snapshot.empty_slab_count = 0;
    snapshot.partial_slab_count = 0;
    snapshot.full_slab_count = 0;
    snapshot_list(cache.empty,
                  snapshot.empty_slab_count,
                  snapshot.total_object_count,
                  snapshot.free_object_count);
    snapshot_list(cache.partial,
                  snapshot.partial_slab_count,
                  snapshot.total_object_count,
                  snapshot.free_object_count);
    snapshot_list(cache.full,
                  snapshot.full_slab_count,
                  snapshot.total_object_count,
                  snapshot.free_object_count);
}

[[nodiscard]] void* allocate_from_cache(KmemCache& cache,
                                        KmallocFlags flags,
                                        uintptr_t caller)
{
    const bool allow_growth = !has_flag(flags, KmallocFlags::NoGrow) &&
                              !has_flag(flags, KmallocFlags::Atomic);
    bool attempted_growth = false;

    for(;;)
    {
        void* object = nullptr;
        {
            CacheLockGuard guard(cache.lock);
            SlabPageHeader* slab = select_slab(cache);
            if(nullptr != slab)
            {
                object = allocate_from_slab_locked(cache, slab, caller);
            }
        }

        if(nullptr != object)
        {
            if(has_flag(flags, KmallocFlags::Zero))
            {
                memset(object, 0, cache.slot_size);
            }
#if defined(OS1_KMEM_DEBUG)
            else
            {
                memset(object, kKmemAllocPoisonByte, cache.slot_size);
            }
#endif
            return object;
        }

        if(!allow_growth || attempted_growth)
        {
            record_cache_failure(cache);
            return kmem_fail("cache exhausted", cache.object_size, flags);
        }

        if(!grow_cache(cache))
        {
            record_cache_failure(cache);
            return kmem_fail("out of physical pages", cache.object_size, flags);
        }
        attempted_growth = true;
    }
}

void free_object(void* ptr, KmemCache* expected_cache)
{
    if(nullptr == ptr)
    {
        return;
    }
    if(!g_kmem_ready || (nullptr == g_kmem_frames))
    {
        increment_global_stat(&KmemGlobalStats::invalid_free_count);
        kmem_corruption("free before initialization", ptr);
    }

    const uint64_t page_base = allocation_page_base(ptr);
    const uint32_t observed_magic = *reinterpret_cast<const uint32_t*>(page_base);
    const auto kind = allocation_kind_from_magic(observed_magic);
    if(AllocationKind::Large == kind)
    {
        if(nullptr != expected_cache)
        {
            increment_global_stat(&KmemGlobalStats::invalid_free_count);
            kmem_corruption("cache free used for large allocation",
                            ptr,
                            page_base,
                            reinterpret_cast<const LargeAllocationHeader*>(page_base)->physical_base,
                            expected_cache->name);
        }

        auto* large = reinterpret_cast<LargeAllocationHeader*>(page_base);
        if(!pointer_is_large_allocation(*large, ptr))
        {
            increment_global_stat(&KmemGlobalStats::invalid_free_count);
            kmem_corruption("invalid large allocation pointer",
                            ptr,
                            reinterpret_cast<uint64_t>(large),
                            large->physical_base,
                            "large");
        }

        release_large_allocation(large);
        return;
    }

    auto* slab = reinterpret_cast<SlabPageHeader*>(page_base);
    if((AllocationKind::Slab != kind) || (nullptr == slab->cache))
    {
        increment_global_stat(&KmemGlobalStats::invalid_free_count);
        kmem_corruption("invalid slab header",
                        ptr,
                        reinterpret_cast<uint64_t>(slab),
                        0,
                        nullptr,
                        kSlabMagic,
                        observed_magic);
    }

    KmemCache& cache = *slab->cache;
    if((nullptr != expected_cache) && (&cache != expected_cache))
    {
        increment_global_stat(&KmemGlobalStats::invalid_free_count);
        kmem_corruption("cache free mismatch",
                        ptr,
                        reinterpret_cast<uint64_t>(slab),
                        slab->physical_page,
                        expected_cache->name);
    }

    bool release_slab = false;
    uint64_t release_physical_page = 0;
    {
        CacheLockGuard guard(cache.lock);
        if((slab->cache != &cache) || (slab->object_size != cache.slot_size))
        {
            increment_global_stat(&KmemGlobalStats::invalid_free_count);
            kmem_corruption("cache mismatch",
                            ptr,
                            reinterpret_cast<uint64_t>(slab),
                            slab->physical_page,
                            cache.name);
        }

        release_slab = free_object_locked(cache, slab, ptr);
        if(release_slab)
        {
            release_physical_page = slab->physical_page;
            detach_slab(cache, slab);
            if(0u == cache.slab_count)
            {
                kmem_corruption("slab count underflow",
                                ptr,
                                reinterpret_cast<uint64_t>(slab),
                                slab->physical_page,
                                cache.name);
            }
            --cache.slab_count;
            ++cache.slab_return_count;
            record_slab_page_release();
        }
    }

    if(release_slab)
    {
        poison_physical_pages(release_physical_page, 1u);
        (void)g_kmem_frames->free(release_physical_page);
    }
}

void release_cache_slabs(KmemCache& cache)
{
    auto release_list = [&](SlabPageHeader* head) {
        while(nullptr != head)
        {
            SlabPageHeader* next = head->next;
            const uint64_t physical_page = head->physical_page;
            if((head->cache != &cache) || (head->object_size != cache.slot_size))
            {
                kmem_corruption("destroy cache mismatch",
                                head,
                                reinterpret_cast<uint64_t>(head),
                                physical_page,
                                cache.name);
            }
            if(head->free_count != head->object_count)
            {
                kmem_corruption("destroy with live objects",
                                head,
                                reinterpret_cast<uint64_t>(head),
                                physical_page,
                                cache.name);
            }
            record_slab_page_release();
            poison_physical_pages(physical_page, 1u);
            (void)g_kmem_frames->free(physical_page);
            head = next;
        }
    };

    release_list(cache.empty);
    release_list(cache.partial);
    release_list(cache.full);
    reset_cache(cache);
}
}  // namespace

void kmem_init(PageFrameContainer& frames)
{
    g_kmem_frames = &frames;
    {
        CacheLockGuard guard(g_kmem_stats_lock);
        g_kmem_global_stats = {};
    }
    for(size_t index = 0; index < builtin_cache_count(); ++index)
    {
        reset_cache(g_builtin_caches[index]);
    }
    {
        CacheLockGuard registry_guard(g_named_cache_registry_lock);
        g_named_cache_head = nullptr;
        g_named_cache_count = 0;
        for(size_t index = 0; index < kMaxNamedCaches; ++index)
        {
            reset_named_cache_slot(g_named_cache_slots[index]);
        }
    }
    g_kmem_ready = true;
}

void* kmalloc(size_t size, KmallocFlags flags)
{
    if(!g_kmem_ready || (nullptr == g_kmem_frames))
    {
        return kmem_fail("allocator not initialized", size, flags);
    }
    if(0u == size)
    {
        return kmem_fail("zero-sized request", size, flags);
    }

    KmemCache* cache = select_cache(size);
    if(nullptr == cache)
    {
        return allocate_large(size, flags);
    }

    return allocate_from_cache(*cache, flags, capture_debug_caller());
}

void* kcalloc(size_t count, size_t size, KmallocFlags flags)
{
    size_t total_size = 0;
    if(multiply_overflow_size(count, size, total_size))
    {
        return kmem_fail("kcalloc overflow", 0, flags);
    }
    return kmalloc(total_size, flags | KmallocFlags::Zero);
}

KmemCache* kmem_cache_create(const char* name, size_t object_size, size_t alignment)
{
    if(!g_kmem_ready || (nullptr == g_kmem_frames))
    {
        return nullptr;
    }

    uint16_t cache_object_size = 0;
    uint16_t cache_slot_size = 0;
    uint16_t cache_payload_offset = 0;
    uint16_t cache_slot_stride = 0;
    uint16_t cache_alignment = 0;
    if(!named_cache_parameters_valid(name,
                                     object_size,
                                     alignment,
                                     cache_object_size,
                                     cache_slot_size,
                                     cache_payload_offset,
                                     cache_slot_stride,
                                     cache_alignment))
    {
        return nullptr;
    }

    CacheLockGuard registry_guard(g_named_cache_registry_lock);
    for(size_t index = 0; index < kMaxNamedCaches; ++index)
    {
        KmemCache& cache = g_named_cache_slots[index];
        if(cache.active)
        {
            continue;
        }

        reset_named_cache_slot(cache);
        configure_named_cache(cache,
                      name,
                      cache_object_size,
                      cache_slot_size,
                      cache_payload_offset,
                      cache_slot_stride,
                      cache_alignment);
        cache.registry_next = g_named_cache_head;
        g_named_cache_head = &cache;
        ++g_named_cache_count;
        return &cache;
    }

    return nullptr;
}

void* kmem_cache_alloc(KmemCache* cache, KmallocFlags flags)
{
    if((nullptr == cache) || !cache->active)
    {
        return nullptr;
    }
    return allocate_from_cache(*cache, flags, capture_debug_caller());
}

void kmem_cache_free(KmemCache* cache, void* ptr)
{
    if(nullptr == ptr)
    {
        return;
    }
    if((nullptr == cache) || !cache->active)
    {
        increment_global_stat(&KmemGlobalStats::invalid_free_count);
        kmem_corruption("free with invalid cache handle", ptr);
    }
    free_object(ptr, cache);
}

bool kmem_cache_destroy(KmemCache* cache)
{
    if((nullptr == cache) || cache->builtin || !cache->active)
    {
        return false;
    }

    {
        CacheLockGuard registry_guard(g_named_cache_registry_lock);
        if(!named_cache_is_registered_locked(cache))
        {
            return false;
        }

        CacheLockGuard cache_guard(cache->lock);
        if(0u != cache->live_objects)
        {
            return false;
        }

        detach_named_cache_locked(cache);
        cache->active = false;
    }

    release_cache_slabs(*cache);
    reset_named_cache_slot(*cache);
    return true;
}

void kfree(void* ptr)
{
    free_object(ptr, nullptr);
}

bool kmem_allocation_usable_size(const void* ptr, size_t& size)
{
    size = 0;
    return allocation_usable_size_internal(ptr, size);
}

size_t kmem_cache_stats_count()
{
    CacheLockGuard registry_guard(g_named_cache_registry_lock);
    return builtin_cache_count() + g_named_cache_count;
}

bool kmem_get_cache_stats(size_t index, KmemCacheStats& stats)
{
    if(index < builtin_cache_count())
    {
        KmemCache& cache = g_builtin_caches[index];
        KmemCacheStats snapshot{};

        {
            CacheLockGuard guard(cache.lock);
            snapshot_cache_locked(cache, snapshot);
        }

        stats = snapshot;
        return true;
    }

    CacheLockGuard registry_guard(g_named_cache_registry_lock);
    KmemCache* cache = named_cache_from_index_locked(index - builtin_cache_count());
    if(nullptr == cache)
    {
        return false;
    }

    KmemCacheStats snapshot{};
    {
        CacheLockGuard guard(cache->lock);
        snapshot_cache_locked(*cache, snapshot);
    }

    stats = snapshot;
    return true;
}

void kmem_get_global_stats(KmemGlobalStats& stats)
{
    {
        CacheLockGuard guard(g_kmem_stats_lock);
        stats = g_kmem_global_stats;
    }
    stats.cache_count = kmem_cache_stats_count();
}

void kmem_dump_stats()
{
    KmemGlobalStats global{};
    kmem_get_global_stats(global);
    debug("kmem: slab_pages=")(global.slab_page_count)(" alloc=")(global.alloc_count)(" free=")(
        global.free_count)(" failed=")(global.failed_alloc_count)(" large=")(
        global.live_large_allocation_count)(" large_bytes=")(global.live_large_allocation_bytes)();

    for(size_t index = 0; index < kmem_cache_stats_count(); ++index)
    {
        KmemCacheStats cache{};
        if(!kmem_get_cache_stats(index, cache))
        {
            continue;
        }

        debug("kmem: cache ")(cache.name)(" obj=")(static_cast<uint64_t>(cache.object_size))(" slabs=")(
            cache.slab_count)(" live=")(cache.live_object_count)(" free=")(cache.free_object_count)(
            " alloc=")(cache.alloc_count)(" fail=")(cache.failed_alloc_count)();
    }
}

void kmem_dump_leaks()
{
    bool found_live_allocations = false;

    auto dump_cache_leaks = [&](KmemCache& cache) {
        CacheLockGuard guard(cache.lock);
        if(0u == cache.live_objects)
        {
            return;
        }

        found_live_allocations = true;
        debug("kmem: leak cache ")(cache.name)(" live=")(cache.live_objects)(" peak=")(
            cache.peak_live_objects)(" alloc=")(cache.alloc_count)(" free=")(cache.free_count)();
#if defined(OS1_KMEM_DEBUG)
        if(0u != cache.debug_last_alloc_caller)
        {
            debug("kmem: leak caller=0x")(static_cast<uint64_t>(cache.debug_last_alloc_caller), 16, 16)();
        }
#endif
    };

    for(size_t index = 0; index < builtin_cache_count(); ++index)
    {
        dump_cache_leaks(g_builtin_caches[index]);
    }

    {
        CacheLockGuard registry_guard(g_named_cache_registry_lock);
        for(KmemCache* cache = g_named_cache_head; nullptr != cache; cache = cache->registry_next)
        {
            dump_cache_leaks(*cache);
        }
    }

    KmemGlobalStats global{};
    kmem_get_global_stats(global);
    if(0u != global.live_large_allocation_count)
    {
        found_live_allocations = true;
        debug("kmem: leak large count=")(global.live_large_allocation_count)(" bytes=")(
            global.live_large_allocation_bytes)();
    }

    if(!found_live_allocations)
    {
        debug("kmem: no live allocations")();
    }
}
