#include "debug/log_export.hpp"

#include <os1/ipc.h>
#include <os1/resource.h>
#include <os1/syscall_numbers.h>

#include "core/kernel_state.hpp"
#include "debug/log.hpp"
#include "fs/initrd.hpp"
#include "log/frame.hpp"
#include "mm/kmem.hpp"
#include "sync/atomic.hpp"
#include "util/memory.h"
#include "vfs/filesystem.hpp"
#include "vfs/namespace.hpp"
#ifndef OS1_HOST_TEST
#include "log_build_identity.hpp"
#else
#define OS1_LOG_REVISION "host-test"
#define OS1_LOG_IMAGE "host-test"
#endif
namespace log_export
{
#ifdef OS1_HOST_TEST
void (*before_remove)(VfsNode*, const char*);
void (*after_history_snapshot)();
#endif
namespace
{
using os1::log::Frame;
using os1::log::Name;
constexpr uint64_t second = 1000000000;
uint8_t* staging;
bool initialized, suspended, dirty;
volatile bool busy;
volatile uint32_t admission;
os1_log_export_info status{}, published_status{};
Spinlock status_lock{"log-export-status"};
void publish_status()
{
    IrqSpinGuard guard(status_lock);
    published_status = status;
}
uint64_t kernel_cursor, next_segment = 1, last_errors, window_start, window_bytes, last_error_drain;
uint64_t mount_identity, last_poll;
unsigned first_stream;
struct Stream
{
    uint64_t session = 0, cursor = 0, segment = 0;
} streams[5];
struct Segment
{
    Name name;
    uint64_t bytes;
    char file[64];
} segments[128];
unsigned segment_count;
struct Ref
{
    NativeObject* object = nullptr;
    ~Ref()
    {
        object_release(object);
    }
    VfsNode* node() const
    {
        return static_cast<VfsNode*>(object);
    }
};
bool same_bytes(const char* a, const char* b, size_t n)
{
    for(size_t i = 0; i < n; ++i)
        if(a[i] != b[i])
            return false;
    return true;
}
size_t length(const char* s)
{
    size_t n = 0;
    while(s[n])
        ++n;
    return n;
}
long lookup(VfsNode* parent, const char* name, Ref& out)
{
    return parent->operations->lookup(parent, name, length(name), out.object);
}
long create(VfsNode* parent, const char* name, unsigned kind, Ref& out)
{
    OpenInstance* instance = nullptr;
    long r = parent->operations->create(parent, name, length(name), kind, instance);
    if(r < 0)
        return r;
    out.object = instance->object;
    object_retain(out.object);
    instance_release(instance);
    return 0;
}
long space(VfsNode* node, uint64_t requested);
long directory(const char* mount, Ref& base, Ref& logs)
{
    auto* root = initrd_namespace_root();
    if(!root)
        return OS1_ERR_NOT_FOUND;
    long r = vfs_resolve(root, mount + 1, length(mount) - 1, base.object);
    if(r < 0)
        return r;
    if(!base.node()->operations)
        return OS1_ERR_UNSUPPORTED;
    if(!(base.node()->allowed_rights & OS1_RIGHT_CREATE))
        return OS1_ERR_ACCESS;
    r = lookup(base.node(), "log", logs);
    if(r == OS1_ERR_NOT_FOUND)
    {
        if(mount[1] == 'd')
        {
            r = space(base.node(), 8192);
            if(r < 0)
                return r;
        }
        r = create(base.node(), "log", OS1_OBJECT_DIRECTORY, logs);
    }
    if(r < 0)
        return r;
    return logs.object->kind == OS1_OBJECT_DIRECTORY ? 0 : OS1_ERR_NOT_DIRECTORY;
}
long metadata(VfsNode* node, os1_metadata& out)
{
    NamespaceGuard guard;
    return guard.held ? vfs_metadata(node, out) : OS1_ERR_WOULD_BLOCK;
}
long space(VfsNode* node, uint64_t requested)
{
    os1_space out{};
    NamespaceGuard guard;
    long r = guard.held ? vfs_space(node, out) : OS1_ERR_WOULD_BLOCK;
    if(r < 0)
        return r;
    uint64_t reserve = out.total_bytes / 10;
    if(reserve < 2 * 1024 * 1024)
        reserve = 2 * 1024 * 1024;
    return out.free_bytes >= reserve && requested <= out.free_bytes - reserve ? 0 : OS1_ERR_LIMIT;
}
Frame& frame()
{
    return *reinterpret_cast<Frame*>(staging);
}
void prepare(unsigned stream, uint64_t session, uint64_t segment, uint64_t overwritten)
{
    auto info = diagnostic_log::info();
    auto& h = frame();
    h = Frame{};
    h.stream = stream;
    h.record_bytes = stream == 1 ? 256 : 2048;
    h.boot = status.boot_id ? status.boot_id : (info.boot_id ? info.boot_id : 1);
    h.session = session;
    h.segment = segment;
    h.monotonic_ns = diagnostic_log::monotonic();
    h.wall_ns = info.wall_time_ns;
    h.resolution_ns = info.clock_resolution_ns;
    h.overwritten = overwritten;
    h.dropped = info.dropped;
    h.diagnostic = info.diagnostic;
#ifndef OS1_HOST_TEST
    h.boot_mode = g_boot_info ? static_cast<unsigned>(g_boot_info->source) : 0;
#endif
    memcpy(h.revision,
           OS1_LOG_REVISION,
           sizeof(OS1_LOG_REVISION) - 1 < sizeof(h.revision) ? sizeof(OS1_LOG_REVISION) - 1
                                                             : sizeof(h.revision));
    memcpy(
        h.image,
        OS1_LOG_IMAGE,
        sizeof(OS1_LOG_IMAGE) - 1 < sizeof(h.image) ? sizeof(OS1_LOG_IMAGE) - 1 : sizeof(h.image));
}
void seal()
{
    frame().bytes = sizeof(Frame) + frame().count * frame().record_bytes;
    frame().checksum = os1::log::crc(staging, frame().bytes);
}
long write(VfsNode* file, uint64_t offset, const void* data, size_t bytes)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for(size_t at = 0; at < bytes;)
    {
        size_t count = bytes - at;
        const size_t preferred = file->operations->preferred_io_bytes;
        const size_t remaining =
            preferred > 256 ? preferred - (offset + at) % preferred : preferred;
        if(count > remaining)
            count = remaining;
        long r = file->operations->write(file, offset + at, p + at, count, false, nullptr);
        if(r < 0)
            return r;
        if(static_cast<size_t>(r) != count)
            return OS1_ERR_IO;
        at += count;
    }
    return 0;
}
long managed(VfsNode* file, const Name& name)
{
    Frame h{};
    long r = file->operations->read(file, 0, &h, sizeof(h));
    if(r < 0)
        return r;
    return r == sizeof(h) && !h.count && os1::log::valid(h, sizeof(h)) && h.stream == name.stream &&
           h.boot == name.boot && h.session == name.session && h.segment == name.segment;
}
long remove_managed(VfsNode* dir, const char* path, const Name& identity)
{
#ifdef OS1_HOST_TEST
    if(before_remove)
        before_remove(dir, path);
#endif
    NamespaceGuard guard;
    if(!guard.held)
        return OS1_ERR_WOULD_BLOCK;
    Ref file;
    long r = lookup(dir, path, file);
    if(r < 0)
        return r;
    if(file.object->kind != OS1_OBJECT_FILE)
        return OS1_ERR_EXISTS;
    r = managed(file.node(), identity);
    if(r < 0)
        return r;
    if(!r)
        return OS1_ERR_EXISTS;
    // Keep namespace admission while releasing the lookup reference for ext2 unlink.
    object_release(file.object);
    file.object = nullptr;
    return dir->operations->remove_locked(dir, path, length(path), OS1_OBJECT_FILE);
}
long scan(VfsNode* dir, uint64_t& maximum)
{
    segment_count = 0;
    maximum = 0;
    uint64_t cursor = 0, next = 0;
    for(unsigned i = 0; i < 256; ++i)
    {
        os1_directory_entry entry{};
        long r = dir->operations->next(dir, cursor, entry, next);
        if(r < 0)
            return r;
        if(!r)
            return 0;
        cursor = next;
        Name name;
        if(!os1::log::parse_name(entry.name, entry.name_bytes, name))
            continue;
        if(name.boot > maximum)
            maximum = name.boot;  // Reserve even incomplete or foreign matching names.
        Ref file;
        r = lookup(dir, entry.name, file);
        if(r < 0)
            return r;
        if(file.object->kind != OS1_OBJECT_FILE)
            continue;
        r = managed(file.node(), name);
        if(r < 0)
            return r;
        if(!r)
            continue;
        os1_metadata m{};
        r = metadata(file.node(), m);
        if(r < 0)
            return r;
        if(segment_count == 128)
            return OS1_ERR_LIMIT;
        auto& row = segments[segment_count++];
        row.name = name;
        row.bytes = m.logical_bytes;
        memcpy(row.file, entry.name, entry.name_bytes);
        row.file[entry.name_bytes] = 0;
    }
    return OS1_ERR_LIMIT;
}
long retention(VfsNode* dir, unsigned stream, uint64_t extra, bool new_file)
{
    for(unsigned attempt = 0; attempt < 128; ++attempt)
    {
        uint64_t maximum = 0;
        long r = scan(dir, maximum);
        if(r < 0)
            return r;
        uint64_t total = 0;
        unsigned boots = 0;
        uint64_t seen[128]{};
        int oldest = -1;
        for(unsigned i = 0; i < segment_count; ++i)
        {
            auto& s = segments[i];
            if(s.name.stream == stream)
                total += s.bytes;
            bool known = false;
            for(unsigned j = 0; j < boots; ++j)
                known |= seen[j] == s.name.boot;
            if(!known)
                seen[boots++] = s.name.boot;
        }
        bool current_seen = false;
        for(unsigned i = 0; i < boots; ++i)
            current_seen |= seen[i] == status.boot_id;
        if(!current_seen)
            ++boots;
        bool stream_full = total + extra > (stream == 1 ? 3u * 1024 * 1024 : 1024u * 1024);
        if(!stream_full && segment_count + unsigned(new_file) <= 16 && boots <= 4)
            return 0;
        for(unsigned i = 0; i < segment_count; ++i)
        {
            auto& a = segments[i];
            if(stream_full && a.name.stream != stream)
                continue;
            if(oldest < 0 || a.name.boot < segments[oldest].name.boot ||
               (a.name.boot == segments[oldest].name.boot &&
                a.name.segment < segments[oldest].name.segment))
                oldest = i;
        }
        if(oldest < 0)
            return OS1_ERR_LIMIT;
        auto& victim = segments[oldest];
        r = remove_managed(dir, victim.file, victim.name);
        if(r < 0)
            return r;
    }
    return OS1_ERR_LIMIT;
}
long new_segment(VfsNode* dir, unsigned stream, Stream& state, Ref& file)
{
    if(next_segment == UINT64_MAX)
        return OS1_ERR_LIMIT;
    long r = retention(dir, stream, sizeof(Frame), true);
    if(r < 0)
        return r;
    r = space(dir, sizeof(Frame) + 4096);
    if(r < 0)
        return r;
    uint64_t number = next_segment++;
    Name name{status.boot_id, stream == 1 ? 0 : state.session, number, stream};
    char path[64]{};
    os1::log::filename(name, path);
    r = create(dir, path, OS1_OBJECT_FILE, file);
    if(r < 0)
        return r;
    // The payload is already staged. Use a separate small initial identity frame.
    Frame header = frame();
    header.count = 0;
    header.first = header.last = 0;
    header.bytes = sizeof(Frame);
    header.boot = status.boot_id;
    header.segment = number;
    header.checksum = os1::log::crc(&header, sizeof(header));
    r = write(file.node(), 0, &header, sizeof(header));
    if(r < 0)
        return r;
    state.segment = number;
    dirty = true;
    status.disk_bytes += sizeof(Frame);
    window_bytes += sizeof(Frame);
    return 0;
}
long append_frame(VfsNode* dir, unsigned stream, Stream& state)
{
    long r = retention(dir, stream, frame().bytes + sizeof(Frame), true);
    if(r < 0)
        return r;
    Ref file;
    r = OS1_ERR_NOT_FOUND;
    if(state.segment)
    {
        Name name{status.boot_id, stream == 1 ? 0 : state.session, state.segment, stream};
        char path[64]{};
        os1::log::filename(name, path);
        r = lookup(dir, path, file);
        if(!r)
        {
            if(file.object->kind != OS1_OBJECT_FILE)
                return OS1_ERR_INVALID;
            long recognized = managed(file.node(), name);
            if(recognized < 0)
                return recognized;
            if(!recognized)
                return OS1_ERR_INVALID;
        }
    }
    if(r < 0 && r != OS1_ERR_NOT_FOUND)
        return r;
    os1_metadata m{};
    if(!r)
    {
        r = metadata(file.node(), m);
        if(r < 0)
            return r;
    }
    if(file.object && m.logical_bytes + frame().bytes > os1::log::segment_limit)
    {
        object_release(file.object);
        file.object = nullptr;
    }
    if(!file.object)
    {
        r = new_segment(dir, stream, state, file);
        if(r < 0)
            return r;
        m.logical_bytes = sizeof(Frame);
    }
    // Do not let retention unlink the append target while it is retained.
    r = retention(dir, stream, frame().bytes, false);
    if(r < 0)
        return r;
    r = space(dir, frame().bytes + 8192);
    if(r < 0)
        return r;
    frame().segment = state.segment;
    frame().boot = status.boot_id;
    seal();
    r = write(file.node(), m.logical_bytes, staging, frame().bytes);
    if(r < 0)
    {
        state.segment = 0;
        return r;
    }  // Never append beyond a torn frame.
    dirty = true;
    status.disk_bytes += frame().bytes;
    window_bytes += frame().bytes;
    return 0;
}
long boot(VfsNode* dir)
{
    if(status.boot_id)
        return 0;
    uint64_t maximum = 0;
    long r = scan(dir, maximum);
    if(r < 0)
        return r;
    if(maximum == UINT64_MAX)
        return OS1_ERR_LIMIT;
    status.boot_id = maximum + 1;
    prepare(1, 0, 0, 0);
    seal();
    Ref file;
    r = new_segment(dir, 1, streams[0], file);
    if(!r)
        r = dir->operations->sync(dir);
    if(r < 0)
    {
        status.boot_id = 0;
        streams[0].segment = 0;
        return r;
    }
    dirty = false;
    status.last_sync_ns = diagnostic_log::monotonic();
    return 0;
}
long gather(unsigned stream,
            unsigned slot,
            uint64_t session,
            uint64_t after,
            uint64_t through,
            unsigned capacity,
            bool final,
            uint64_t& advance)
{
    advance = after;
    uint64_t scan_after = after;
    auto& h = frame();
    while(h.count < capacity)
    {
        os1_log_query q{1, sizeof(q), scan_after, through, 0, 0, 0, 0};
        auto* dst = staging + sizeof(Frame) + h.count * h.record_bytes;
        long r;
        os1_log_record* record;
        if(stream == 1)
        {
            record = reinterpret_cast<os1_log_record*>(dst);
            r = diagnostic_log::read(q, *record);
        }
        else
        {
            auto* row = reinterpret_cast<os1_log_history_record*>(dst);
            record = &row->record;
            r = diagnostic_log::history_read(slot, session, q, *row);
        }
        if(r < 0)
            return r;
        if(!r)
            break;
        if((record->flags & OS1_LOG_PENDING) && !final)
            break;
        if(!h.count)
            h.first = record->sequence;
        h.last = record->sequence;
        ++h.count;
        scan_after = record->sequence;
        if(record->flags & OS1_LOG_PENDING)
            break;
        advance = scan_after;
    }
    seal();
    return 0;
}
void retire_closed(bool success)
{
    for(unsigned i = 0; i < 4; ++i)
    {
        diagnostic_log::HistoryState h{};
        if(diagnostic_log::history_state(i, h) && h.closing)
        {
            bool complete =
                streams[i + 1].session == h.session && streams[i + 1].cursor >= h.latest;
            if(success && !complete)
                continue;
            if(!complete)
                ++status.closing_lost;
            diagnostic_log::retire_history(i, h.session);
        }
    }
}
void observe_mount(uint64_t identity)
{
    if(identity == mount_identity)
        return;
    mount_identity = identity;
    suspended = false;
    dirty = false;
    status.suspended = 0;
    status.consecutive_failures = 0;
    status.boot_id = 0;
    next_segment = 1;
    kernel_cursor = 0;
    for(auto& stream : streams)
        stream = {};
}
long disk(bool final, bool synchronize)
{
    Ref base, dir;
    long r = directory("/disk", base, dir);
    if(r < 0)
        return r;
    observe_mount(base.node()->object_id);
    r = boot(dir.node());
    if(r < 0)
        return r;
    auto now = diagnostic_log::monotonic();
    unsigned ceiling = diagnostic_log::info().diagnostic ? 256 * 1024 : 64 * 1024;
    if(now - window_start >= second)
    {
        window_start = now;
        window_bytes = 0;
    }
    unsigned budget =
        final ? 2 * 1024 * 1024 : (window_bytes < ceiling ? ceiling - window_bytes : 0);
    // Rotate admission so each bounded stream can make progress under sustained traffic.
    bool pending_written[5]{};
    for(unsigned step = 0; step < (final ? 100u : 5u) && budget > sizeof(Frame) + 2048; ++step)
    {
        unsigned index = (step + first_stream) % 5, stream = index ? 2 : 1;
        if(pending_written[index])
            continue;
        auto& state = streams[index];
        diagnostic_log::HistoryState h{};
        auto info = diagnostic_log::info();
        uint64_t latest = info.latest, overwritten = info.overwritten;
        if(index)
        {
            if(!diagnostic_log::history_state(index - 1, h))
                continue;
            if(state.session != h.session)
                state = {h.session, 0, 0};
            latest = h.latest;
            overwritten = h.overwritten;
        }
        if(state.cursor >= latest && !final)
            continue;
        prepare(stream, state.session, state.segment, overwritten);
        unsigned record = stream == 1 ? 256 : 2048;
        unsigned capacity = (budget - 2 * sizeof(Frame)) / record;
        if(capacity > (65536 - sizeof(Frame)) / record)
            capacity = (65536 - sizeof(Frame)) / record;
        uint64_t advance;
        r = gather(stream,
                   index ? index - 1 : 0,
                   state.session,
                   state.cursor,
                   latest,
                   capacity,
                   final || h.closing,
                   advance);
        if(r < 0)
            return r;
        if(!frame().count)
            continue;
        pending_written[index] = frame().last > advance;
        r = append_frame(dir.node(), stream, state);
        if(r < 0)
            return r;
        budget -= frame().bytes + sizeof(Frame);
        state.cursor = h.closing ? frame().last : advance;
    }
    first_stream = (first_stream + 1) % 5;
    kernel_cursor = streams[0].cursor;
    status.kernel_cursor = kernel_cursor;
    status.last_drain_ns = now;
    if(dirty && (synchronize || now - status.last_sync_ns >=
                                    (diagnostic_log::info().diagnostic ? second : 5 * second)))
    {
        r = dir.node()->operations->sync(dir.node());
        if(r < 0)
            return r;
        dirty = false;
        status.last_sync_ns = now;
    }
    retire_closed(true);
    return 0;
}
long snapshot_file(VfsNode* dir,
                   const char* path,
                   unsigned stream,
                   unsigned slot,
                   uint64_t session,
                   uint64_t latest,
                   uint64_t overwritten,
                   unsigned budget)
{
    Ref file;
    long r = lookup(dir, path, file);
    if(r == OS1_ERR_NOT_FOUND)
        r = create(dir, path, OS1_OBJECT_FILE, file);
    else if(!r)
    {
        Frame header{};
        if(file.object->kind != OS1_OBJECT_FILE ||
           file.node()->operations->read(file.node(), 0, &header, sizeof(header)) !=
               sizeof(header) ||
           !os1::log::valid(header, sizeof(header)) || header.count || header.stream != stream ||
           header.session != session)
            return OS1_ERR_EXISTS;
    }
    if(r < 0)
        return r;
    r = file.node()->operations->truncate(file.node(), 0);
    if(r < 0)
        return r;
    prepare(stream, session, 0, overwritten);
    seal();
    r = write(file.node(), 0, staging, sizeof(Frame));
    if(r < 0)
        return r;
    unsigned capacity = (budget - 2 * sizeof(Frame)) / (stream == 1 ? 256 : 2048);
    uint64_t after = latest > capacity ? latest - capacity : 0, advance;
    r = gather(stream, slot, session, after, latest, capacity, true, advance);
    if(r < 0)
        return r;
    if(frame().count)
        r = write(file.node(), sizeof(Frame), staging, frame().bytes);
    if(!r)
        status.tmp_bytes += sizeof(Frame) + (frame().count ? frame().bytes : 0);
    return r;
}
long temporary()
{
    Ref base, dir;
    long r = directory("/tmp", base, dir);
    if(r < 0)
        return r;
    // Remove only prior managed shell snapshots before enforcing the aggregate budget.
    uint64_t cursor = 0, next = 0;
    char stale[4][64]{};
    Name stale_identity[4]{};
    unsigned count = 0;
    bool ended = false;
    for(unsigned i = 0; i < 256; ++i)
    {
        os1_directory_entry entry{};
        r = dir.node()->operations->next(dir.node(), cursor, entry, next);
        if(r < 0)
            return r;
        if(!r)
        {
            ended = true;
            break;
        }
        cursor = next;
        if(entry.name_bytes < 6 || entry.name_bytes >= 64 || !same_bytes(entry.name, "shell-", 6))
            continue;
        Ref file;
        r = lookup(dir.node(), entry.name, file);
        if(r < 0)
            return r;
        Frame h{};
        if(file.object->kind != OS1_OBJECT_FILE ||
           file.node()->operations->read(file.node(), 0, &h, sizeof(h)) != sizeof(h) || h.count ||
           h.stream != 2 || !os1::log::valid(h, sizeof(h)))
            continue;
        char expected[64] = "shell-";
        char* p = expected + 6;
        os1::log::hex(p, h.session);
        memcpy(p, ".bin", 5);
        if(entry.name_bytes != 26 || !same_bytes(entry.name, expected, 26))
            continue;
        if(count == 4)
            return OS1_ERR_LIMIT;
        memcpy(stale[count], entry.name, entry.name_bytes);
        stale_identity[count] = {h.boot, h.session, h.segment, h.stream};
        ++count;
    }
    if(!ended)
        return OS1_ERR_LIMIT;
    for(unsigned i = 0; i < count; ++i)
    {
        r = remove_managed(dir.node(), stale[i], stale_identity[i]);
        if(r < 0)
            return r;
    }
    status.tmp_bytes = 0;
    auto info = diagnostic_log::info();
    r = snapshot_file(dir.node(), "kernel.bin", 1, 0, 0, info.latest, info.overwritten, 32768);
    if(r < 0)
        return r;
    unsigned histories = 0;
    diagnostic_log::HistoryState captured[4]{};
    bool present[4]{};
    for(unsigned i = 0; i < 4; ++i)
    {
        present[i] = diagnostic_log::history_state(i, captured[i]);
        histories += present[i];
    }
#ifdef OS1_HOST_TEST
    if(after_history_snapshot)
        after_history_snapshot();
#endif
    for(unsigned i = 0; i < 4; ++i)
        if(present[i])
        {
            const auto& h = captured[i];
            char name[64] = "shell-";
            char* p = name + 6;
            os1::log::hex(p, h.session);
            memcpy(p, ".bin", 5);
            r = snapshot_file(
                dir.node(), name, 2, i, h.session, h.latest, h.overwritten, 32768 / histories);
            if(r < 0)
                return r;
        }
    return 0;
}
void outcome(long result)
{
    status.last_status = result;
    if(result < 0)
    {
        ++status.failures;
        ++status.consecutive_failures;
        if(status.consecutive_failures >= 3)
            suspended = true;
        retire_closed(false);
    }
    else
        status.consecutive_failures = 0;
    status.suspended = suspended;
}
bool try_begin_exclusive()
{
    bool expected = false;
    if(!atomic_compare_exchange_strong(&busy, &expected, true))
        return false;
    uint32_t idle = 0;
    if(atomic_compare_exchange_strong(&admission, &idle, UINT32_MAX))
        return true;
    atomic_store_release(&busy, false);
    return false;
}
void end_exclusive()
{
    atomic_store_release(&admission, 0u);
    atomic_store_release(&busy, false);
}
long export_disk()
{
    suspended = false;
    status.suspended = 0;
    status.consecutive_failures = 0;
    const long result = disk(true, true);
    outcome(result);
    return result;
}
}  // namespace
bool syscall_needs_filesystem(uint64_t number, uint64_t operation)
{
    switch(number)
    {
        case os1_sys_ipc:
            return false;
        case os1_sys_resource:
            return operation < OS1_RESOURCE_CLOSE || operation > OS1_RESOURCE_SET_STATUS_FLAGS;
        case os1_sys_yield:
        case os1_sys_getpid:
        case os1_sys_observe:
        case os1_sys_waitpid:
        case os1_sys_log:
            return false;
        default:
            return true;
    }
}
ForegroundGuard::ForegroundGuard(bool required) : held(!required), acquired(false)
{
    if(!required)
        return;
    uint32_t value = atomic_load_acquire(&admission);
    while(value < UINT32_MAX - 1)
        if(atomic_compare_exchange_strong(&admission, &value, value + 1))
        {
            held = acquired = true;
            break;
        }
}
ForegroundGuard::~ForegroundGuard()
{
    if(acquired)
        (void)__atomic_fetch_sub(&admission, 1u, __ATOMIC_RELEASE);
}
void initialize()
{
    if(initialized)
        return;
    staging = static_cast<uint8_t*>(kmalloc(65536, KmallocFlags::Zero));
    status.version = 1;
    status.size = sizeof(status);
    status.enabled = staging != nullptr;
    diagnostic_log::enable_export(staging != nullptr);
    publish_status();
    atomic_store_release(&initialized, true);
}
os1_log_export_info info()
{
    IrqSpinGuard guard(status_lock);
    return published_status;
}
bool try_run(unsigned target, long& result)
{
    if(!atomic_load_acquire(&initialized) || !staging)
    {
        result = OS1_ERR_NO_MEMORY;
        return true;
    }
    if(target != OS1_LOG_EXPORT_TMP && target != OS1_LOG_EXPORT_DISK)
    {
        result = OS1_ERR_INVALID;
        return true;
    }
    if(!try_begin_exclusive())
    {
        result = OS1_ERR_WOULD_BLOCK;
        return false;
    }
    long r;
    if(target == OS1_LOG_EXPORT_TMP)
    {
        r = temporary();
        status.last_status = r;
        if(r < 0)
            ++status.failures;
    }
    else
        r = export_disk();
    publish_status();
    end_exclusive();
    result = r;
    return true;
}
long run(unsigned target)
{
    long result;
    (void)try_run(target, result);
    return result;
}

