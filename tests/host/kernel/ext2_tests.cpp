#include <gtest/gtest.h>
#include <os1/system.h>

#include <array>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <vector>

#include "arch/x86_64/cpu/cpu.hpp"
#include "fs/ext2.hpp"
#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "platform/system_control.hpp"
#include "storage/block_device.hpp"
#include "support/lapic_stub.hpp"
#include "support/physical_memory.hpp"
#include "vfs/ext2.hpp"
#include "vfs/filesystem.hpp"
#ifdef assert
#undef assert
#endif
namespace
{
constexpr size_t start = 2048 * 512;
struct Disk
{
    std::vector<uint8_t> bytes;
    int writes = -1;
    bool fail_read = false, fail_flush = false;
    unsigned flushes = 0, total_writes = 0;
    uint32_t unreadable_block = UINT32_MAX;
    uint32_t counted_block = UINT32_MAX;
    unsigned counted_reads = 0;
    Disk()
    {
        std::ifstream file(OS1_EXT2_FIXTURE, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(file), {});
    }
    static bool read(void* ctx, uint32_t block, void* out)
    {
        auto& d = *static_cast<Disk*>(ctx);
        size_t at = start + size_t(block) * 4096;
        if(d.fail_read || block == d.unreadable_block || at + 4096 > d.bytes.size())
            return false;
        if(block == d.counted_block)
            ++d.counted_reads;
        std::memcpy(out, d.bytes.data() + at, 4096);
        return true;
    }
    static bool write(void* ctx, uint32_t block, const void* in)
    {
        auto& d = *static_cast<Disk*>(ctx);
        size_t at = start + size_t(block) * 4096;
        if(d.writes == 0 || at + 4096 > d.bytes.size())
            return false;
        if(d.writes > 0)
            --d.writes;
        ++d.total_writes;
        std::memcpy(d.bytes.data() + at, in, 4096);
        return true;
    }
    static bool flush(void* ctx)
    {
        auto& d = *static_cast<Disk*>(ctx);
        ++d.flushes;
        return !d.fail_flush;
    }
    ext2::Io io()
    {
        return {this, read, write, flush, uint32_t((bytes.size() - start) / 4096)};
    }
    static bool submit(BlockDevice& dev, BlockRequest& r)
    {
        auto& d = *static_cast<Disk*>(dev.driver_state);
        size_t at = r.sector * 512, count = r.sector_count * 512;
        bool ok = at <= d.bytes.size() && count <= d.bytes.size() - at;
        if(ok && r.operation == BlockOperation::Read)
            std::memcpy(r.buffer, d.bytes.data() + at, count);
        else if(ok && r.operation == BlockOperation::Write)
            std::memcpy(d.bytes.data() + at, r.buffer, count);
        else
            ok = r.operation == BlockOperation::Flush && !d.fail_flush;
        block_request_complete(
            r, ok ? BlockRequestStatus::Success : BlockRequestStatus::Invalid, ok ? count : 0);
        return ok;
    }
    BlockDevice device()
    {
        return {"memory", bytes.size() / 512, 512, 8, 1, this, submit, submit};
    }
};
class Ext2 : public testing::Test
{
protected:
    Disk disk;
    std::unique_ptr<ext2::Volume> fs = std::make_unique<ext2::Volume>();
    ext2::Inode root{};
    void SetUp() override
    {
        ASSERT_GT(disk.bytes.size(), start);
        ASSERT_EQ(fs->mount(disk.io()), 0);
        ASSERT_EQ(fs->inode_read(2, root), 0);
    }
};
TEST_F(Ext2, CreateIndirectSparseTruncateAndReload)
{
    uint32_t number = 0;
    ext2::Inode file{};
    ASSERT_EQ(fs->create(2, root, "test", 4, false, number, file), 0);
    std::vector<uint8_t> pattern(131072);
    for(size_t i = 0; i < pattern.size(); ++i)
        pattern[i] = uint8_t(i * 37 + i / 251);
    ASSERT_EQ(fs->write(number, file, 0, pattern.data(), pattern.size()), pattern.size());
    ASSERT_EQ(fs->sync(), 0);
    auto reboot = std::make_unique<ext2::Volume>();
    ASSERT_EQ(reboot->mount(disk.io()), 0);
    ASSERT_EQ(reboot->inode_read(number, file), 0);
    ASSERT_EQ(reboot->validate_inode(number, file), 0);
    std::vector<uint8_t> read(pattern.size());
    ASSERT_EQ(reboot->read(file, 0, read.data(), read.size()), read.size());
    EXPECT_EQ(read, pattern);
    ASSERT_EQ(reboot->truncate(number, file, 5), 0);
    ASSERT_EQ(reboot->truncate(number, file, 131072), 0);
    ASSERT_EQ(reboot->read(file, 0, read.data(), read.size()), read.size());
    for(size_t i = 0; i < read.size(); ++i)
        ASSERT_EQ(read[i], i < 5 ? pattern[i] : 0) << i;
    EXPECT_EQ(reboot->write(number, file, ext2::max_file_bytes, "x", 1), OS1_ERR_RANGE);
    EXPECT_EQ(reboot->sync(), 0);
}
TEST_F(Ext2, IndirectMappingIsReadOncePerMultiBlockRead)
{
    uint32_t number = 0;
    ext2::Inode file{};
    ASSERT_EQ(fs->create(2, root, "cached", 6, false, number, file), 0);
    constexpr size_t first_indirect = 12 * ext2::block_bytes;
    std::vector<uint8_t> data(3 * ext2::block_bytes, 0x6d);
    ASSERT_EQ(data.size(), fs->write(number, file, first_indirect, data.data(), data.size()));
    disk.counted_block = ext2::u32(file.bytes + 88);
    ASSERT_NE(0u, disk.counted_block);

    std::vector<uint8_t> actual(data.size());
    ASSERT_EQ(data.size(), fs->read(file, first_indirect, actual.data(), actual.size()));
    EXPECT_EQ(data, actual);
    EXPECT_EQ(1u, disk.counted_reads);

    ASSERT_EQ(data.size(), fs->read(file, first_indirect, actual.data(), actual.size()));
    EXPECT_EQ(data, actual);
    EXPECT_EQ(2u, disk.counted_reads);
}

TEST_F(Ext2, AlignedFullBlockOverwriteDoesNotReadOldData)
{
    uint32_t number = 0;
    ext2::Inode file{};
    ASSERT_EQ(fs->create(2, root, "block", 5, false, number, file), 0);
    std::array<uint8_t, ext2::block_bytes> data{};
    ASSERT_EQ(fs->write(number, file, 0, data.data(), data.size()), data.size());
    ASSERT_EQ(fs->get_block(file, 0, disk.unreadable_block), 0);
    ASSERT_NE(0u, disk.unreadable_block);

    data.fill(0x6d);
    ASSERT_EQ(fs->write(number, file, 0, data.data(), data.size()), data.size());
    disk.unreadable_block = UINT32_MAX;
    std::array<uint8_t, ext2::block_bytes> actual{};
    ASSERT_EQ(fs->read(file, 0, actual.data(), actual.size()), actual.size());
    EXPECT_EQ(data, actual);
}

TEST_F(Ext2, RejectsFeaturesGeometryDirtyStateAndBitmapCounters)
{
    const auto original = disk.bytes;
    for(auto [offset, value] : std::vector<std::pair<unsigned, unsigned>>{
            {96, 0x42}, {92, 4}, {100, 4}, {24, 3}, {4, UINT32_MAX}, {40, 0}, {58, 2}, {12, 1}})
    {
        disk.bytes = original;
        ext2::put32(disk.bytes.data() + start + 1024 + offset, value);
        auto v = std::make_unique<ext2::Volume>();
        EXPECT_EQ(v->mount(disk.io()), OS1_ERR_BAD_IMAGE) << offset;
    }
}
TEST_F(Ext2, DirtyStateClearsCleanBitWithoutSettingErrorBit)
{
    const auto* state = disk.bytes.data() + start + 1024 + 58;
    ASSERT_EQ(ext2::u16(state), 1u);
    const unsigned writes_before = disk.total_writes;
    const unsigned flushes_before = disk.flushes;
    ASSERT_EQ(fs->begin_write(), 0);
    EXPECT_EQ(ext2::u16(state), 0u);
    EXPECT_EQ(disk.total_writes - writes_before, 1u);
    EXPECT_EQ(disk.flushes - flushes_before, 1u);

    auto reopened = std::make_unique<ext2::Volume>();
    EXPECT_EQ(reopened->mount(disk.io()), OS1_ERR_BAD_IMAGE);
    ASSERT_EQ(fs->sync(), 0);
    EXPECT_EQ(ext2::u16(state), 1u);
    EXPECT_EQ(disk.total_writes - writes_before, 2u);
    EXPECT_EQ(disk.flushes - flushes_before, 3u);
    reopened = std::make_unique<ext2::Volume>();
    EXPECT_EQ(reopened->mount(disk.io()), 0);
}
TEST_F(Ext2, RejectsMetadataAndDuplicateBlockReferences)
{
    ext2::Inode file{};
    ASSERT_EQ(fs->inode_read(11, file), 0);
    ext2::put32(file.bytes + 40, 2);
    EXPECT_EQ(fs->validate_inode(11, file), OS1_ERR_BAD_IMAGE);
    fs = std::make_unique<ext2::Volume>();
    ASSERT_EQ(fs->mount(disk.io()), 0);
    ASSERT_EQ(fs->inode_read(11, file), 0);
    ASSERT_EQ(fs->validate_inode(11, file), 0);
    EXPECT_EQ(fs->validate_inode(12, file), OS1_ERR_BAD_IMAGE);
}
TEST_F(Ext2, FailedWriteAndFlushPoisonVolumeAndDoNotReportSyncSuccess)
{
    uint32_t number = 0;
    ext2::Inode file{};
    ASSERT_EQ(fs->create(2, root, "test", 4, false, number, file), 0);
    disk.writes = 0;
    EXPECT_EQ(fs->write(number, file, 0, "abc", 3), OS1_ERR_IO);
    EXPECT_EQ(fs->sync(), OS1_ERR_IO);
    disk.writes = -1;
    auto reboot = std::make_unique<ext2::Volume>();
    EXPECT_EQ(reboot->mount(disk.io()), OS1_ERR_BAD_IMAGE);
}
TEST_F(Ext2, FlushFailureBeforeFirstMutationPreservesNamespace)
{
    disk.fail_flush = true;
    uint32_t number = 0;
    ext2::Inode file{};
    EXPECT_EQ(fs->create(2, root, "test", 4, false, number, file), OS1_ERR_IO);
    EXPECT_TRUE(fs->failed);
    EXPECT_EQ(fs->sync(), OS1_ERR_IO);
}
TEST_F(Ext2, DirectoryMutationAndCapacity)
{
    uint32_t number = 0;
    ext2::Inode directory{};
    ASSERT_EQ(fs->create(2, root, "sub", 3, true, number, directory), 0);
    EXPECT_EQ(ext2::u16(root.bytes + 26), 3);
    ASSERT_EQ(fs->rename(2, root, number, directory, "sub", 3, "renamed", 7), 0);
    ASSERT_EQ(fs->remove(2, root, "renamed", 7, number, directory), 0);
    EXPECT_EQ(ext2::u16(root.bytes + 26), 2);
    ASSERT_EQ(fs->sync(), 0);
    auto reload = std::make_unique<ext2::Volume>();
    ASSERT_EQ(reload->mount(disk.io()), 0);
    ASSERT_EQ(reload->inode_read(2, root), 0);
    unsigned count = 0;
    for(unsigned i = 0; i < 200; ++i)
    {
        char name[16];
        auto len = snprintf(name, sizeof(name), "f%u", i);
        ext2::Inode f{};
        long r = reload->create(2, root, name, len, false, number, f);
        if(r == OS1_ERR_LIMIT)
            break;
        ASSERT_EQ(r, 0);
        ++count;
    }
    EXPECT_EQ(count, 116u);  // 128 inodes minus 10 reserved and two seed files.
}
TEST_F(Ext2, DiskFullRejectsWriteWithoutChangingSize)
{
    uint32_t n = 0;
    ext2::Inode f{};
    ASSERT_EQ(fs->create(2, root, "full", 4, false, n, f), 0);
    std::array<uint8_t, 4096> bytes{};
    uint32_t at = 0;
    for(;;)
    {
        long r = fs->write(n, f, at, bytes.data(), bytes.size());
        if(r == OS1_ERR_LIMIT)
            break;
        ASSERT_EQ(r, 4096);
        at += 4096;
        ASSERT_LT(at, ext2::max_file_bytes);
    }
    EXPECT_EQ(ext2::size(f), at);
    EXPECT_EQ(fs->sync(), 0);
}
TEST_F(Ext2, MalformedDirectoryRecordsFail)
{
    uint32_t b = ext2::u32(root.bytes + 40);
    ext2::put16(disk.bytes.data() + start + b * 4096 + 4, 0);
    ext2::Entry entry{};
    uint64_t next = 0;
    EXPECT_EQ(fs->next(root, 0, entry, next), OS1_ERR_BAD_IMAGE);
}
TEST_F(Ext2, BlockFacadeRejectsOverflowAndPartitionEscape)
{
    auto device = disk.device();
    std::array<uint8_t, 512> bytes{};
    EXPECT_FALSE(block_read_sync(device, UINT64_MAX, bytes.data(), 1));
    EXPECT_FALSE(block_write_sync(device, device.sector_count, bytes.data(), 1));
    EXPECT_TRUE(block_flush_sync(device));
}
TEST_F(Ext2, VfsMountAuthoritySnapshotAndUnlinkPolicy)
{
    os1::host_test::PhysicalMemoryArena arena{16 * 1024 * 1024};
    PageFrameContainer frames;
    std::array<BootMemoryRegion, 1> regions{{{0, arena.size(), BootMemoryType::Usable, 0}}};
    ASSERT_TRUE(frames.initialize(regions, 0x300000, kPageFrameBitmapQwordLimit));
    kmem_init(frames);
    VfsNode root_node{};
    root_node.kind = OS1_OBJECT_DIRECTORY;
    auto device = disk.device();
    ASSERT_EQ(ext2_mount(&root_node, device), 0);
    auto* mount = root_node.children;
    ASSERT_NE(mount, nullptr);
    NativeObject* object = nullptr;
    ASSERT_EQ(mount->operations->lookup(mount, "diskutil", 8, object), 0);
    auto* executable = static_cast<VfsNode*>(object);
    EXPECT_TRUE(object->allowed_rights & OS1_RIGHT_EXECUTE);
    EXPECT_FALSE(object->allowed_rights & OS1_RIGHT_WRITE);
    uint8_t* snapshot = nullptr;
    uint64_t length = 0;
    ASSERT_EQ(executable->operations->snapshot(executable, snapshot, length), 0);
    EXPECT_GT(length, 0u);
    kfree(snapshot);
    object_release(object);
    OpenInstance* opened = nullptr;
    ASSERT_EQ(mount->operations->create(mount, "new", 3, OS1_OBJECT_FILE, opened), 0);
    EXPECT_FALSE(opened->object->allowed_rights & OS1_RIGHT_EXECUTE);
    EXPECT_EQ(mount->operations->remove(mount, "new", 3, 0), OS1_ERR_WOULD_BLOCK);
    instance_release(opened);
    EXPECT_EQ(mount->operations->remove(mount, "new", 3, 0), 0);
    EXPECT_EQ(mount->operations->sync(mount), 0);
    object_release(mount);
    root_node.children = nullptr;
    ext2::put32(disk.bytes.data() + 454, UINT32_MAX);
    EXPECT_EQ(ext2_mount(&root_node, device), OS1_ERR_BAD_IMAGE);
}
TEST_F(Ext2, ReadOnlyMountKeepsReadsAndSkipsDeviceFlush)
{
    os1::host_test::PhysicalMemoryArena arena{16 * 1024 * 1024};
    PageFrameContainer frames;
    std::array<BootMemoryRegion, 1> regions{{{0, arena.size(), BootMemoryType::Usable, 0}}};
    ASSERT_TRUE(frames.initialize(regions, 0x300000, kPageFrameBitmapQwordLimit));
    kmem_init(frames);

    const auto original = disk.bytes;
    disk.fail_flush = true;
    auto device = disk.device();
    device.read_only = true;
    VfsNode root{};
    root.kind = OS1_OBJECT_DIRECTORY;
    ASSERT_EQ(ext2_mount(&root, device), 0);
    auto* mount = root.children;
    ASSERT_NE(mount, nullptr);
    EXPECT_FALSE(mount->allowed_rights & OS1_RIGHT_CREATE);

    OpenInstance* opened = nullptr;
    EXPECT_EQ(mount->operations->create(mount, "new", 3, OS1_OBJECT_FILE, opened), OS1_ERR_ACCESS);
    EXPECT_EQ(opened, nullptr);
    NativeObject* object = nullptr;
    ASSERT_EQ(mount->operations->lookup(mount, "readme", 6, object), 0);
    auto* file = static_cast<VfsNode*>(object);
    EXPECT_FALSE(file->allowed_rights & OS1_RIGHT_WRITE);
    EXPECT_FALSE(file->interfaces & OS1_INTERFACE_BYTE_OUTPUT);
    EXPECT_EQ(file->operations->truncate(file, 0), OS1_ERR_ACCESS);
    EXPECT_EQ(file->operations->set_times(file, OS1_TIME_MODIFY), OS1_ERR_ACCESS);
    char contents[10]{};
    EXPECT_EQ(file->operations->read(file, 0, contents, sizeof(contents)), sizeof(contents));
    EXPECT_EQ(std::memcmp(contents, "Persistent", sizeof(contents)), 0);
    EXPECT_EQ(mount->operations->sync(mount), 0);
    EXPECT_EQ(ext2_system_sync(true), 0);
    ext2_system_thaw();
    EXPECT_TRUE(disk.bytes == original);

    object_release(file);
    object_release(mount);
}

}  // namespace

