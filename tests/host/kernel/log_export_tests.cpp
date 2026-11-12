#include <gtest/gtest.h>
#include <os1/input.h>
#include <os1/observe.h>
#include <os1/syscall_numbers.h>
#include <os1/system.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "arch/x86_64/interrupt/interrupt.hpp"
#include "debug/log.hpp"
#include "debug/log_export.hpp"
#include "fs/initrd.hpp"
#include "ipc/channel.hpp"
#include "ipc/kernel_provider.hpp"
#include "log/frame.hpp"
#include "mm/kmem.hpp"
#include "mm/user_copy.hpp"
#include "platform/system_control.hpp"
#include "proc/startup.hpp"
#include "proc/user_program.hpp"
#include "storage/block_device.hpp"
#include "support/initrd_builder.hpp"
#include "support/physical_memory.hpp"
#include "vfs/ext2.hpp"
#include "vfs/filesystem.hpp"
#include "vfs/namespace.hpp"
#ifdef assert
#undef assert
#endif
extern void (*system_transition_wait_hook)();

namespace log_export
{
extern void (*before_remove)(VfsNode*, const char*);
extern void (*after_history_snapshot)();
}  // namespace log_export
namespace
{
using os1::log::Frame;
TEST(LogFrames, BoundsChecksumAndNames)
{
    alignas(Frame) std::array<uint8_t, 512> bytes{};
    auto& h = *reinterpret_cast<Frame*>(bytes.data());
    h = Frame{};
    h.boot = 1;
    h.stream = 1;
    h.record_bytes = 256;
    h.count = 1;
    h.first = h.last = 7;
    h.bytes = 512;
    h.checksum = os1::log::crc(bytes.data(), bytes.size());
    EXPECT_TRUE(os1::log::valid(h, bytes.size()));
    EXPECT_FALSE(os1::log::valid(h, 511));
    bytes[400] = 1;
    EXPECT_FALSE(os1::log::valid(h, 512));
    bytes[400] = 0;
    h.count = UINT32_MAX;
    EXPECT_FALSE(os1::log::header_valid(h));
    for(unsigned stream = 1; stream <= 2; ++stream)
    {
        os1::log::Name name{19, stream == 2 ? 43u : 0u, 77, stream}, read;
        char text[64]{};
        os1::log::filename(name, text);
        ASSERT_TRUE(os1::log::parse_name(text, std::strlen(text), read));
        EXPECT_EQ(name.boot, read.boot);
        EXPECT_EQ(name.session, read.session);
        EXPECT_EQ(name.segment, read.segment);
        text[0] = 'x';
        EXPECT_FALSE(os1::log::parse_name(text, std::strlen(text), read));
    }
}
class LogExport : public testing::Test
{
protected:
    os1::host_test::PhysicalMemoryArena arena{32 * 1024 * 1024};
    PageFrameContainer frames;
    BootInfo boot{};
    BootModuleInfo module{};
    std::vector<uint8_t> disk;
    BlockDevice device{};
    static inline LogExport* active = nullptr;
    static inline const VfsOperations* saved_operations = nullptr;
    Thread* thread = nullptr;
    uint64_t read_bytes = 0, write_bytes = 0, flushes = 0;
    bool fail_flush = false;
    bool check_exclusive_flush = false;
    std::optional<log_export::ForegroundGuard> foreground_to_release;
    unsigned transition_waits = 0;
    int writes = -1;
    static bool submit(BlockDevice& device, BlockRequest& request)
    {
        auto& self = *static_cast<LogExport*>(device.driver_state);
        uint64_t at = request.sector * 512, bytes = request.sector_count * 512;
        if(request.operation == BlockOperation::Read)
            self.read_bytes += bytes;
        if(request.operation == BlockOperation::Write)
            self.write_bytes += bytes;
        if(request.operation == BlockOperation::Flush)
        {
            ++self.flushes;
            if(self.check_exclusive_flush)
            {
                log_export::ForegroundGuard foreground;
                EXPECT_FALSE(foreground.held);
            }
        }
        bool ok = at <= self.disk.size() && bytes <= self.disk.size() - at;
        if(ok && request.operation == BlockOperation::Read)
            std::memcpy(request.buffer, self.disk.data() + at, bytes);
        else if(ok && request.operation == BlockOperation::Write)
        {
            ok = self.writes != 0;
            if(self.writes > 0)
                --self.writes;
            if(ok)
                std::memcpy(self.disk.data() + at, request.buffer, bytes);
        }
        else
            ok = request.operation == BlockOperation::Flush && !self.fail_flush;
        block_request_complete(request,
                               ok ? BlockRequestStatus::Success : BlockRequestStatus::Invalid,
                               ok ? bytes : 0);
        return ok;
    }
    void SetUp() override
    {
        active = this;
        std::array<BootMemoryRegion, 1> regions{{{0, arena.size(), BootMemoryType::Usable, 0}}};
        ASSERT_TRUE(frames.initialize(regions, 0x300000, kPageFrameBitmapQwordLimit));
        kmem_init(frames);
        os1::host_test::InitrdBuilder archive;
        archive.entry(".", {}, 0040755);
        archive.finish();
        auto* data = kernel_physical_pointer<uint8_t>(0x100000);
        std::memcpy(data, archive.bytes.data(), archive.bytes.size());
        module = {0x100000, archive.bytes.size(), "test"};
        boot.modules = &module;
        boot.module_count = 1;
        bind_initrd_boot_info(&boot);
        ASSERT_EQ(0, initialize_initrd_namespace());
        diagnostic_log::test_reset();
        diagnostic_log::initialize(false);
        log_export::initialize();
    }
    void TearDown() override
    {
        system_transition_wait_hook = nullptr;
        foreground_to_release.reset();
        if(thread)
        {
            diagnostic_log::cleanup(thread->process);
            descriptor_close_all(thread->process->descriptors);
        }
        for(unsigned slot = 0; slot < 4; ++slot)
        {
            diagnostic_log::HistoryState h{};
            if(diagnostic_log::history_state(slot, h))
                diagnostic_log::retire_history(slot, h.session);
        }
        log_export::test_reset();
        diagnostic_log::test_reset();
        reset_initrd_namespace_for_test();
        bind_initrd_boot_info(nullptr);
    }
    void mount(const char* fixture = OS1_EXT2_FIXTURE, bool read_only = false)
    {
        std::ifstream file(fixture, std::ios::binary);
        disk.assign(std::istreambuf_iterator<char>(file), {});
        device = {"log-test", disk.size() / 512, 512, 8, 1, this, submit, submit};
        device.read_only = read_only;
        ASSERT_EQ(0, ext2_mount(initrd_namespace_root(), device));
    }
    static void open_during_snapshot()
    {
        active->shell();
    }
    void shell()
    {
        ASSERT_TRUE(init_tasks(frames));
        VirtualMemory vm(frames);
        ASSERT_TRUE(vm.allocate_and_map(kUserImageBase, 1, PageFlags::User | PageFlags::Write));
        auto* process = create_user_process("log-export", vm.root());
        ASSERT_NE(nullptr, process);
        ASSERT_EQ(0, inherit_process_namespace(nullptr, process));
        thread = create_user_thread(process, kUserImageBase, kUserImageBase + 4096, frames, false);
        ASSERT_NE(nullptr, thread);
        ASSERT_EQ(0, diagnostic_log::context(frames, thread, OS1_LOG_CONTEXT_OPEN, 0));
    }
    void command(os1_log_command& c, unsigned operation)
    {
        c.version = 1;
        c.size = sizeof(c);
        ASSERT_TRUE(copy_to_user(frames, thread, kUserImageBase, &c, sizeof(c)));
        ASSERT_EQ(0, diagnostic_log::context(frames, thread, operation, kUserImageBase));
        ASSERT_TRUE(copy_from_user(frames, thread, kUserImageBase, &c, sizeof(c)));
    }
    NativeObject* open(const char* path)
    {
        NativeObject* object = nullptr;
        EXPECT_EQ(0, vfs_resolve(initrd_namespace_root(), path + 1, std::strlen(path) - 1, object));
        return object;
    }
    std::vector<uint8_t> read(const char* path)
    {
        auto* object = open(path);
        if(!object)
            return {};
        auto* file = static_cast<VfsNode*>(object);
        os1_metadata m{};
        {
            NamespaceGuard guard;
            EXPECT_EQ(0, vfs_metadata(file, m));
        }
        std::vector<uint8_t> bytes(m.logical_bytes);
        EXPECT_EQ(static_cast<long>(bytes.size()),
                  file->operations->read(file, 0, bytes.data(), bytes.size()));
        object_release(object);
        return bytes;
    }
    void events(unsigned n)
    {
        os1_log_record r{};
        r.level = OS1_LOG_INFO_LEVEL;
        r.kind = OS1_LOG_EVENT;
        for(unsigned i = 0; i < n; ++i)
            diagnostic_log::append(r);
    }
    template<class T>
    long ipc(unsigned action, int fd, T& value)
    {
        EXPECT_TRUE(copy_to_user(frames, thread, kUserImageBase, &value, sizeof(value)));
        long result = sys_ipc(frames, thread, action, fd, kUserImageBase);
        EXPECT_TRUE(copy_from_user(frames, thread, kUserImageBase, &value, sizeof(value)));
        return result;
    }
    int log_client()
    {
        auto& table = thread->process->descriptors;
        initrd_namespace_root()->allowed_rights |= OS1_RIGHT_LOG_READ | OS1_RIGHT_SYSTEM_CONTROL;
        InstanceRef source(open_instance(initrd_namespace_root()));
        int root = descriptor_install(
            table, source.get(), OS1_RIGHT_LOG_READ | OS1_RIGHT_SYSTEM_CONTROL, 0);
        EXPECT_GE(root, 0);
        os1_ipc_kernel_binding binding{1,
                                       sizeof(binding),
                                       OS1_LOG_INTERFACE_ID,
                                       1,
                                       OS1_RIGHT_LOG_READ | OS1_RIGHT_SYSTEM_CONTROL};
        int client = ipc(OS1_IPC_BIND_KERNEL, root, binding);
        descriptor_close(table, root);
        return client;
    }
    int export_operation(int client, unsigned target = OS1_LOG_EXPORT_TMP)
    {
        os1_ipc_message request{};
        request.version = 1;
        request.size = sizeof(request);
        request.interface_id = OS1_LOG_INTERFACE_ID;
        request.interface_version = 1;
        request.member = OS1_LOG_EXPORT;
        os1_log_export_request args{1, sizeof(args), target, 0};
        request.payload_bytes = sizeof(args);
        std::memcpy(request.payload, &args, sizeof(args));
        return ipc(OS1_IPC_SUBMIT, client, request);
    }
    void verify(const std::vector<uint8_t>& bytes)
    {
        ASSERT_GE(bytes.size(), sizeof(Frame));
        size_t at = 0;
        while(at < bytes.size())
        {
            ASSERT_GE(bytes.size() - at, sizeof(Frame));
            auto& h = *reinterpret_cast<const Frame*>(bytes.data() + at);
            ASSERT_TRUE(os1::log::valid(h, bytes.size() - at));
            at += h.bytes;
        }
        EXPECT_EQ(at, bytes.size());
    }
};
TEST_F(LogExport, SubjectlessTrapClearsCommandAndProcessProvenance)
{
    Process context{};
    context.pid = 10;
    context.log_session = 11;
    context.log_command = 12;
    context.log_parent_command = 13;

    diagnostic_log::event(OS1_KERNEL_EVENT_TRAP, 0, 0, T_NMI, 0, &context);
    os1_log_query query{};
    query.version = 1;
    query.size = sizeof(query);
    query.kind = OS1_LOG_EVENT;
    os1_log_record record{};
    ASSERT_EQ(diagnostic_log::read(query, record), 1);
    EXPECT_EQ(record.arg0, T_NMI);
    EXPECT_EQ(record.pid, 0u);
    EXPECT_EQ(record.session_id, 0u);
    EXPECT_EQ(record.command_id, 0u);
    EXPECT_EQ(record.parent_command_id, 0u);

    diagnostic_log::event(
        OS1_KERNEL_EVENT_TRAP, OS1_KERNEL_EVENT_FLAG_USER, context.pid, T_PGFLT, 0, &context);
    query.after = record.sequence;
    ASSERT_EQ(diagnostic_log::read(query, record), 1);
    EXPECT_EQ(record.arg0, T_PGFLT);
    EXPECT_EQ(record.pid, context.pid);
    EXPECT_EQ(record.session_id, context.log_session);
    EXPECT_EQ(record.command_id, context.log_command);
    EXPECT_EQ(record.parent_command_id, context.log_parent_command);
}

// The original CI interleavings are forced here; no wall-clock race is required.
TEST_F(LogExport, ExportDoesNotInvalidateUnrelatedDirectoryWalk)
{
    mount();
    auto* directory = static_cast<VfsNode*>(open("/tmp"));
    ASSERT_NE(nullptr, directory);
    DescriptorTable table{};
    InstanceRef instance(open_instance(directory));
    object_release(directory);
    int fd =
        descriptor_install(table, instance.get(), kTmpDirectoryRights, 0, -1, kNamespaceCeiling);
    ASSERT_GE(fd, 0);
    os1_tree_step step{};
    step.version = OS1_TREE_SCOPED_VERSION;
    step.size = sizeof(step);
    step.action = OS1_TREE_CHECK;
    step.excluded_fd = -1;
    ASSERT_EQ(0, vfs_tree_step(table, fd, step));
    os1_tree_step legacy = step;
    legacy.version = 1;
    legacy.generation = 0;
    ASSERT_EQ(0, vfs_tree_step(table, fd, legacy));
    diagnostic_log::tick(2'000'000'000);
    log_export::poll();  // Creates /disk/log and the first persistent segment.
    ASSERT_GT(log_export::info().disk_bytes, 0u);
    step.action = OS1_TREE_NEXT;
    EXPECT_EQ(0, vfs_tree_step(table, fd, step));
    legacy.action = OS1_TREE_NEXT;
    EXPECT_EQ(OS1_ERR_CHANGED, vfs_tree_step(table, fd, legacy));
    descriptor_close_all(table);
}
TEST_F(LogExport, ScopedWalkRejectsLogEntryChangesButNotSiblingExport)
{
    mount();
    DescriptorTable table{};
    descriptor_table_initialize(table);
    auto install = [&](VfsNode* node) -> int {
        InstanceRef instance(open_instance(node));
        return descriptor_install(
            table, instance.get(), kTmpDirectoryRights, 0, -1, kNamespaceCeiling);
    };
    auto snapshot = [&](int fd) {
        os1_tree_step step{};
        step.version = OS1_TREE_SCOPED_VERSION;
        step.size = sizeof(step);
        step.action = OS1_TREE_CHECK;
        step.excluded_fd = -1;
        EXPECT_EQ(0, vfs_tree_step(table, fd, step));
        return step;
    };
    for(const char* path : {"/tmp", "/disk"})
    {
        auto* base = static_cast<VfsNode*>(open(path));
        ASSERT_NE(nullptr, base);
        OpenInstance* made = nullptr;
        ASSERT_EQ(0, base->operations->create(base, "work", 4, OS1_OBJECT_DIRECTORY, made));
        InstanceRef work(made);
        int fd = install(static_cast<VfsNode*>(made->object));
        ASSERT_GE(fd, 0);
        auto before = snapshot(fd);
        long status;
        ASSERT_TRUE(
            log_export::try_run(path[1] == 't' ? OS1_LOG_EXPORT_TMP : OS1_LOG_EXPORT_DISK, status));
        ASSERT_EQ(0, status);
        before.action = OS1_TREE_NEXT;
        EXPECT_EQ(0, vfs_tree_step(table, fd, before));
        object_release(base);
    }
    auto* logs = static_cast<VfsNode*>(open("/tmp/log"));
    ASSERT_NE(nullptr, logs);
    int fd = install(logs);
    object_release(logs);
    auto before = snapshot(fd);
    shell();  // Adds a new shell snapshot file; rewriting kernel.bin only changes contents.
    ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_TMP));
    before.action = OS1_TREE_NEXT;
    EXPECT_EQ(OS1_ERR_CHANGED, vfs_tree_step(table, fd, before));
    descriptor_close_all(table);
}
TEST_F(LogExport, ScopedMoveInvalidatesBothParentsAndMovedObjectOnBothBackends)
{
    mount();
    DescriptorTable table{};
    descriptor_table_initialize(table);
    auto install = [&](VfsNode* node) -> int {
        InstanceRef instance(open_instance(node));
        return descriptor_install(table, instance.get(), OS1_RIGHT_INSPECT, 0);
    };
    auto snapshot = [&](int fd) {
        os1_tree_step step{};
        step.version = OS1_TREE_SCOPED_VERSION;
        step.size = sizeof(step);
        step.action = OS1_TREE_CHECK;
        step.excluded_fd = -1;
        EXPECT_EQ(0, vfs_tree_step(table, fd, step));
        return step;
    };
    for(const char* path : {"/tmp", "/disk"})
    {
        auto* base = static_cast<VfsNode*>(open(path));
        ASSERT_NE(nullptr, base);
        OpenInstance *a = nullptr, *b = nullptr, *c = nullptr;
        ASSERT_EQ(0, base->operations->create(base, "left", 4, OS1_OBJECT_DIRECTORY, a));
        ASSERT_EQ(0, base->operations->create(base, "right", 5, OS1_OBJECT_DIRECTORY, b));
        InstanceRef left(a), right(b);
        auto* left_node = static_cast<VfsNode*>(a->object);
        auto* right_node = static_cast<VfsNode*>(b->object);
        ASSERT_EQ(0, left_node->operations->create(left_node, "item", 4, OS1_OBJECT_FILE, c));
        InstanceRef child(c);
        int fds[] = {
            install(left_node), install(right_node), install(static_cast<VfsNode*>(c->object))};
        auto x = snapshot(fds[0]), y = snapshot(fds[1]), z = snapshot(fds[2]);
        ASSERT_EQ(
            0, left_node->operations->move(left_node, "item", 4, right_node, "moved", 5, false, 0));
        EXPECT_EQ(OS1_ERR_CHANGED, vfs_tree_step(table, fds[0], x));
        EXPECT_EQ(OS1_ERR_CHANGED, vfs_tree_step(table, fds[1], y));
        EXPECT_EQ(OS1_ERR_CHANGED, vfs_tree_step(table, fds[2], z));
        y = snapshot(fds[1]);
        z = snapshot(fds[2]);
        ASSERT_EQ(0, right_node->operations->rename(right_node, "moved", 5, "renamed", 7));
        EXPECT_EQ(OS1_ERR_CHANGED, vfs_tree_step(table, fds[1], y));
        EXPECT_EQ(OS1_ERR_CHANGED, vfs_tree_step(table, fds[2], z));
        object_release(base);
    }
    descriptor_close_all(table);
}
TEST_F(LogExport, ExplicitOperationWaitsForForegroundAdmission)
{
    shell();
    auto& table = thread->process->descriptors;
    int client = log_client();
    ASSERT_GE(client, 0);
    int operation = export_operation(client);
    ASSERT_GE(operation, 0);
    {
        log_export::ForegroundGuard foreground;
        ASSERT_TRUE(foreground.held);
        EXPECT_FALSE(ipc_run_kernel_request());
        os1_ipc_result result{};
        EXPECT_EQ(OS1_ERR_WOULD_BLOCK, ipc(OS1_IPC_RESULT, operation, result));
        EXPECT_EQ(0u, log_export::info().tmp_bytes);
        EXPECT_FALSE(ipc_run_kernel_request());  // The deferred request cannot busy-spin.
        os1_ipc_message status_request{};
        status_request.version = 1;
        status_request.size = sizeof(status_request);
        status_request.interface_id = OS1_LOG_INTERFACE_ID;
        status_request.interface_version = 1;
        status_request.member = OS1_LOG_EXPORT_INFO;
        int status_op = ipc(OS1_IPC_SUBMIT, client, status_request);
        ASSERT_GE(status_op, 0);
        EXPECT_TRUE(
            ipc_run_kernel_request());  // An unrelated ready request passes the deferred one.
        EXPECT_EQ(0, ipc(OS1_IPC_RESULT, status_op, result));
        EXPECT_EQ(0, result.application_status);
        descriptor_close(table, status_op);
    }
    ipc_timer_tick(1'000'000);
    EXPECT_TRUE(ipc_run_kernel_request());
    os1_ipc_result result{};
    EXPECT_EQ(0, ipc(OS1_IPC_RESULT, operation, result));
    EXPECT_EQ(0, result.application_status);
    EXPECT_GT(log_export::info().tmp_bytes, 0u);
    descriptor_close_all(table);
}
TEST_F(LogExport, DeferredOperationRetainsSourceAfterCallerClosesDescriptors)
{
    shell();
    auto& table = thread->process->descriptors;
    auto before = initrd_namespace_root()->references;
    int client = log_client();
    ASSERT_GE(client, 0);
    int operation = export_operation(client);
    ASSERT_GE(operation, 0);
    {
        log_export::ForegroundGuard foreground;
        ASSERT_TRUE(foreground.held);
        EXPECT_FALSE(ipc_run_kernel_request());
        EXPECT_EQ(0, descriptor_close(table, client));
        EXPECT_EQ(0, descriptor_close(table, operation));
        EXPECT_GT(initrd_namespace_root()->references, before);
        thread->user_mode = false;
        ASSERT_TRUE(ipc_wait_kernel_work(thread));
        EXPECT_EQ(ThreadWaitReason::Ipc, thread->wait.reason);
        cancel_ipc_wait(thread);
        thread->user_mode = true;
    }
    ipc_timer_tick(1'000'000);
    EXPECT_TRUE(ipc_run_kernel_request());
    EXPECT_GT(log_export::info().tmp_bytes, 0u);
    EXPECT_EQ(initrd_namespace_root()->references, before);
    EXPECT_FALSE(ipc_run_kernel_request());
    descriptor_close_all(table);
}
TEST_F(LogExport, AdmittedStorageErrorIsCompletedWithoutReplay)
{
    shell();
    int client = log_client();
    ASSERT_GE(client, 0);
    int operation = export_operation(client);
    ASSERT_GE(operation, 0);
    {
        NamespaceGuard namespace_guard;
        ASSERT_TRUE(namespace_guard.held);
        EXPECT_TRUE(ipc_run_kernel_request());
    }
    os1_ipc_result result{};
    EXPECT_EQ(0, ipc(OS1_IPC_RESULT, operation, result));
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, result.application_status);
    EXPECT_EQ(1u, log_export::info().failures);
    ipc_timer_tick(1'000'000);
    EXPECT_FALSE(ipc_run_kernel_request());
    EXPECT_EQ(0u, log_export::info().tmp_bytes);
    descriptor_close_all(thread->process->descriptors);
}
TEST_F(LogExport, InvalidExportCompletesEvenWhenAdmissionIsBusy)
{
    shell();
    int client = log_client();
    ASSERT_GE(client, 0);
    int operation = export_operation(client, 999);
    ASSERT_GE(operation, 0);
    log_export::ForegroundGuard foreground;
    ASSERT_TRUE(foreground.held);
    EXPECT_TRUE(ipc_run_kernel_request());
    os1_ipc_result result{};
    EXPECT_EQ(0, ipc(OS1_IPC_RESULT, operation, result));
    EXPECT_EQ(OS1_ERR_INVALID, result.application_status);
    descriptor_close_all(thread->process->descriptors);
}
TEST_F(LogExport, TemporarySnapshotIsBoundedAndMissingDiskDoesNotStopCapture)
{
    events(1500);
    ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_TMP));
    auto bytes = read("/tmp/log/kernel.bin");
    EXPECT_LE(bytes.size(), 32768u);
    verify(bytes);
    for(unsigned i = 0; i < 3; ++i)
    {
        diagnostic_log::tick(2'000'000'000);
        log_export::poll();
    }
    EXPECT_EQ(1u, log_export::info().suspended);
    auto before = diagnostic_log::info().latest;
    events(1);
    EXPECT_EQ(before + 1, diagnostic_log::info().latest);
    ASSERT_EQ(
        0, log_export::run(OS1_LOG_EXPORT_TMP));  // Volatile export does not unsuspend disk export.
    EXPECT_EQ(1u, log_export::info().suspended);
}
TEST_F(LogExport, ExistingForeignSnapshotIsNeverTruncated)
{
    auto* object = open("/tmp");
    auto* dir = static_cast<VfsNode*>(object);
    OpenInstance* logs = nullptr;
    ASSERT_EQ(0, dir->operations->create(dir, "log", 3, OS1_OBJECT_DIRECTORY, logs));
    auto* node = static_cast<VfsNode*>(logs->object);
    OpenInstance* file = nullptr;
    ASSERT_EQ(0, node->operations->create(node, "kernel.bin", 10, OS1_OBJECT_FILE, file));
    auto* f = static_cast<VfsNode*>(file->object);
    ASSERT_EQ(4, f->operations->write(f, 0, "user", 4, false, nullptr));
    instance_release(file);
    instance_release(logs);
    object_release(object);
    EXPECT_EQ(OS1_ERR_EXISTS, log_export::run(OS1_LOG_EXPORT_TMP));
    auto bytes = read("/tmp/log/kernel.bin");
    EXPECT_EQ(std::string(bytes.begin(), bytes.end()), "user");
}
TEST_F(LogExport, DiskFramesSurviveRemountAndBootNumbersIncrease)
{
    mount();
    events(100);
    ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_DISK));
    uint64_t previous = log_export::info().boot_id;
    ASSERT_EQ(1u, previous);
    char name[64]{};
    os1::log::filename({previous, 0, 1, 1}, name);
    auto bytes = read((std::string("/disk/log/") + name).c_str());
    verify(bytes);
    log_export::test_reset();
    reset_initrd_namespace_for_test();
    ASSERT_EQ(0, initialize_initrd_namespace());
    ASSERT_EQ(0, ext2_mount(initrd_namespace_root(), device));
    log_export::initialize();
    ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_DISK));
    EXPECT_EQ(previous + 1, log_export::info().boot_id);
    EXPECT_EQ(bytes, read((std::string("/disk/log/") + name).c_str()));
}
TEST_F(LogExport, ShutdownReportsBusyBeforeStorageSyncWhenAdmissionStaysHeld)
{
    mount();
    events(10);
    const auto before = flushes;
    system_transition_wait_hook = [] {
        auto& test =
            static_cast<LogExport_ShutdownReportsBusyBeforeStorageSyncWhenAdmissionStaysHeld_Test&>(
                *active);
        ++test.transition_waits;
    };
    {
        log_export::ForegroundGuard foreground;
        ASSERT_TRUE(foreground.held);
        EXPECT_EQ(OS1_ERR_WOULD_BLOCK, system_control(OS1_SYSTEM_SHUTDOWN));
        EXPECT_EQ(before, flushes);
        EXPECT_EQ(250u, transition_waits);
    }
    EXPECT_EQ(0, system_control(OS1_SYSTEM_SYNC));
}