void poll()
{
    if(!atomic_load_acquire(&initialized) || !staging)
        return;
    auto now = diagnostic_log::monotonic();
    if(now - last_poll < second / 4)
        return;
    last_poll = now;
    if(!try_begin_exclusive())
        return;
    Ref mount;
    long mounted = vfs_resolve(initrd_namespace_root(), "disk", 4, mount.object);
    if(!mounted)
        observe_mount(mount.node()->object_id);
    uint64_t errors = diagnostic_log::error_sequence();
    bool early = errors != last_errors && now - last_error_drain >= second;
    auto info = diagnostic_log::info();
    bool closing = false;
    uint64_t kernel_pending = info.latest > kernel_cursor ? info.latest - kernel_cursor : 0;
    uint64_t pending_bytes = (kernel_pending > 128 ? 128 : kernel_pending) * 256;
    diagnostic_log::HistoryState h{};
    for(unsigned i = 0; i < 4; ++i)
        if(diagnostic_log::history_state(i, h))
        {
            closing |= h.closing;
            uint64_t cursor = streams[i + 1].session == h.session ? streams[i + 1].cursor : 0;
            uint64_t pending = h.latest > cursor ? h.latest - cursor : 0;
            pending_bytes += (pending > 16 ? 16 : pending) * 2048;
        }
    bool sync_due = dirty && now - status.last_sync_ns >= (info.diagnostic ? second : 5 * second);
    bool due = sync_due ||
               now - status.last_drain_ns >= (info.diagnostic ? second / 4 : 2 * second) ||
               pending_bytes >= 32768 || early || closing;
    const bool disk_read_only = !mounted && !(mount.node()->allowed_rights & OS1_RIGHT_CREATE);
    if(disk_read_only)
        retire_closed(false);
    else if(due && !suspended)
    {
        if(early)
        {
            last_errors = errors;
            last_error_drain = now;
        }
        status.last_drain_ns = now;
        outcome(disk(false, early));
    }
    else if(suspended)
        retire_closed(false);
    publish_status();
    end_exclusive();
}
bool FinalDrainGuard::try_acquire()
{
    if(!held_)
        held_ = try_begin_exclusive();
    return held_;
}
FinalDrainGuard::~FinalDrainGuard()
{
    if(held_)
        end_exclusive();
}
void FinalDrainGuard::drain()
{
    if(!held_ || !atomic_load_acquire(&initialized) || !staging)
        return;
    Ref mount;
    if(vfs_resolve(initrd_namespace_root(), "disk", 4, mount.object) == 0 &&
       !(mount.node()->allowed_rights & OS1_RIGHT_CREATE))
        return;
    (void)export_disk();
    publish_status();
}
#ifdef OS1_HOST_TEST
void test_reset()
{
    before_remove = nullptr;
    after_history_snapshot = nullptr;
    kfree(staging);
    staging = nullptr;
    initialized = suspended = dirty = false;
    busy = false;
    admission = 0;
    status = {};
    publish_status();
    kernel_cursor = last_errors = window_start = window_bytes = last_error_drain = mount_identity =
        last_poll = 0;
    next_segment = 1;
    first_stream = 0;
    for(auto& s : streams)
        s = {};
    diagnostic_log::enable_export(false);
}
#endif
}  // namespace log_export