TEST_F(Ext2, GrowthClearsForeignNonzeroTail)
{
    ext2::Inode file{};
    ASSERT_EQ(fs->inode_read(12, file), 0);
    const uint32_t old = ext2::size(file), b = ext2::u32(file.bytes + 40);
    std::memset(disk.bytes.data() + start + b * 4096 + old, 0xa5, 4096 - old);
    ASSERT_EQ(fs->write(12, file, old + 100, "x", 1), 1);
    std::array<uint8_t, 101> out{};
    ASSERT_EQ(fs->read(file, old, out.data(), out.size()), 101);
    for(unsigned i = 0; i < 100; ++i)
        EXPECT_EQ(out[i], 0);
    EXPECT_EQ(out[100], 'x');
    const uint32_t after = ext2::size(file);
    std::memset(disk.bytes.data() + start + b * 4096 + after, 0x5a, 4096 - after);
    ASSERT_EQ(fs->truncate(12, file, after + 100), 0);
    ASSERT_EQ(fs->read(file, after, out.data(), 100), 100);
    for(unsigned i = 0; i < 100; ++i)
        EXPECT_EQ(out[i], 0);
}

#include "drivers/block/completion.hpp"
TEST(BlockCompletion, ObservedDoneWaitsForCallbackRetirement)
{
    Spinlock queue_lock("completion-owner");
    BlockRequest request{};
    queue_lock.lock();
    // The IRQ has published done but still owns the request's wait queue.
    block_request_complete(request, BlockRequestStatus::Success, 4096);
    std::promise<void> entered;
    auto started = entered.get_future();
    bool drained = false;
    auto poller = std::async(std::launch::async, [&] {
        entered.set_value();
        return poll_block_completion(request, queue_lock, [&] {
            EXPECT_TRUE(queue_lock.locked());
            drained = true;
        });
    });
    started.wait();
    const auto before_retirement = poller.wait_for(std::chrono::milliseconds(50));
    queue_lock.unlock();
    EXPECT_EQ(before_retirement, std::future_status::timeout);
    EXPECT_TRUE(poller.get());
    EXPECT_TRUE(drained);
    EXPECT_FALSE(queue_lock.locked());
    EXPECT_EQ(request.bytes_transferred, 4096u);
}