TEST_F(LogExport, ShutdownDrainsAfterAdmissionClearsAndKeepsItThroughStorageFreeze)
{
    mount();
    events(10);
    foreground_to_release.emplace();
    ASSERT_TRUE(foreground_to_release->held);
    system_transition_wait_hook = [] {
        auto& test = static_cast<
            LogExport_ShutdownDrainsAfterAdmissionClearsAndKeepsItThroughStorageFreeze_Test&>(
            *active);
        if(++test.transition_waits == 3)
            test.foreground_to_release.reset();
        diagnostic_log::tick(1'000'000);
    };
    check_exclusive_flush = true;
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, system_control(OS1_SYSTEM_SHUTDOWN));
    check_exclusive_flush = false;
    EXPECT_EQ(3u, transition_waits);
    EXPECT_GT(log_export::info().kernel_cursor, 0u);
    EXPECT_GT(flushes, 0u);
    log_export::ForegroundGuard foreground;
    EXPECT_TRUE(foreground.held);
    EXPECT_EQ(0, system_control(OS1_SYSTEM_SYNC));
}

TEST_F(LogExport, RebootWaitsForActiveExportBeforeStorageSync)
{
    mount();
    events(10);
    system_transition_wait_hook = [] {
        auto& test =
            static_cast<LogExport_RebootWaitsForActiveExportBeforeStorageSync_Test&>(*active);
        ++test.transition_waits;
        diagnostic_log::tick(251'000'000);
    };
    log_export::after_history_snapshot = [] {
        auto& test =
            static_cast<LogExport_RebootWaitsForActiveExportBeforeStorageSync_Test&>(*active);
        const auto before = test.flushes;
        EXPECT_EQ(OS1_ERR_WOULD_BLOCK, system_control(OS1_SYSTEM_REBOOT));
        EXPECT_EQ(before, test.flushes);
    };
    EXPECT_EQ(0, log_export::run(OS1_LOG_EXPORT_TMP));
    EXPECT_EQ(1u, transition_waits);
    EXPECT_EQ(0, system_control(OS1_SYSTEM_SYNC));
}