TEST(BlockCompletion, BoundaryCompletionWinsOverDeviceReset)
{
    BlockRequest request{};
    bool reset = false;
    EXPECT_TRUE(finish_block_poll(
        request,
        [&] { block_request_complete(request, BlockRequestStatus::Success); },
        [&] { reset = true; }));
    EXPECT_FALSE(reset);
    EXPECT_TRUE(block_request_succeeded(request));
    BlockRequest pending{};
    EXPECT_FALSE(finish_block_poll(pending, [] {}, [&] { reset = true; }));
    EXPECT_TRUE(reset);
}

TEST_F(Ext2, MountRejectsReachableReservedInode)
{
    // Move the seed utility into reserved inode 3 and repair all allocation counts.
    // The shape was previously accepted despite inode 3 being reserved by ext2.
    auto* table = disk.bytes.data() + start + 4 * 4096;
    std::memcpy(table + 2 * 128, table + 10 * 128, 128);
    std::memset(table + 10 * 128, 0, 128);
    auto* bits = disk.bytes.data() + start + 3 * 4096;
    bits[10 / 8] &= ~(1u << (10 % 8));
    auto* super = disk.bytes.data() + start + 1024;
    auto* group = disk.bytes.data() + start + 4096;
    ext2::put32(super + 16, ext2::u32(super + 16) + 1);
    ext2::put16(group + 14, ext2::u16(group + 14) + 1);
    const uint32_t root_block = ext2::u32(table + 128 + 40);
    ext2::put32(disk.bytes.data() + start + root_block * 4096 + 24, 3);
    os1::host_test::PhysicalMemoryArena arena{16 * 1024 * 1024};
    PageFrameContainer frames;
    std::array<BootMemoryRegion, 1> regions{{{0, arena.size(), BootMemoryType::Usable, 0}}};
    ASSERT_TRUE(frames.initialize(regions, 0x300000, kPageFrameBitmapQwordLimit));
    kmem_init(frames);
    VfsNode root_node{};
    root_node.kind = OS1_OBJECT_DIRECTORY;
    auto device = disk.device();
    EXPECT_EQ(ext2_mount(&root_node, device), OS1_ERR_BAD_IMAGE);
    EXPECT_EQ(root_node.children, nullptr);
}

TEST_F(Ext2, MoveAcrossParentsReplacesAndPersistsDirectoryParentLinks)
{
    uint32_t left_id, right_id, file_id, target_id, sub_id;
    ext2::Inode left{}, right{}, file{}, target{}, sub{};
    ASSERT_EQ(fs->create(2, root, "left", 4, true, left_id, left), 0);
    ASSERT_EQ(fs->create(2, root, "right", 5, true, right_id, right), 0);
    ASSERT_EQ(fs->create(left_id, left, "file", 4, false, file_id, file), 0);
    ASSERT_EQ(fs->write(file_id, file, 0, "kept", 4), 4);
    ASSERT_EQ(fs->create(right_id, right, "target", 6, false, target_id, target), 0);
    ASSERT_EQ(fs->write(target_id, target, 0, "old", 3), 3);
    std::array<uint8_t, 8192> scratch{};
    ASSERT_EQ(fs->move(left_id,
                       left,
                       "file",
                       4,
                       right_id,
                       right,
                       "target",
                       6,
                       file_id,
                       file,
                       target_id,
                       &target,
                       scratch.data()),
              0);
    ASSERT_EQ(fs->create(left_id, left, "sub", 3, true, sub_id, sub), 0);
    auto old_left_links = ext2::u16(left.bytes + 26), old_right_links = ext2::u16(right.bytes + 26);
    ASSERT_EQ(fs->move(left_id,
                       left,
                       "sub",
                       3,
                       right_id,
                       right,
                       "sub",
                       3,
                       sub_id,
                       sub,
                       0,
                       nullptr,
                       scratch.data()),
              0);
    EXPECT_EQ(ext2::u16(left.bytes + 26), old_left_links - 1);
    EXPECT_EQ(ext2::u16(right.bytes + 26), old_right_links + 1);
    ASSERT_EQ(fs->sync(), 0);
    auto reboot = std::make_unique<ext2::Volume>();
    ASSERT_EQ(reboot->mount(disk.io()), 0);
    ASSERT_EQ(reboot->inode_read(file_id, file), 0);
    char bytes[4];
    EXPECT_EQ(reboot->read(file, 0, bytes, 4), 4);
    EXPECT_EQ(std::string(bytes, 4), "kept");
    uint32_t block = 0;
    ASSERT_EQ(reboot->get_block(sub, 0, block), 0);
    std::array<uint8_t, 4096> directory{};
    ASSERT_TRUE(Disk::read(&disk, block, directory.data()));
    uint32_t parent = 0;
    memcpy(&parent, directory.data() + 12, 4);
    EXPECT_EQ(parent, right_id);
    ASSERT_EQ(reboot->validate_inode(sub_id, sub), 0);
}
TEST_F(Ext2, MoveIoFailurePoisonsVolumeInsteadOfReportingSuccess)
{
    uint32_t file_id;
    ext2::Inode file{};
    ASSERT_EQ(fs->create(2, root, "source", 6, false, file_id, file), 0);
    std::array<uint8_t, 8192> scratch{};
    disk.writes = 0;
    EXPECT_LT(
        fs->move(
            2, root, "source", 6, 2, root, "target", 6, file_id, file, 0, nullptr, scratch.data()),
        0);
    EXPECT_LT(fs->sync(), 0);
}