TEST_F(LogExport, AutomaticExportSkipsReadOnlyMount)
{
    mount(OS1_EXT2_FIXTURE, true);
    events(100);
    diagnostic_log::tick(2'000'000'000);
    log_export::poll();
    {
        log_export::FinalDrainGuard drain;
        ASSERT_TRUE(drain.try_acquire());
        drain.drain();
    }
    EXPECT_EQ(0u, log_export::info().failures);
    EXPECT_EQ(0u, write_bytes);
    EXPECT_EQ(0u, flushes);
    EXPECT_EQ(OS1_ERR_ACCESS, log_export::run(OS1_LOG_EXPORT_DISK));
    EXPECT_EQ(0u, write_bytes);
    EXPECT_EQ(0u, flushes);
}
TEST_F(LogExport, FailedFlushSuspendsAndNeedsHealthyMount)
{
    mount();
    events(10);
    fail_flush = true;
    for(unsigned i = 0; i < 3; ++i)
    {
        diagnostic_log::tick(2'000'000'000);
        log_export::poll();
    }
    EXPECT_EQ(1u, log_export::info().suspended);
    EXPECT_GE(log_export::info().failures, 3u);
    fail_flush = false;
    EXPECT_EQ(
        OS1_ERR_IO,
        log_export::run(OS1_LOG_EXPORT_DISK));  // A retired filesystem cannot recover in place.
    reset_initrd_namespace_for_test();
    ASSERT_EQ(0, initialize_initrd_namespace());
    mount();
    ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_DISK));
    EXPECT_EQ(0u, log_export::info().suspended);
}
TEST_F(LogExport, RotationRetainsFourBootsAndForeignFiles)
{
    mount();
    events(20);
    for(unsigned boot = 0; boot < 6; ++boot)
    {
        ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_DISK));
        log_export::test_reset();
        log_export::initialize();
    }
    auto* object = open("/disk/log");
    auto* dir = static_cast<VfsNode*>(object);
    uint64_t cursor = 0, next = 0;
    std::set<uint64_t> boots;
    unsigned count = 0;
    for(;;)
    {
        os1_directory_entry e{};
        long r = dir->operations->next(dir, cursor, e, next);
        ASSERT_GE(r, 0);
        if(!r)
            break;
        cursor = next;
        os1::log::Name n;
        ASSERT_TRUE(os1::log::parse_name(e.name, e.name_bytes, n));
        boots.insert(n.boot);
        ++count;
    }
    EXPECT_LE(boots.size(), 4u);
    EXPECT_LE(count, 16u);
    object_release(object);
}
TEST_F(LogExport, AutomaticBudgetAndExplicitBootIdentityAreStable)
{
    mount();
    events(1024);
    diagnostic_log::tick(250'000'000);
    log_export::poll();
    auto first = log_export::info();
    ASSERT_GT(first.disk_bytes, 0u);
    EXPECT_LE(first.disk_bytes, 65536u);
    diagnostic_log::tick(250'000'000);
    log_export::poll();
    EXPECT_LE(log_export::info().disk_bytes, 65536u);
    ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_DISK));
    auto identity = log_export::info().boot_id;
    diagnostic_log::tick(2'000'000'000);
    log_export::poll();
    EXPECT_EQ(identity, log_export::info().boot_id);
    EXPECT_GE(log_export::info().kernel_cursor, 1024u);
}
TEST_F(LogExport, PendingHistoryCompletionAndClosingLifetime)
{
    mount();
    shell();
    os1_log_command c{};
    std::strcpy(c.text, "visible command");
    command(c, OS1_LOG_COMMAND_BEGIN);
    ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_DISK));
    command(c, OS1_LOG_COMMAND_END);
    c = {};
    c.flags = OS1_LOG_SUPPRESSED;
    std::strcpy(c.text, "secret command");
    command(c, OS1_LOG_COMMAND_BEGIN);
    command(c, OS1_LOG_COMMAND_END);
    diagnostic_log::cleanup(thread->process);
    diagnostic_log::HistoryState h{};
    ASSERT_TRUE(diagnostic_log::history_state(0, h));
    ASSERT_TRUE(h.closing);
    ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_DISK));
    EXPECT_FALSE(diagnostic_log::history_state(0, h));
    EXPECT_EQ(0u, log_export::info().closing_lost);
    char name[64]{};
    os1::log::filename({1, thread->process->pid, 2, 2}, name);
    auto bytes = read((std::string("/disk/log/") + name).c_str());
    verify(bytes);
    unsigned pending = 0, completed = 0, suppressed = 0;
    for(size_t at = 0; at < bytes.size();)
    {
        auto& f = *reinterpret_cast<const Frame*>(bytes.data() + at);
        for(unsigned i = 0; i < f.count; ++i)
        {
            auto& row = *reinterpret_cast<const os1_log_history_record*>(bytes.data() + at + 256 +
                                                                         i * 2048);
            if(row.record.flags & OS1_LOG_SUPPRESSED)
            {
                ++suppressed;
                EXPECT_EQ(0, row.text[0]);
            }
            else if(row.record.flags & OS1_LOG_PENDING)
                ++pending;
            else
                ++completed;
        }
        at += f.bytes;
    }
    EXPECT_EQ(1u, pending);
    EXPECT_EQ(1u, completed);
    EXPECT_EQ(1u, suppressed);
}
TEST_F(LogExport, ShellThresholdDrainsBeforePeriodicDeadlineAndMissingDiskReleasesClosedHistory)
{
    mount();
    shell();
    for(unsigned i = 0; i < 16; ++i)
    {
        os1_log_command c{};
        command(c, OS1_LOG_COMMAND_BEGIN);
        command(c, OS1_LOG_COMMAND_END);
    }
    diagnostic_log::tick(250'000'000);
    log_export::poll();
    EXPECT_GT(log_export::info().disk_bytes, 32768u);
    reset_initrd_namespace_for_test();
    ASSERT_EQ(0, initialize_initrd_namespace());
    os1_log_command c{};
    command(c, OS1_LOG_COMMAND_BEGIN);
    command(c, OS1_LOG_COMMAND_END);
    diagnostic_log::cleanup(thread->process);
    diagnostic_log::tick(250'000'000);
    log_export::poll();
    diagnostic_log::HistoryState h{};
    EXPECT_FALSE(diagnostic_log::history_state(0, h));
    EXPECT_EQ(1u, log_export::info().closing_lost);
}
TEST_F(LogExport, FreeSpaceReserveStopsExportBeforeCreatingDirectory)
{
    mount();
    auto* root = open("/disk");
    auto* dir = static_cast<VfsNode*>(root);
    OpenInstance* output = nullptr;
    ASSERT_EQ(0, dir->operations->create(dir, "fill", 4, OS1_OBJECT_FILE, output));
    auto* file = static_cast<VfsNode*>(output->object);
    std::array<uint8_t, 256> zero{};
    for(unsigned at = 0; at < 2 * 1024 * 1024; at += 256)
        ASSERT_EQ(256, file->operations->write(file, at, zero.data(), 256, false, nullptr));
    instance_release(output);
    events(10);
    EXPECT_EQ(OS1_ERR_LIMIT, log_export::run(OS1_LOG_EXPORT_DISK));
    NativeObject* logs = nullptr;
    EXPECT_EQ(OS1_ERR_NOT_FOUND, dir->operations->lookup(dir, "log", 3, logs));
    auto before = diagnostic_log::info().latest;
    events(1);
    EXPECT_EQ(before + 1, diagnostic_log::info().latest);
    object_release(root);
}
TEST_F(LogExport, ConcurrentHistoryRegistrationUsesFrozenSnapshotBudget)
{
    log_export::after_history_snapshot = open_during_snapshot;
    ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_TMP));
    EXPECT_LE(log_export::info().tmp_bytes, 65536u);
    log_export::after_history_snapshot = nullptr;
    os1_log_command c{};
    command(c, OS1_LOG_COMMAND_BEGIN);
    command(c, OS1_LOG_COMMAND_END);
    ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_TMP));
    char path[64] = "/tmp/log/shell-";
    char* p = path + 15;
    os1::log::hex(p, thread->process->pid);
    std::memcpy(p, ".bin", 5);
    verify(read(path));
}
TEST_F(LogExport, RetentionRevalidatesReplacementBeforeRemoval)
{
    mount();
    events(1);
    for(unsigned i = 0; i < 4; ++i)
    {
        ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_DISK));
        log_export::test_reset();
        log_export::initialize();
    }
    log_export::before_remove = [](VfsNode* dir, const char* name) {
        log_export::before_remove = nullptr;
        ASSERT_EQ(0, dir->operations->rename(dir, name, std::strlen(name), "saved", 5));
        OpenInstance* file = nullptr;
        ASSERT_EQ(0, dir->operations->create(dir, name, std::strlen(name), OS1_OBJECT_FILE, file));
        auto* node = static_cast<VfsNode*>(file->object);
        ASSERT_EQ(4, node->operations->write(node, 0, "user", 4, false, nullptr));
        instance_release(file);
    };
    EXPECT_EQ(OS1_ERR_EXISTS, log_export::run(OS1_LOG_EXPORT_DISK));
    char name[64]{};
    os1::log::filename({1, 0, 1, 1}, name);
    auto data = read((std::string("/disk/log/") + name).c_str());
    EXPECT_EQ("user", std::string(data.begin(), data.end()));
}
TEST_F(LogExport, TemporaryCleanupRevalidatesReplacement)
{
    shell();
    ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_TMP));
    log_export::before_remove = [](VfsNode* dir, const char* name) {
        log_export::before_remove = nullptr;
        ASSERT_EQ(0, dir->operations->rename(dir, name, std::strlen(name), "saved", 5));
        OpenInstance* file = nullptr;
        ASSERT_EQ(0, dir->operations->create(dir, name, std::strlen(name), OS1_OBJECT_FILE, file));
        auto* node = static_cast<VfsNode*>(file->object);
        ASSERT_EQ(4, node->operations->write(node, 0, "user", 4, false, nullptr));
        instance_release(file);
    };
    EXPECT_EQ(OS1_ERR_EXISTS, log_export::run(OS1_LOG_EXPORT_TMP));
}
TEST_F(LogExport, HeaderReadFailureIsNotForeignFile)
{
    mount();
    events(1);
    ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_DISK));
    char name[64]{};
    os1::log::filename({1, 0, 1, 1}, name);
    auto* object = open((std::string("/disk/log/") + name).c_str());
    auto* node = static_cast<VfsNode*>(object);
    auto* original = node->operations;
    VfsOperations fail = *original;
    fail.read = [](VfsNode*, uint64_t, void*, size_t) -> long { return OS1_ERR_WOULD_BLOCK; };
    node->operations = &fail;
    events(1);
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, log_export::run(OS1_LOG_EXPORT_DISK));
    node->operations = original;
    object_release(object);
    ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_DISK));
}
TEST_F(LogExport, IndirectAllocationMaintainsFreeSpaceReserve)
{
    mount();
    events(1);
    ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_DISK));
    auto* dir_object = open("/disk/log");
    auto* dir = static_cast<VfsNode*>(dir_object);
    // Inject space reporting at the VFS contract: a 10% reserve need not align to a block.
    auto* original = dir->operations;
    VfsOperations limited = *original;
    limited.space = [](VfsNode*, os1_space& out) -> long {
        out = {};
        out.total_bytes = 30 * 1024 * 1024 + 17;
        out.free_bytes = out.total_bytes / 10 + 6000;
        return 0;
    };
    dir->operations = &limited;
    char name[64]{};
    os1::log::filename({1, 0, 1, 1}, name);
    auto* file_object = open((std::string("/disk/log/") + name).c_str());
    auto* file = static_cast<VfsNode*>(file_object);
    ASSERT_EQ(0, file->operations->truncate(file, 49152));
    object_release(file_object);
    events(1);
    EXPECT_EQ(OS1_ERR_LIMIT, log_export::run(OS1_LOG_EXPORT_DISK));
    dir->operations = original;
    object_release(dir_object);
}
TEST_F(LogExport, ShortAppendRetainsPrefixAndUsesNewSegment)
{
    mount();
    events(3);
    ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_DISK));
    char name[64]{};
    os1::log::filename({1, 0, 1, 1}, name);
    std::string path = std::string("/disk/log/") + name;
    auto prefix = read(path.c_str());
    auto* object = open(path.c_str());
    auto* node = static_cast<VfsNode*>(object);
    saved_operations = node->operations;
    VfsOperations partial = *saved_operations;
    partial.write =
        [](VfsNode* n, uint64_t offset, const void* bytes, size_t size, bool append, uint64_t* end)
        -> long {
        const size_t prefix = size > 128 ? 128 : size / 2;
        return saved_operations->write(n, offset, bytes, prefix, append, end);
    };
    node->operations = &partial;
    events(3);
    EXPECT_EQ(OS1_ERR_IO, log_export::run(OS1_LOG_EXPORT_DISK));
    node->operations = saved_operations;
    object_release(object);
    auto torn = read(path.c_str());
    ASSERT_EQ(prefix.size() + 128, torn.size());
    EXPECT_TRUE(std::equal(prefix.begin(), prefix.end(), torn.begin()));
    ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_DISK));
    EXPECT_EQ(torn, read(path.c_str()));
    os1::log::filename({1, 0, 2, 1}, name);
    verify(read((std::string("/disk/log/") + name).c_str()));
}
TEST_F(LogExport, ByteAndSegmentBudgetsHoldUnderSustainedTraffic)
{
    mount(OS1_LOG_EXT2_FIXTURE);
    shell();
    read_bytes = write_bytes = flushes = 0;
    for(unsigned iteration = 0; iteration < 24; ++iteration)
    {
        events(1024);
        for(unsigned i = 0; i < 32; ++i)
        {
            os1_log_command c{};
            command(c, OS1_LOG_COMMAND_BEGIN);
            command(c, OS1_LOG_COMMAND_END);
        }
        ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_DISK));
    }
    const auto measured_reads = read_bytes;
    auto* object = open("/disk/log");
    auto* dir = static_cast<VfsNode*>(object);
    unsigned count = 0;
    uint64_t cursor = 0, next = 0, total[2]{};
    for(;;)
    {
        os1_directory_entry entry{};
        long r = dir->operations->next(dir, cursor, entry, next);
        ASSERT_GE(r, 0);
        if(!r)
            break;
        cursor = next;
        ++count;
        os1::log::Name n{};
        ASSERT_TRUE(os1::log::parse_name(entry.name, entry.name_bytes, n));
        auto data = read((std::string("/disk/log/") + entry.name).c_str());
        verify(data);
        EXPECT_LE(data.size(), 262144u);
        total[n.stream - 1] += data.size();
    }
    EXPECT_LE(count, 16u);
    EXPECT_LE(total[0], 3 * 1024 * 1024u);
    EXPECT_LE(total[1], 1024 * 1024u);
    EXPECT_GT(log_export::info().disk_bytes, 4 * 1024 * 1024u);
    std::printf(
        "export_io framed_bytes=%llu read_bytes=%llu write_bytes=%llu flushes=%llu "
        "retained_bytes=%llu\n",
        (unsigned long long)log_export::info().disk_bytes,
        (unsigned long long)measured_reads,
        (unsigned long long)write_bytes,
        (unsigned long long)flushes,
        (unsigned long long)(total[0] + total[1]));
    object_release(object);
}
TEST_F(LogExport, SynchronizationDeadlineDoesNotWaitForNextDrain)
{
    mount();
    events(1);
    diagnostic_log::tick(2'000'000'000);
    log_export::poll();
    ASSERT_EQ(2'000'000'000u, log_export::info().last_sync_ns);
    events(1);
    diagnostic_log::tick(2'000'000'000);
    log_export::poll();
    diagnostic_log::tick(2'000'000'000);
    log_export::poll();
    EXPECT_EQ(2'000'000'000u, log_export::info().last_sync_ns);
    diagnostic_log::tick(1'000'000'000);
    log_export::poll();
    EXPECT_EQ(7'000'000'000u, log_export::info().last_sync_ns);
}
TEST_F(LogExport, ErrorDrainCoalescesWithinOneSecond)
{
    mount();
    ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_DISK));
    os1_log_record row{};
    row.level = OS1_LOG_ERROR;
    row.kind = OS1_LOG_EVENT;
    diagnostic_log::append(row);
    diagnostic_log::tick(1'000'000'000);
    log_export::poll();
    EXPECT_EQ(1'000'000'000u, log_export::info().last_sync_ns);
    diagnostic_log::append(row);
    diagnostic_log::tick(250'000'000);
    log_export::poll();
    EXPECT_EQ(1'000'000'000u, log_export::info().last_sync_ns);
    diagnostic_log::tick(750'000'000);
    log_export::poll();
    EXPECT_EQ(2'000'000'000u, log_export::info().last_sync_ns);
}
TEST_F(LogExport, ForegroundAdmissionDefersExportAndResumesAfterward)
{
    {
        log_export::ForegroundGuard foreground;
        ASSERT_TRUE(foreground.held);
        EXPECT_EQ(OS1_ERR_WOULD_BLOCK, log_export::run(OS1_LOG_EXPORT_TMP));
        diagnostic_log::tick(2'000'000'000);
        log_export::poll();
        EXPECT_EQ(0u, log_export::info().failures);
    }
    log_export::after_history_snapshot = [] {
        log_export::ForegroundGuard foreground;
        EXPECT_FALSE(foreground.held);
    };
    ASSERT_EQ(0, log_export::run(OS1_LOG_EXPORT_TMP));
    log_export::ForegroundGuard foreground;
    EXPECT_TRUE(foreground.held);
}
TEST_F(LogExport, AggregatePendingBytesTriggerEarlyDrain)
{
    mount();
    shell();
    for(unsigned i = 0; i < 8; ++i)
    {
        os1_log_command c{};
        command(c, OS1_LOG_COMMAND_BEGIN);
        command(c, OS1_LOG_COMMAND_END);
    }
    events(64);
    diagnostic_log::tick(250'000'000);
    log_export::poll();
    EXPECT_GT(log_export::info().disk_bytes, 32768u);
    EXPECT_EQ(250'000'000u, log_export::info().last_drain_ns);
}
TEST(LogExportAuthority, RequiresControlForExportButNotStatus)
{
    const auto* provider = kernel_provider(OS1_LOG_INTERFACE_ID, 1);
    OpenInstance source{};
    DescriptorEntry output{};
    os1_ipc_message request{};
    os1_ipc_result result{};
    request.member = OS1_LOG_EXPORT;
    provider->invoke(source, OS1_RIGHT_LOG_READ, 0, request, result, output);
    EXPECT_EQ(OS1_ERR_ACCESS, result.application_status);
    provider->invoke(
        source, OS1_RIGHT_LOG_READ | OS1_RIGHT_SYSTEM_CONTROL, 0, request, result, output);
    EXPECT_EQ(OS1_ERR_INVALID, result.application_status);
    request.member = OS1_LOG_EXPORT_INFO;
    result = {};
    provider->invoke(source, OS1_RIGHT_LOG_READ, 0, request, result, output);
    EXPECT_EQ(0, result.application_status);
    EXPECT_EQ(sizeof(os1_log_export_info), result.payload_bytes);
}
}  // namespace

TEST_F(LogExport, ServiceLaneRemainsAvailableInsideExport)
{
    shell();
    int client = log_client();
    ASSERT_GE(client, 0);
    int operation = export_operation(client);
    ASSERT_GE(operation, 0);
    EXPECT_FALSE(ipc_run_kernel_request(KernelDispatch::Service));
    log_export::after_history_snapshot = [] {
        // Export owns admission. Clock, IPC, and descriptor cleanup must proceed.
        for(auto pair : {std::pair{os1_sys_ipc, int(OS1_IPC_SUBMIT)},
                         std::pair{os1_sys_ipc, int(OS1_IPC_RESULT)},
                         std::pair{os1_sys_ipc, int(OS1_IPC_WAIT_MANY)},
                         std::pair{os1_sys_ipc, int(OS1_IPC_CREATE)},
                         std::pair{os1_sys_resource, int(OS1_RESOURCE_CLOSE)}})
        {
            log_export::ForegroundGuard allowed(
                log_export::syscall_needs_filesystem(pair.first, pair.second));
            EXPECT_TRUE(allowed.held);
        }
        for(auto pair : {std::pair{os1_sys_spawn, 0},
                         std::pair{os1_sys_write, 0},
                         std::pair{os1_sys_launch, 0},
                         std::pair{os1_sys_resource, int(OS1_RESOURCE_OPEN_AT)}})
        {
            log_export::ForegroundGuard blocked(
                log_export::syscall_needs_filesystem(pair.first, pair.second));
            EXPECT_FALSE(blocked.held);
        }
        // A published snapshot stays readable while the writer owns its buffer.
        EXPECT_EQ(1u, log_export::info().version);
        auto& self = static_cast<LogExport_ServiceLaneRemainsAvailableInsideExport_Test&>(*active);
        int client = self.log_client();
        os1_ipc_message request{};
        request.version = request.interface_version = 1;
        request.size = sizeof(request);
        request.interface_id = OS1_LOG_INTERFACE_ID;
        request.member = OS1_LOG_INFO;
        int op = self.ipc(OS1_IPC_SUBMIT, client, request);
        ASSERT_GE(op, 0);
        EXPECT_FALSE(ipc_run_kernel_request(KernelDispatch::Export));
        EXPECT_TRUE(ipc_run_kernel_request(KernelDispatch::Service));
        os1_ipc_result result{};
        EXPECT_EQ(0, self.ipc(OS1_IPC_RESULT, op, result));
        EXPECT_EQ(0, result.application_status);
        descriptor_close(self.thread->process->descriptors, op);
        descriptor_close(self.thread->process->descriptors, client);
        auto& table = self.thread->process->descriptors;
        initrd_namespace_root()->allowed_rights |= OS1_RIGHT_INPUT_READ;
        InstanceRef root(open_instance(initrd_namespace_root()));
        int fd = descriptor_install(table, root.get(), OS1_RIGHT_INPUT_READ, 0);
        os1_ipc_kernel_binding binding{
            1, sizeof(binding), OS1_INPUT_INTERFACE_ID, 1, OS1_RIGHT_INPUT_READ};
        client = self.ipc(OS1_IPC_BIND_KERNEL, fd, binding);
        ASSERT_GE(client, 0);
        request.interface_id = OS1_INPUT_INTERFACE_ID;
        request.member = OS1_INPUT_LIST;
        request.payload_bytes = sizeof(os1_input_list);
        op = self.ipc(OS1_IPC_SUBMIT, client, request);
        ASSERT_GE(op, 0);
        EXPECT_TRUE(ipc_run_kernel_request(KernelDispatch::Service));
        EXPECT_EQ(0, self.ipc(OS1_IPC_RESULT, op, result));
        EXPECT_EQ(0, result.application_status);
        descriptor_close(table, op);
        descriptor_close(table, client);
        descriptor_close(table, fd);
    };
    ASSERT_TRUE(ipc_run_kernel_request(KernelDispatch::Export));
    os1_ipc_result result{};
    EXPECT_EQ(0, ipc(OS1_IPC_RESULT, operation, result));
    EXPECT_EQ(0, result.application_status);
    descriptor_close_all(thread->process->descriptors);
    log_export::ForegroundGuard released;
    EXPECT_TRUE(released.held);
}