TEST_F(Ext2, CrossDirectoryMoveKeepsSourceUntilDestinationFlush)
{
    for(bool fail_flush : {false, true})
    {
        SCOPED_TRACE(fail_flush ? "destination flush fails" : "second directory write fails");
        Disk fixture;
        auto volume = std::make_unique<ext2::Volume>();
        ASSERT_EQ(volume->mount(fixture.io()), 0);
        ext2::Inode parent{}, left{}, right{}, source{};
        uint32_t left_id, right_id, source_id;
        ASSERT_EQ(volume->inode_read(2, parent), 0);
        ASSERT_EQ(volume->create(2, parent, "left", 4, true, left_id, left), 0);
        ASSERT_EQ(volume->create(2, parent, "right", 5, true, right_id, right), 0);
        ASSERT_EQ(volume->create(left_id, left, "source", 6, false, source_id, source), 0);
        ASSERT_EQ(volume->write(source_id, source, 0, "kept", 4), 4);
        ASSERT_TRUE(volume->dirty);

        const auto writes_before = fixture.total_writes;
        const auto flushes_before = fixture.flushes;
        fixture.fail_flush = fail_flush;
        fixture.writes = fail_flush ? -1 : 1;
        std::array<uint8_t, 2 * ext2::block_bytes> scratch{};
        EXPECT_EQ(volume->move(left_id,
                               left,
                               "source",
                               6,
                               right_id,
                               right,
                               "target",
                               6,
                               source_id,
                               source,
                               0,
                               nullptr,
                               scratch.data()),
                  OS1_ERR_IO);
        EXPECT_TRUE(volume->failed);
        EXPECT_EQ(fixture.total_writes - writes_before, 1u);
        EXPECT_EQ(fixture.flushes - flushes_before, 1u);
        EXPECT_EQ(volume->sync(), OS1_ERR_IO);

        // Inspect the two directory blocks without mounting the dirty image.
        auto inspector = std::make_unique<ext2::Volume>(*volume);
        inspector->failed = false;
        const auto entry_inode = [&](ext2::Inode& directory, const char* name) -> uint32_t {
            uint64_t offset = 0;
            for(;;)
            {
                ext2::Entry entry{};
                long result = inspector->next(directory, offset, entry, offset);
                EXPECT_GE(result, 0);
                if(result <= 0)
                    return 0;
                if(std::string(entry.name, entry.length) == name)
                    return entry.inode;
            }
        };
        EXPECT_EQ(entry_inode(left, "source"), source_id);
        EXPECT_EQ(entry_inode(right, "target"), source_id);
        char bytes[4]{};
        ASSERT_EQ(inspector->read(source, 0, bytes, sizeof(bytes)), 4);
        EXPECT_EQ(std::string(bytes, sizeof(bytes)), "kept");
    }
}

TEST_F(Ext2, EmptyDirectoryReplacementUpdatesParentLinksInBothMoveForms)
{
    for(bool same_parent : {true, false})
    {
        Disk fixture;
        auto volume = std::make_unique<ext2::Volume>();
        ASSERT_EQ(volume->mount(fixture.io()), 0);
        ext2::Inode root_node{}, left{}, right{}, source{}, target{};
        uint32_t l, r, s, t;
        ASSERT_EQ(volume->inode_read(2, root_node), 0);
        ASSERT_EQ(volume->create(2, root_node, "left", 4, true, l, left), 0);
        ASSERT_EQ(volume->create(2, root_node, "right", 5, true, r, right), 0);
        auto& destination = same_parent ? left : right;
        uint32_t dest_id = same_parent ? l : r;
        ASSERT_EQ(volume->create(l, left, "source", 6, true, s, source), 0);
        ASSERT_EQ(volume->create(dest_id, destination, "target", 6, true, t, target), 0);
        auto before_left = ext2::u16(left.bytes + 26), before_right = ext2::u16(right.bytes + 26);
        const auto flushes_before = fixture.flushes;
        std::array<uint8_t, 8192> scratch{};
        ASSERT_EQ(volume->move(l,
                               left,
                               "source",
                               6,
                               dest_id,
                               destination,
                               "target",
                               6,
                               s,
                               source,
                               t,
                               &target,
                               scratch.data()),
                  0);
        EXPECT_EQ(fixture.flushes - flushes_before, same_parent ? 0u : 1u);
        EXPECT_EQ(ext2::u16(left.bytes + 26), before_left - 1);
        EXPECT_EQ(ext2::u16(right.bytes + 26), before_right);
        ASSERT_EQ(volume->sync(), 0);
        auto reboot = std::make_unique<ext2::Volume>();
        ASSERT_EQ(reboot->mount(fixture.io()), 0);
        ext2::Inode loaded{};
        ASSERT_EQ(reboot->inode_read(l, loaded), 0);
        EXPECT_EQ(ext2::u16(loaded.bytes + 26), before_left - 1);
        ASSERT_EQ(reboot->validate_inode(s, source), 0);
    }
}

#include "vfs/namespace.hpp"
TEST_F(Ext2, MetadataCountsIndirectBlocksAndReportsAllocatorCapacity)
{
    os1::host_test::PhysicalMemoryArena arena{16 * 1024 * 1024};
    PageFrameContainer frames;
    std::array<BootMemoryRegion, 1> regions{{{0, arena.size(), BootMemoryType::Usable, 0}}};
    ASSERT_TRUE(frames.initialize(regions, 0x300000, kPageFrameBitmapQwordLimit));
    kmem_init(frames);
    VfsNode root_node{};
    root_node.kind = OS1_OBJECT_DIRECTORY;
    auto device = disk.device();
    ASSERT_EQ(0, ext2_mount(&root_node, device));
    auto* mount = root_node.children;
    os1_space before{}, after{};
    {
        NamespaceGuard guard;
        ASSERT_EQ(0, vfs_space(mount, before));
    }
    OpenInstance* opened = nullptr;
    ASSERT_EQ(0, mount->operations->create(mount, "sparse", 6, OS1_OBJECT_FILE, opened));
    auto* file = static_cast<VfsNode*>(opened->object);
    ASSERT_EQ(1, file->operations->write(file, 50000, "x", 1, false, nullptr));
    os1_metadata metadata{};
    {
        NamespaceGuard guard;
        ASSERT_EQ(0, vfs_metadata(file, metadata));
        ASSERT_EQ(0, vfs_space(mount, after));
    }
    EXPECT_EQ(metadata.logical_bytes, 50001u);
    EXPECT_EQ(metadata.allocated_bytes, 8192u);
    EXPECT_EQ(before.free_bytes - after.free_bytes, 8192u);
    EXPECT_EQ(after.available_bytes, after.free_bytes);
    ASSERT_EQ(0, file->operations->truncate(file, 0));
    {
        NamespaceGuard guard;
        ASSERT_EQ(0, vfs_metadata(file, metadata));
    }
    EXPECT_EQ(metadata.allocated_bytes, 0u);
    instance_release(opened);
    ASSERT_EQ(0, mount->operations->remove(mount, "sparse", 6, OS1_OBJECT_FILE));
    object_release(mount);
}

TEST_F(Ext2, AppendBatchUsesCurrentEndUnderVolumeGuard)
{
    os1::host_test::PhysicalMemoryArena arena{16 * 1024 * 1024};
    PageFrameContainer frames;
    std::array<BootMemoryRegion, 1> regions{{{0, arena.size(), BootMemoryType::Usable, 0}}};
    ASSERT_TRUE(frames.initialize(regions, 0x300000, kPageFrameBitmapQwordLimit));
    kmem_init(frames);
    VfsNode root{};
    root.kind = OS1_OBJECT_DIRECTORY;
    auto device = disk.device();
    ASSERT_EQ(0, ext2_mount(&root, device));
    auto* mount = root.children;
    OpenInstance* opened = nullptr;
    ASSERT_EQ(0, mount->operations->create(mount, "append", 6, OS1_OBJECT_FILE, opened));
    auto* file = static_cast<VfsNode*>(opened->object);
    std::array<uint8_t, ext2::block_bytes> data{};
    ASSERT_EQ(data.size(),
              file->operations->write(file, 0, data.data(), data.size(), false, nullptr));
    ASSERT_EQ(256, file->operations->write(file, 0, data.data(), 256, true, nullptr));

    uint64_t end = 0;
    ASSERT_EQ(data.size() - 256,
              file->operations->write(file, data.size(), data.data(), data.size(), true, &end));
    EXPECT_EQ(2 * data.size(), end);
    EXPECT_EQ(end, file->byte_size);
    instance_release(opened);
    ASSERT_EQ(0, mount->operations->remove(mount, "append", 6, OS1_OBJECT_FILE));
    object_release(mount);
}

TEST_F(Ext2, TimestampsPersistAndReadsDoNotWrite)
{
    uint64_t now = 1700000000123456789ull;
    struct Clock
    {
        static uint64_t& value()
        {
            static uint64_t time;
            return time;
        }
        static bool read(void*, uint64_t& out)
        {
            out = value();
            return out != 0;
        }
    };
    Clock::value() = now;
    fs->io.clock = Clock::read;
    uint32_t number = 0;
    ext2::Inode inode{};
    ASSERT_EQ(0, fs->create(2, root, "times", 5, false, number, inode));
    EXPECT_EQ(ext2::u32(inode.bytes + 8), 1700000000u);
    EXPECT_EQ(ext2::u32(inode.bytes + 12), 1700000000u);
    EXPECT_EQ(ext2::u32(inode.bytes + 16), 1700000000u);
    EXPECT_EQ(ext2::u32(root.bytes + 16), 1700000000u);
    Clock::value() += 2000000000;
    ASSERT_EQ(3, fs->write(number, inode, 0, "abc", 3));
    EXPECT_EQ(ext2::u32(inode.bytes + 8), 1700000000u);
    EXPECT_EQ(ext2::u32(inode.bytes + 16), 1700000002u);
    disk.writes = 0;
    char bytes[3]{};
    EXPECT_EQ(3, fs->read(inode, 0, bytes, 3));
    EXPECT_EQ(std::string(bytes, 3), "abc");
    EXPECT_FALSE(fs->failed);
    disk.writes = -1;
    Clock::value() += 2000000000;
    ASSERT_EQ(0, fs->set_times(number, inode, OS1_TIME_ACCESS));
    EXPECT_EQ(ext2::u32(inode.bytes + 8), 1700000004u);
    EXPECT_EQ(ext2::u32(inode.bytes + 16), 1700000002u);
    EXPECT_EQ(ext2::u32(inode.bytes + 12), 1700000004u);
    EXPECT_EQ(0, fs->sync());
    auto reload = std::make_unique<ext2::Volume>();
    ASSERT_EQ(0, reload->mount(disk.io()));
    ext2::Inode persisted{};
    ASSERT_EQ(0, reload->inode_read(number, persisted));
    EXPECT_EQ(0, std::memcmp(&inode, &persisted, sizeof(inode)));
    Clock::value() = 0;
    const auto unchanged = inode;
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, fs->set_times(number, inode, 3));
    EXPECT_EQ(0, std::memcmp(&inode, &unchanged, sizeof(inode)));
    EXPECT_EQ(OS1_ERR_INVALID, fs->set_times(number, inode, OS1_TIME_CHANGE));
    Clock::value() = (uint64_t(UINT32_MAX) + 1) * 1000000000;
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, fs->set_times(number, inode, 3));
    Clock::value() = now;
    disk.writes = 0;
    EXPECT_EQ(OS1_ERR_IO, fs->set_times(number, inode, 3));
    EXPECT_EQ(0, std::memcmp(&inode, &unchanged, sizeof(inode)));
    EXPECT_TRUE(fs->failed);
}

TEST_F(Ext2, AppendReusesMetadataAndWritesThroughNeighboringInodes)
{
    uint32_t first_number = 0, second_number = 0;
    ext2::Inode first{}, second{};
    ASSERT_EQ(0, fs->create(2, root, "first", 5, false, first_number, first));
    ASSERT_EQ(0, fs->create(2, root, "second", 6, false, second_number, second));
    ASSERT_EQ((first_number - 1) / 32, (second_number - 1) / 32);

    std::vector<char> initial(12 * ext2::block_bytes + 256, 'x');
    ASSERT_EQ(initial.size(), fs->write(first_number, first, 0, initial.data(), initial.size()));
    ASSERT_EQ(1, fs->write(second_number, second, 0, "a", 1));

    disk.counted_block = ext2::u32(first.bytes + 88);
    ASSERT_NE(0u, disk.counted_block);
    disk.counted_reads = 0;
    EXPECT_EQ(1, fs->write(first_number, first, initial.size(), "b", 1));
    EXPECT_EQ(1u, disk.counted_reads);

    const uint32_t table = 4 + (first_number - 1) / 32;
    disk.counted_block = table;
    disk.counted_reads = 0;
    ASSERT_EQ(1, fs->write(second_number, second, 1, "c", 1));
    EXPECT_EQ(0u, disk.counted_reads);
    const auto* block = disk.bytes.data() + start + table * ext2::block_bytes;
    EXPECT_EQ(initial.size() + 1, ext2::u32(block + ((first_number - 1) % 32) * 128 + 4));
    EXPECT_EQ(2u, ext2::u32(block + ((second_number - 1) % 32) * 128 + 4));

    ASSERT_EQ(0, fs->sync());
    auto remount = std::make_unique<ext2::Volume>();
    ASSERT_EQ(0, remount->mount(disk.io()));
    ASSERT_EQ(0, remount->inode_read(first_number, first));
    ASSERT_EQ(0, remount->inode_read(second_number, second));
    char tail = 0;
    ASSERT_EQ(1, remount->read(first, initial.size(), &tail, 1));
    EXPECT_EQ('b', tail);
    std::array<char, 2> data{};
    ASSERT_EQ(data.size(), remount->read(second, 0, data.data(), data.size()));
    EXPECT_EQ((std::array<char, 2>{'a', 'c'}), data);
}

TEST_F(Ext2, TimestampMutationAndWriteCost)
{
    auto workload = [](bool timed) {
        Disk disk;
        auto volume = std::make_unique<ext2::Volume>();
        ASSERT_EQ(0, volume->mount(disk.io()));
        if(timed)
            volume->io.clock = [](void*, uint64_t& now) {
                now = 1700000000000000000ull;
                return true;
            };
        ext2::Inode root{}, inode{};
        ASSERT_EQ(0, volume->inode_read(2, root));
        uint32_t number = 0;
        unsigned start_writes = disk.total_writes;
        ASSERT_EQ(0, volume->create(2, root, "a", 1, false, number, inode));
        const auto create_writes = disk.total_writes - start_writes;
        start_writes = disk.total_writes;
        ASSERT_EQ(1, volume->write(number, inode, 0, "x", 1));
        const auto content_writes = disk.total_writes - start_writes;
        start_writes = disk.total_writes;
        ASSERT_EQ(0, volume->rename(2, root, number, inode, "a", 1, "b", 1));
        const auto rename_writes = disk.total_writes - start_writes;
        if(timed)
        {
            EXPECT_EQ(ext2::u32(root.bytes + 16), 1700000000u);
            EXPECT_EQ(ext2::u32(inode.bytes + 12), 1700000000u);
        }
        start_writes = disk.total_writes;
        ASSERT_EQ(0, volume->truncate(number, inode, 0));
        const auto truncate_writes = disk.total_writes - start_writes;
        start_writes = disk.total_writes;
        ASSERT_EQ(0, volume->remove(2, root, "b", 1, number, inode));
        const auto remove_writes = disk.total_writes - start_writes;
        std::cout << "timestamp_io clock=" << timed << " create=" << create_writes
                  << " write=" << content_writes << " rename=" << rename_writes
                  << " truncate=" << truncate_writes << " remove=" << remove_writes << '\n';
        ASSERT_EQ(0, volume->sync());
    };
    workload(false);
    workload(true);
}

TEST_F(Ext2, NamespaceChangesAdvanceParentAndObjectTimes)
{
    struct Clock
    {
        static uint64_t& value()
        {
            static uint64_t time;
            return time;
        }
        static bool read(void*, uint64_t& out)
        {
            out = value();
            return true;
        }
    };
    Clock::value() = 1700000000000000000ull;
    fs->io.clock = Clock::read;
    uint32_t file_number = 0, directory_number = 0;
    ext2::Inode file{}, directory{};
    ASSERT_EQ(0, fs->create(2, root, "a", 1, false, file_number, file));
    Clock::value() += 1000000000;
    ASSERT_EQ(0, fs->rename(2, root, file_number, file, "a", 1, "b", 1));
    EXPECT_EQ(ext2::u32(root.bytes + 16), 1700000001u);
    EXPECT_EQ(ext2::u32(file.bytes + 12), 1700000001u);
    EXPECT_EQ(ext2::u32(file.bytes + 16), 1700000000u);
    ASSERT_EQ(0, fs->create(2, root, "sub", 3, true, directory_number, directory));
    Clock::value() += 1000000000;
    std::array<uint8_t, 8192> scratch{};
    ASSERT_EQ(0,
              fs->move(2,
                       root,
                       "b",
                       1,
                       directory_number,
                       directory,
                       "c",
                       1,
                       file_number,
                       file,
                       0,
                       nullptr,
                       scratch.data()));
    EXPECT_EQ(ext2::u32(root.bytes + 16), 1700000002u);
    EXPECT_EQ(ext2::u32(directory.bytes + 16), 1700000002u);
    EXPECT_EQ(ext2::u32(file.bytes + 12), 1700000002u);
    EXPECT_EQ(ext2::u32(file.bytes + 16), 1700000000u);
    Clock::value() += 1000000000;
    ASSERT_EQ(0, fs->remove(directory_number, directory, "c", 1, file_number, file));
    EXPECT_EQ(ext2::u32(directory.bytes + 16), 1700000003u);
    EXPECT_EQ(ext2::u32(directory.bytes + 12), 1700000003u);
    ASSERT_EQ(0, fs->sync());
    auto reload = std::make_unique<ext2::Volume>();
    ASSERT_EQ(0, reload->mount(disk.io()));
    ext2::Inode persisted{};
    ASSERT_EQ(0, reload->inode_read(directory_number, persisted));
    EXPECT_EQ(ext2::u32(persisted.bytes + 16), 1700000003u);
}

void host_console_output_clear();
const std::string& host_console_output();

TEST_F(Ext2, SystemSyncFreezesWritesAndCanThawAfterFailedTransition)
{
    os1::host_test::PhysicalMemoryArena arena{16 * 1024 * 1024};
    PageFrameContainer frames;
    std::array<BootMemoryRegion, 1> regions{{{0, arena.size(), BootMemoryType::Usable, 0}}};
    ASSERT_TRUE(frames.initialize(regions, 0x300000, kPageFrameBitmapQwordLimit));
    kmem_init(frames);
    VfsNode root_node{};
    root_node.kind = OS1_OBJECT_DIRECTORY;
    auto device = disk.device();
    ASSERT_EQ(0, ext2_mount(&root_node, device));
    auto* mount = root_node.children;
    OpenInstance* opened = nullptr;
    ASSERT_EQ(0, mount->operations->create(mount, "lifecycle", 9, OS1_OBJECT_FILE, opened));
    instance_release(opened);
    EXPECT_EQ(0, ext2::u16(disk.bytes.data() + start + 1082));
    ASSERT_EQ(0, ext2_system_sync(true));
    EXPECT_EQ(1, ext2::u16(disk.bytes.data() + start + 1082));
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, mount->operations->remove(mount, "lifecycle", 9, 0));
    ext2_system_thaw();
    host_console_output_clear();
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, system_control(OS1_SYSTEM_SHUTDOWN));
    EXPECT_EQ("system: shutdown stage=acpi-tables detail=not-initialized\n", host_console_output());
    EXPECT_EQ(0, mount->operations->remove(mount, "lifecycle", 9, 0));
    EXPECT_EQ(0, ext2::u16(disk.bytes.data() + start + 1082));
    cpu boot{}, peer{};
    peer.id = 1;
    peer.booted = static_cast<uint32_t>(ApStartupState::Online);
    boot.next = &peer;
    auto* saved_boot = g_cpu_boot;
    auto* saved_current = g_cpu_host_current;
    g_cpu_boot = &boot;
    g_cpu_host_current = &boot;
    lapic_stub_reset();
    host_console_output_clear();
    const long result = system_control(OS1_SYSTEM_REBOOT);
    g_cpu_host_current = saved_current;
    g_cpu_boot = saved_boot;
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, result);
    EXPECT_EQ("system: reboot stage=cpu-quiescence detail=timeout-or-unavailable\n",
              host_console_output());
    ASSERT_EQ(0, mount->operations->create(mount, "after-park", 10, OS1_OBJECT_FILE, opened));
    instance_release(opened);
    disk.fail_flush = true;
    host_console_output_clear();
    EXPECT_EQ(OS1_ERR_IO, system_control(OS1_SYSTEM_SHUTDOWN));
    EXPECT_EQ("system: shutdown stage=storage-sync detail=failed\n", host_console_output());
    host_console_output_clear();
    EXPECT_EQ(OS1_ERR_IO, system_control(OS1_SYSTEM_REBOOT));
    EXPECT_EQ("system: reboot stage=storage-sync detail=failed\n", host_console_output());
    EXPECT_EQ(0, ext2::u16(disk.bytes.data() + start + 1082));
    object_release(mount);
    EXPECT_EQ(0, ext2_system_sync(false));
}
