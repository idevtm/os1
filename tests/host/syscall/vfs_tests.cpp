#include "arch/x86_64/cpu/cpu.hpp"
#include "ipc/channel.hpp"
#include "mm/kmem.hpp"
#include "mm/user_copy.hpp"
#include "proc/startup.hpp"
#include "proc/user_program.hpp"
#include "support/initrd_builder.hpp"
#include "support/physical_memory.hpp"
#include "syscall/console_read.hpp"
#include "syscall/process.hpp"
#include "syscall/resource.hpp"
#include "syscall/vfs.hpp"
#include "vfs/filesystem.hpp"
#include "vfs/initrd.hpp"
#include "vfs/tmpfs.hpp"
#ifdef assert
#undef assert
#endif
#ifdef static_assert
#undef static_assert
#endif
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
namespace
{
constexpr uint64_t kBuffer = kUserImageBase;
constexpr uint64_t kRequest = kUserImageBase + 4096;
class Vfs : public ::testing::Test
{
protected:
    os1::host_test::PhysicalMemoryArena arena{32ull * 1024 * 1024};
    PageFrameContainer frames;
    NativeObject* root = nullptr;
    Process* process = nullptr;
    Thread* thread = nullptr;
    os1::host_test::InitrdBuilder archive;
    void SetUp() override
    {
        std::array<BootMemoryRegion, 1> regions{{{0, arena.size(), BootMemoryType::Usable, 0}}};
        ASSERT_TRUE(frames.initialize(regions, 0x300000, kPageFrameBitmapQwordLimit));
        kmem_init(frames);
        ASSERT_TRUE(init_tasks(frames));
        archive.entry(".", {}, 0040755);
        archive.entry("etc/motd", "hello world\n");
        archive.entry("etc", {}, 0040755);
        archive.entry("bin/tool", os1::host_test::tiny_executable(), 0100755);
        archive.entry("big", std::string(4096, 'x'));
        archive.finish();
        ASSERT_EQ(0, vfs_import_initrd(archive.bytes.data(), archive.bytes.size(), root));
        VirtualMemory vm(frames);
        ASSERT_TRUE(vm.allocate_and_map(kBuffer, 2, PageFlags::User | PageFlags::Write));
        process = create_user_process("vfs-test", vm.root());
        ASSERT_NE(nullptr, process);
        ASSERT_EQ(0, descriptor_bootstrap_console(process->descriptors));
        InstanceRef instance(open_instance(root));
        ASSERT_NE(nullptr, instance.get());
        ASSERT_EQ(
            62,
            descriptor_install(table(), instance.get(), kDirectoryRights, 0, 62, kInitrdCeiling));
        process->root_fd = process->cwd_fd = 62;
        thread = create_user_thread(process, kBuffer, kBuffer + 4096, frames, false);
        ASSERT_NE(nullptr, thread);
        set_current_thread(thread);
    }
    void TearDown() override
    {
        cancel_vfs_wait(thread);
        descriptor_close_all(table());
        object_release(root);
        set_current_thread(nullptr);
    }
    DescriptorTable& table()
    {
        return process->descriptors;
    }
    long open(const std::string& path, uint32_t rights, uint32_t ceiling = 0, int dir = 62)
    {
        return vfs_open_descriptor(
            table(), dir, path.data(), path.size(), rights, OS1_DESCRIPTOR_CLOEXEC, ceiling);
    }
    long duplicate(int fd, uint32_t rights, uint32_t ceiling = 0, int target = -1)
    {
        os1_scoped_duplicate_request r{OS1_VFS_ABI_VERSION,
                                       sizeof(r),
                                       target,
                                       rights,
                                       OS1_DESCRIPTOR_CLOEXEC,
                                       ceiling,
                                       {0, 0}};
        return descriptor_duplicate_scoped(table(), fd, r);
    }
    template<class T>
    long call(int fd, uint32_t op, const T& value)
    {
        EXPECT_TRUE(copy_to_user(frames, thread, kRequest, &value, sizeof(value)));
        return sys_resource(frames, thread, fd, op, kRequest);
    }
    long read(int fd, size_t length, uint64_t address = kBuffer)
    {
        long result = 0;
        EXPECT_TRUE(read_or_block(frames, thread, fd, address, length, result));
        return result;
    }
    long directory(int fd, size_t length, uint64_t address = kBuffer)
    {
        return call(
            fd,
            OS1_RESOURCE_READ_DIRECTORY,
            os1_directory_request{
                1, sizeof(os1_directory_request), address, static_cast<uint32_t>(length), 0});
    }
    long seek(int fd, int64_t offset, uint32_t origin = OS1_SEEK_SET)
    {
        return call(fd,
                    OS1_RESOURCE_SEEK,
                    os1_seek_request{1, sizeof(os1_seek_request), offset, origin, 0});
    }
    std::string bytes(size_t count, uint64_t address = kBuffer, Thread* owner = nullptr)
    {
        std::string result(count, '\0');
        EXPECT_TRUE(copy_from_user(frames, owner ? owner : thread, address, result.data(), count));
        return result;
    }
    os1_launch_request launch_request()
    {
        os1_launch_request request{};
        request.version = OS1_STARTUP_VERSION;
        request.size = sizeof(request);
        request.startup.version = OS1_STARTUP_VERSION;
        request.startup.size = sizeof(request.startup);
        request.startup.root_fd = request.startup.cwd_fd = -1;
        request.startup.argc = 2;
        request.startup.envc = 1;
        constexpr char args[] = "tool\0alpha\0MODE=test";
        std::memcpy(request.startup.strings, args, sizeof(args));
        request.startup.string_bytes = sizeof(args);
        request.startup.argv[1] = 5;
        request.startup.envp[0] = 11;
        return request;
    }
};
TEST(VfsAbi, LayoutsAndBounds)
{
    EXPECT_EQ(32u, sizeof(os1_open_request));
    EXPECT_EQ(24u, sizeof(os1_directory_request));
    EXPECT_EQ(16u, sizeof(os1_directory_record));
    EXPECT_EQ(24u, sizeof(os1_seek_request));
    EXPECT_EQ(32u, sizeof(os1_file_info));
    EXPECT_EQ(32u, sizeof(os1_scoped_duplicate_request));
    EXPECT_EQ(2480u, sizeof(os1_startup_record));
    EXPECT_EQ(2880u, sizeof(os1_launch_request));
    EXPECT_EQ(16u, offsetof(os1_launch_request, startup));
    EXPECT_EQ(272u, OS1_DIRECTORY_RECORD_MAX);
}
TEST_F(Vfs, StrictImportRejectsAliasesCollisionsLinksAndMalformedNames)
{
    const auto bad =
        [&](const std::string& first, const std::string& second, uint32_t second_mode = 0100644) {
            os1::host_test::InitrdBuilder b;
            b.entry(first, "x");
            b.entry(second, second_mode == 0040755 ? "" : "y", second_mode);
            b.finish();
            NativeObject* result = nullptr;
            EXPECT_EQ(OS1_ERR_BAD_IMAGE, vfs_import_initrd(b.bytes.data(), b.bytes.size(), result));
            EXPECT_EQ(nullptr, result);
        };
    bad("a", "./a");
    bad("dir/a", "dir//a");
    bad("a", "a/b");
    bad("a", "a", 0040755);
    for(const std::string path : std::vector<std::string>{
            "/absolute", "../parent", "a/../b", "", std::string("a\0b", 3), std::string(256, 'x')})
    {
        os1::host_test::InitrdBuilder b;
        b.entry(path, "x");
        b.finish();
        NativeObject* result = nullptr;
        EXPECT_EQ(OS1_ERR_BAD_IMAGE, vfs_import_initrd(b.bytes.data(), b.bytes.size(), result));
        EXPECT_EQ(nullptr, result);
    }
    for(uint32_t mode : {0120777u, 0020600u})
    {
        os1::host_test::InitrdBuilder b;
        b.entry("bad", "target", mode);
        b.finish();
        NativeObject* result = nullptr;
        EXPECT_EQ(OS1_ERR_BAD_IMAGE, vfs_import_initrd(b.bytes.data(), b.bytes.size(), result));
    }
    os1::host_test::InitrdBuilder link;
    link.entry("hard", "x", 0100644, 2);
    link.finish();
    NativeObject* result = nullptr;
    EXPECT_EQ(OS1_ERR_BAD_IMAGE, vfs_import_initrd(link.bytes.data(), link.bytes.size(), result));
}
TEST_F(Vfs, ImportRejectsTruncationHeaderCorruptionMissingTrailerAndQuotas)
{
    NativeObject* result = nullptr;
    for(size_t size : {size_t{0}, size_t{109}, archive.bytes.size() - 1})
        EXPECT_EQ(OS1_ERR_BAD_IMAGE, vfs_import_initrd(archive.bytes.data(), size, result));
    auto malformed = archive.bytes;
    malformed[14] = 'Z';
    EXPECT_EQ(OS1_ERR_BAD_IMAGE, vfs_import_initrd(malformed.data(), malformed.size(), result));
    EXPECT_EQ(OS1_ERR_LIMIT, vfs_import_initrd(nullptr, 64ull * 1024 * 1024 + 1, result));
    os1::host_test::InitrdBuilder b;
    for(int i = 0; i < 2048; ++i)
        b.entry(std::to_string(i), "");
    b.finish();
    EXPECT_EQ(OS1_ERR_LIMIT, vfs_import_initrd(b.bytes.data(), b.bytes.size(), result));
    EXPECT_EQ(nullptr, result);
}
TEST_F(Vfs, RelativePathGrammarAndTargetRights)
{
    EXPECT_GE(open("etc//./motd", OS1_RIGHT_READ), 0);
    for(const std::string path : std::vector<std::string>{
            "/etc/motd", "../etc/motd", "etc/../motd", "", std::string("etc\0/motd", 9)})
        EXPECT_EQ(OS1_ERR_INVALID, open(path, OS1_RIGHT_READ));
    EXPECT_EQ(OS1_ERR_NOT_DIRECTORY, open("etc/motd/", OS1_RIGHT_READ));
    EXPECT_EQ(OS1_ERR_NOT_DIRECTORY, open("etc/motd/.", OS1_RIGHT_READ));
    EXPECT_EQ(OS1_ERR_NOT_FOUND, open("missing", OS1_RIGHT_READ));
    EXPECT_EQ(OS1_ERR_ACCESS, open("etc/motd", OS1_RIGHT_WRITE));
    EXPECT_EQ(OS1_ERR_INVALID, open("etc/motd", OS1_RIGHT_READ, OS1_RIGHT_READ));
    EXPECT_EQ(OS1_ERR_ACCESS, open("etc", OS1_RIGHT_READ));
}
TEST_F(Vfs, EnumerationAndLookupAreIndependentAndNoDelegationCanBeRecovered)
{
    long listing = open("etc", OS1_RIGHT_ENUMERATE);
    ASSERT_GE(listing, 0);
    EXPECT_GT(directory(listing, 272), 0);
    EXPECT_EQ(OS1_ERR_ACCESS, open("motd", OS1_RIGHT_READ, 0, listing));
    long lookup = open("etc", OS1_RIGHT_LOOKUP, OS1_RIGHT_READ | OS1_RIGHT_LOOKUP);
    ASSERT_GE(lookup, 0);
    EXPECT_EQ(OS1_ERR_ACCESS, directory(lookup, 272));
    EXPECT_GE(open("motd", OS1_RIGHT_READ, 0, lookup), 0);
    EXPECT_EQ(OS1_ERR_ACCESS, open(".", OS1_RIGHT_LOOKUP | OS1_RIGHT_DELEGATE, 0, lookup));
    long stripped = duplicate(62, OS1_RIGHT_LOOKUP, kInitrdCeiling);
    ASSERT_GE(stripped, 0);
    EXPECT_EQ(OS1_ERR_ACCESS, open("etc", OS1_RIGHT_LOOKUP | OS1_RIGHT_DELEGATE, 0, stripped));
    EXPECT_EQ(OS1_ERR_ACCESS, open("etc", OS1_RIGHT_LOOKUP, kInitrdCeiling, stripped));
    EXPECT_EQ(OS1_ERR_ACCESS, duplicate(stripped, OS1_RIGHT_LOOKUP));
}
TEST_F(Vfs, ScopedDuplicationCannotWidenEitherRightsOrCeiling)
{
    long scoped = open("etc", OS1_RIGHT_LOOKUP | OS1_RIGHT_DELEGATE, OS1_RIGHT_READ);
    ASSERT_GE(scoped, 0);
    EXPECT_EQ(OS1_ERR_ACCESS, duplicate(scoped, OS1_RIGHT_LOOKUP | OS1_RIGHT_ENUMERATE));
    EXPECT_EQ(OS1_ERR_ACCESS, duplicate(scoped, OS1_RIGHT_LOOKUP, OS1_RIGHT_EXECUTE));
    EXPECT_GE(duplicate(scoped, OS1_RIGHT_LOOKUP, OS1_RIGHT_READ), 0);
}
TEST_F(Vfs, SharedFileCursorIndependentOpenSeekAndEof)
{
    long file =
        open("etc/motd", OS1_RIGHT_READ | OS1_RIGHT_SEEK | OS1_RIGHT_DELEGATE | OS1_RIGHT_INSPECT);
    ASSERT_GE(file, 0);
    long alias = duplicate(file, OS1_RIGHT_READ | OS1_RIGHT_SEEK);
    ASSERT_GE(alias, 0);
    long independent = open("etc/motd", OS1_RIGHT_READ);
    ASSERT_GE(independent, 0);
    EXPECT_EQ(5, read(file, 5));
    EXPECT_EQ("hello", bytes(5));
    EXPECT_EQ(6, read(alias, 6));
    EXPECT_EQ(" world", bytes(6));
    EXPECT_EQ(5, read(independent, 5));
    EXPECT_EQ("hello", bytes(5));
    EXPECT_EQ(11, seek(file, -1, OS1_SEEK_END));
    EXPECT_EQ(1, read(alias, 20));
    EXPECT_EQ("\n", bytes(1));
    EXPECT_EQ(0, read(file, 20));
    EXPECT_EQ(0, read(file, 0, 0));
    EXPECT_EQ(OS1_ERR_RANGE, seek(file, -13, OS1_SEEK_END));
    EXPECT_EQ(INT64_MAX, seek(file, INT64_MAX));
    EXPECT_EQ(OS1_ERR_RANGE, seek(file, 1, OS1_SEEK_CUR));
    EXPECT_EQ(0, read(file, 1));
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, descriptor_set_status(table(), file, OS1_STATUS_NONBLOCK));
    EXPECT_EQ(0, sys_resource(frames, thread, file, OS1_RESOURCE_FILE_INFO, kBuffer));
    os1_file_info info{};
    ASSERT_TRUE(copy_from_user(frames, thread, kBuffer, &info, sizeof(info)));
    EXPECT_EQ(12u, info.byte_size);
    EXPECT_EQ(0u, info.descendant_rights);
}
TEST_F(Vfs, FileCopyFailureCommitsOnlyReportedProgress)
{
    long file = open("big", OS1_RIGHT_READ | OS1_RIGHT_SEEK);
    ASSERT_GE(file, 0);
    EXPECT_EQ(OS1_ERR_FAULT, read(file, 5, 0x1000));
    EXPECT_EQ(0, seek(file, 0, OS1_SEEK_CUR));
    EXPECT_EQ(256, read(file, 512, kBuffer + 8192 - 256));
    EXPECT_EQ(256, seek(file, 0, OS1_SEEK_CUR));
    EXPECT_EQ(OS1_ERR_INVALID, read(file, OS1_IO_MAX_BYTES + 1));
}
TEST_F(Vfs, DirectoryRecordsAreCompleteZeroPaddedAndRetryable)
{
    long dir = open("etc", OS1_RIGHT_ENUMERATE | OS1_RIGHT_DELEGATE);
    ASSERT_GE(dir, 0);
    EXPECT_EQ(0, directory(dir, 0, 0));
    EXPECT_EQ(OS1_ERR_BUFFER_TOO_SMALL, directory(dir, 1));
    os1_directory_request output{};
    ASSERT_TRUE(copy_from_user(frames, thread, kRequest, &output, sizeof(output)));
    EXPECT_EQ(24u, output.required_bytes);
    EXPECT_EQ(OS1_ERR_FAULT, directory(dir, 272, 0x1000));
    long alias = duplicate(dir, OS1_RIGHT_ENUMERATE);
    ASSERT_GE(alias, 0);
    ASSERT_EQ(24, directory(alias, 24));
    const auto data = bytes(24);
    const auto* record = reinterpret_cast<const os1_directory_record*>(data.data());
    EXPECT_EQ(1u, record->version);
    EXPECT_EQ(24u, record->record_bytes);
    EXPECT_EQ(4u, record->name_bytes);
    EXPECT_EQ(OS1_OBJECT_FILE, record->kind);
    EXPECT_EQ(0u, record->reserved);
    EXPECT_EQ("motd", data.substr(16, 4));
    EXPECT_EQ(std::string(4, '\0'), data.substr(20));
    EXPECT_EQ(0, directory(dir, 24));
    long fresh = open("etc", OS1_RIGHT_ENUMERATE);
    EXPECT_EQ(24, directory(fresh, 24));
}
TEST_F(Vfs, DirectoryLaterFaultPreservesTheNextUndeliveredRecord)
{
    long dir = open(".", OS1_RIGHT_ENUMERATE);
    ASSERT_GE(dir, 0);
    EXPECT_EQ(24, directory(dir, 48, kBuffer + 8192 - 24));
    EXPECT_EQ(48, directory(dir, 1024));
    EXPECT_EQ(0, directory(dir, 1024));
}
TEST_F(Vfs, CursorContentionSleepsAndRetainsAcceptedFileAcrossReuse)
{
    long file = open("etc/motd", OS1_RIGHT_READ);
    ASSERT_GE(file, 0);
    OpenInstance* instance = nullptr;
    ASSERT_EQ(0, descriptor_acquire(table(), file, OS1_RIGHT_READ, instance));
    InstanceRef pin(instance);
    atomic_store_release(&instance->cursor_busy, true);
    long result = 0;
    ASSERT_FALSE(read_or_block(frames, thread, file, kBuffer, 5, result));
    EXPECT_EQ(ThreadWaitReason::VfsIo, thread->wait.reason);
    EXPECT_EQ(ThreadState::Blocked, thread->state);
    ASSERT_EQ(0, descriptor_close(table(), file));
    ASSERT_EQ(file, duplicate(1, OS1_RIGHT_WRITE, 0, file));
    atomic_store_release(&instance->cursor_busy, false);
    wake_vfs_waiters(frames);
    EXPECT_EQ(ThreadState::Blocked, thread->state);  // still current
    set_current_thread(nullptr);
    wake_vfs_waiters(frames);
    EXPECT_EQ(ThreadState::Ready, thread->state);
    EXPECT_EQ(5u, thread->frame.rax);
    EXPECT_EQ("hello", bytes(5));
    EXPECT_EQ(0u, thread->io_pins);
    EXPECT_EQ(1u, instance->references);
}
TEST_F(Vfs, StartupBoundsAndCanonicalOffsets)
{
    auto request = launch_request();
    EXPECT_EQ(0, validate_startup(request.startup));
    request.startup.argv[1] = 4;
    EXPECT_EQ(OS1_ERR_INVALID, validate_startup(request.startup));
    request = launch_request();
    request.startup.envc = 17;
    EXPECT_EQ(OS1_ERR_INVALID, validate_startup(request.startup));
    request = launch_request();
    request.startup.string_bytes = 2049;
    EXPECT_EQ(OS1_ERR_INVALID, validate_startup(request.startup));
    request = launch_request();
    request.startup.strings[request.startup.string_bytes - 1] = 'x';
    EXPECT_EQ(OS1_ERR_INVALID, validate_startup(request.startup));
    request = launch_request();
    request.startup.root_fd = 64;
    EXPECT_EQ(OS1_ERR_INVALID, validate_startup(request.startup));
}
TEST_F(Vfs, ChildMapsAreExplicitAndTransactionalWithCycles)
{
    auto request = launch_request();
    request.grant_count = 2;
    request.grants[0] = {1, 2, OS1_RIGHT_WRITE, 0, 0, 0};
    request.grants[1] = {2, 1, OS1_RIGHT_WRITE, 0, 0, 0};
    DescriptorTable child;
    EXPECT_EQ(0, install_child_grants(table(), child, request));
    EXPECT_EQ(table().entries[1].instance, child.entries[2].instance);
    EXPECT_EQ(table().entries[2].instance, child.entries[1].instance);
    EXPECT_EQ(nullptr, child.entries[0].instance);
    EXPECT_EQ(nullptr, child.entries[62].instance);
    descriptor_close_all(child);
    descriptor_table_initialize(child);
    request.grants[1].target = 2;
    EXPECT_EQ(OS1_ERR_INVALID, install_child_grants(table(), child, request));
    for(const auto& entry : child.entries)
        EXPECT_EQ(nullptr, entry.instance);
    request.grants[1].target = 1;
    request.grants[1].rights = OS1_RIGHT_READ;
    EXPECT_EQ(OS1_ERR_ACCESS, install_child_grants(table(), child, request));
    EXPECT_NE(nullptr, table().entries[1].instance);
}
TEST_F(Vfs, NativeSpawnUsesExecutableAuthorityAndCopiedStartup)
{
    auto request = launch_request();
    long image = open("bin/tool", OS1_RIGHT_READ | OS1_RIGHT_SEEK);
    ASSERT_GE(image, 0);
    ASSERT_TRUE(copy_to_user(frames, thread, kRequest, &request, sizeof(request)));
    ProcessSyscallContext context{.frames = &frames, .kernel_root_cr3 = thread->address_space_cr3};
    EXPECT_EQ(OS1_ERR_ACCESS, sys_launch(context, image, OS1_LAUNCH_SPAWN, kRequest));
    descriptor_close(table(), image);
    image = open("bin/tool", OS1_RIGHT_EXECUTE | OS1_RIGHT_SEEK);
    ASSERT_GE(image, 0);
    EXPECT_EQ(10, seek(image, 10));
    ASSERT_TRUE(copy_to_user(frames, thread, kRequest, &request, sizeof(request)));
    const long pid = sys_launch(context, image, OS1_LAUNCH_SPAWN, kRequest);
    ASSERT_GT(pid, 0);
    Thread* child = nullptr;
    for(auto* item = first_thread(); item; item = next_thread(item))
        if(item->process->pid == static_cast<uint64_t>(pid))
            child = item;
    ASSERT_NE(nullptr, child);
    EXPECT_EQ(-1, child->process->root_fd);
    EXPECT_EQ(nullptr, child->process->descriptors.entries[62].instance);
    os1_startup_record startup{};
    ASSERT_TRUE(copy_from_user(frames, child, child->frame.rdi, &startup, sizeof(startup)));
    EXPECT_EQ(2u, startup.argc);
    EXPECT_EQ(1u, startup.envc);
    EXPECT_STREQ("alpha", startup.strings + startup.argv[1]);
    EXPECT_STREQ("MODE=test", startup.strings + startup.envp[0]);
    EXPECT_EQ(10, seek(image, 0, OS1_SEEK_CUR));
}
TEST_F(Vfs, FailedExecAndMissingNamespaceCannotRecoverAmbientRoot)
{
    auto request = launch_request();
    request.startup.root_fd = 62;
    long image = open("bin/tool", OS1_RIGHT_EXECUTE);
    ASSERT_GE(image, 0);
    ASSERT_EQ(0, descriptor_set_flags(table(), 62, OS1_DESCRIPTOR_CLOEXEC));
    ASSERT_TRUE(copy_to_user(frames, thread, kRequest, &request, sizeof(request)));
    const uint64_t old = thread->address_space_cr3;
    ProcessSyscallContext context{
        .frames = &frames, .kernel_root_cr3 = old, .write_cr3 = [](uint64_t) {}};
    EXPECT_EQ(OS1_ERR_INVALID, sys_launch(context, image, OS1_LAUNCH_EXEC, kRequest));
    EXPECT_EQ(old, thread->address_space_cr3);
    EXPECT_NE(nullptr, table().entries[image].instance);
    ASSERT_EQ(0, descriptor_close(table(), 62));
    OpenInstance* output = nullptr;
    EXPECT_EQ(OS1_ERR_BAD_DESCRIPTOR, resolve_process_image(process, "/bin/tool", output));
    EXPECT_EQ(nullptr, output);
    EXPECT_EQ(OS1_ERR_BAD_DESCRIPTOR, open("bin/tool", OS1_RIGHT_EXECUTE));
}

TEST_F(Vfs, OpenSyscallRejectsMalformedRequestsAndCopiesBoundedPaths)
{
    constexpr char path[] = "etc/motd";
    ASSERT_TRUE(copy_to_user(frames, thread, kBuffer, path, sizeof(path) - 1));
    os1_open_request request{
        1, sizeof(request), kBuffer, sizeof(path) - 1, OS1_RIGHT_READ, OS1_DESCRIPTOR_CLOEXEC, 0};
    const long fd = call(62, OS1_RESOURCE_OPEN_AT, request);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(5, read(fd, 5));
    EXPECT_EQ("hello", bytes(5));
    request.path = 0x1000;
    EXPECT_EQ(OS1_ERR_FAULT, call(62, OS1_RESOURCE_OPEN_AT, request));
    request.path_bytes = OS1_PATH_MAX + 1;
    EXPECT_EQ(OS1_ERR_INVALID, call(62, OS1_RESOURCE_OPEN_AT, request));
    request.path_bytes = 0;
    EXPECT_EQ(OS1_ERR_INVALID, call(62, OS1_RESOURCE_OPEN_AT, request));
    request.path_bytes = 8;
    request.version = 2;
    EXPECT_EQ(OS1_ERR_INVALID, call(62, OS1_RESOURCE_OPEN_AT, request));
    request.version = 1;
    request.size = 0;
    EXPECT_EQ(OS1_ERR_INVALID, call(62, OS1_RESOURCE_OPEN_AT, request));
    request.size = sizeof(request);
    request.path = kBuffer;
    request.descriptor_flags = 2;
    EXPECT_EQ(OS1_ERR_INVALID, call(62, OS1_RESOURCE_OPEN_AT, request));
    EXPECT_EQ(OS1_ERR_FAULT, sys_resource(frames, thread, 62, OS1_RESOURCE_OPEN_AT, 0x1000));
}
TEST_F(Vfs, LongNamesPathsAndImportDepthHaveExactBounds)
{
    os1::host_test::InitrdBuilder b;
    const std::string name(255, 'n');
    b.entry(name, "x");
    b.finish();
    NativeObject* imported = nullptr;
    ASSERT_EQ(0, vfs_import_initrd(b.bytes.data(), b.bytes.size(), imported));
    InstanceRef instance(open_instance(imported));
    object_release(imported);
    long fd = descriptor_install(table(), instance.get(), OS1_RIGHT_ENUMERATE, 0);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(272, directory(fd, 272));
    os1_directory_record record{};
    ASSERT_TRUE(copy_from_user(frames, thread, kBuffer, &record, sizeof(record)));
    EXPECT_EQ(255, record.name_bytes);
    EXPECT_EQ(272u, record.record_bytes);
    std::string path = "etc/" + std::string(OS1_PATH_MAX - 8, '/') + "motd";
    ASSERT_EQ(OS1_PATH_MAX, path.size());
    EXPECT_GE(open(path, OS1_RIGHT_READ), 0);
    path.insert(4, "/");
    EXPECT_EQ(OS1_ERR_INVALID, open(path, OS1_RIGHT_READ));
    for(unsigned depth : {64u, 65u})
    {
        os1::host_test::InitrdBuilder deep;
        std::string nested;
        for(unsigned i = 1; i < depth; ++i)
            nested += "d/";
        nested += "f";
        deep.entry(nested, "x");
        deep.finish();
        imported = nullptr;
        EXPECT_EQ(depth == 64 ? 0 : OS1_ERR_LIMIT,
                  vfs_import_initrd(deep.bytes.data(), deep.bytes.size(), imported));
        object_release(imported);
    }
}
TEST_F(Vfs, CursorWaitCancellationReleasesItsRetainedReference)
{
    const long fd = open("etc", OS1_RIGHT_ENUMERATE);
    OpenInstance* instance = nullptr;
    ASSERT_EQ(0, descriptor_acquire(table(), fd, 0, instance));
    InstanceRef pin(instance);
    atomic_store_release(&instance->cursor_busy, true);
    EXPECT_EQ(kVfsBlocked, directory(fd, 272));
    EXPECT_EQ(3u, instance->references);
    descriptor_close(table(), fd);
    cancel_vfs_wait(thread);
    EXPECT_EQ(1u, instance->references);
    EXPECT_EQ(ThreadWaitReason::None, thread->wait.reason);
    atomic_store_release(&instance->cursor_busy, false);
    set_current_thread(nullptr);
    wake_vfs_waiters(frames);
    EXPECT_EQ(1u, instance->references);
}
TEST_F(Vfs, NativeLaunchValidatesEnvelopesAndRejectsBadImagesWithoutChangingParent)
{
    auto request = launch_request();
    const long fd = open("etc/motd", OS1_RIGHT_EXECUTE);
    ASSERT_GE(fd, 0);
    const uint64_t old = thread->address_space_cr3;
    ProcessSyscallContext context{
        .frames = &frames, .kernel_root_cr3 = old, .write_cr3 = [](uint64_t) {}};
    ASSERT_TRUE(copy_to_user(frames, thread, kRequest, &request, sizeof(request)));
    EXPECT_EQ(OS1_ERR_BAD_IMAGE, sys_launch(context, fd, OS1_LAUNCH_SPAWN, kRequest));
    EXPECT_EQ(OS1_ERR_BAD_IMAGE, sys_launch(context, fd, OS1_LAUNCH_EXEC, kRequest));
    EXPECT_EQ(old, thread->address_space_cr3);
    EXPECT_NE(nullptr, table().entries[fd].instance);
    EXPECT_EQ(OS1_ERR_FAULT, sys_launch(context, fd, OS1_LAUNCH_SPAWN, 0x1000));
    request.grants[15].source = 1;
    ASSERT_TRUE(copy_to_user(frames, thread, kRequest, &request, sizeof(request)));
    EXPECT_EQ(OS1_ERR_INVALID, sys_launch(context, fd, OS1_LAUNCH_SPAWN, kRequest));
    request = launch_request();
    request.version = OS1_STARTUP_VERSION + 1;
    ASSERT_TRUE(copy_to_user(frames, thread, kRequest, &request, sizeof(request)));
    EXPECT_EQ(OS1_ERR_INVALID, sys_launch(context, fd, OS1_LAUNCH_SPAWN, kRequest));
}
TEST_F(Vfs, NativeExecInstallsStartupAndClosesOnlyCloexecGrants)
{
    auto request = launch_request();
    request.startup.root_fd = request.startup.cwd_fd = 62;
    const long fd = open("bin/tool", OS1_RIGHT_EXECUTE);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(copy_to_user(frames, thread, kRequest, &request, sizeof(request)));
    const uint64_t old = thread->address_space_cr3;
    ProcessSyscallContext context{
        .frames = &frames, .kernel_root_cr3 = old, .write_cr3 = [](uint64_t) {}};
    EXPECT_EQ(0, sys_launch(context, fd, OS1_LAUNCH_EXEC, kRequest));
    EXPECT_NE(old, thread->address_space_cr3);
    EXPECT_EQ(nullptr, table().entries[fd].instance);
    EXPECT_NE(nullptr, table().entries[1].instance);
    EXPECT_NE(nullptr, table().entries[62].instance);
    os1_startup_record startup{};
    ASSERT_TRUE(copy_from_user(frames, thread, thread->frame.rdi, &startup, sizeof(startup)));
    EXPECT_EQ(62, startup.root_fd);
    EXPECT_STREQ("alpha", startup.strings + startup.argv[1]);
    EXPECT_STREQ("MODE=test", startup.strings + startup.envp[0]);
    EXPECT_EQ(8u, thread->frame.rsp & 15u);
}
TEST_F(Vfs, ImageAllocationFailureIsDistinctAndRollsBackPageTables)
{
    const long fd = open("bin/tool", OS1_RIGHT_EXECUTE);
    OpenInstance* image = nullptr;
    ASSERT_EQ(0, descriptor_acquire(table(), fd, OS1_RIGHT_EXECUTE, image));
    InstanceRef pin(image);
    std::vector<uint64_t> held;
    uint64_t page = 0;
    while(frames.allocate(page))
        held.push_back(page);
    ASSERT_GT(held.size(), 8u);
    for(unsigned i = 0; i < 8; ++i)
    {
        ASSERT_TRUE(frames.free(held.back()));
        held.pop_back();
    }
    const auto before = frames.free_page_count();
    uint64_t cr3 = 0, entry = 0, rsp = 0;
    EXPECT_EQ(OS1_ERR_NO_MEMORY,
              load_user_object_image(frames, thread->address_space_cr3, image, cr3, entry, rsp));
    EXPECT_EQ(before, frames.free_page_count());
    EXPECT_EQ(0u, cr3);
    for(auto frame : held)
        EXPECT_TRUE(frames.free(frame));
}

TEST_F(Vfs, TypedStartupEnvelopeRejectsMalformedArguments)
{
    auto request = launch_request();
    auto& args = request.startup.constructor;
    args.schema_id = 42;
    args.version = 1;
    args.size = 16;
    EXPECT_EQ(0, validate_startup(request.startup));
    args.version = 0;
    EXPECT_EQ(OS1_ERR_INVALID, validate_startup(request.startup));
    args.version = 1;
    args.size = 257;
    EXPECT_EQ(OS1_ERR_INVALID, validate_startup(request.startup));
    args.size = 16;
    args.payload[16] = 1;
    EXPECT_EQ(OS1_ERR_INVALID, validate_startup(request.startup));
    args.payload[16] = 0;
    args.schema_id = 0;
    EXPECT_EQ(OS1_ERR_INVALID, validate_startup(request.startup));
}
TEST_F(Vfs, PrivateProviderLaunchRestrictsGrantsAndCopiesConstructorData)
{
    ASSERT_EQ(0, sys_ipc(frames, thread, OS1_IPC_CREATE, 0, kBuffer));
    os1_ipc_pair pair{};
    ASSERT_TRUE(copy_from_user(frames, thread, kBuffer, &pair, sizeof(pair)));
    auto request = launch_request();
    request.grant_count = 1;
    request.grants[0] = {pair.provider, 3, OS1_RIGHT_READ | OS1_RIGHT_WRITE, 0, 0, 0};
    request.startup.constructor = {42, 1, 8, {10}};
    long image = open("bin/tool", OS1_RIGHT_EXECUTE);
    ASSERT_GE(image, 0);
    ProcessSyscallContext context{.frames = &frames, .kernel_root_cr3 = thread->address_space_cr3};
    request.startup.root_fd = 62;
    ASSERT_TRUE(copy_to_user(frames, thread, kRequest, &request, sizeof(request)));
    EXPECT_EQ(OS1_ERR_INVALID, sys_launch(context, image, OS1_LAUNCH_PRIVATE_PROVIDER, kRequest));
    request.startup.root_fd = -1;
    request.grants[0].source = pair.client;
    ASSERT_TRUE(copy_to_user(frames, thread, kRequest, &request, sizeof(request)));
    EXPECT_EQ(OS1_ERR_UNSUPPORTED,
              sys_launch(context, image, OS1_LAUNCH_PRIVATE_PROVIDER, kRequest));
    request.grants[0].source = pair.provider;
    ASSERT_TRUE(copy_to_user(frames, thread, kRequest, &request, sizeof(request)));
    long pid = sys_launch(context, image, OS1_LAUNCH_PRIVATE_PROVIDER, kRequest);
    ASSERT_GT(pid, 0);
    Thread* child = nullptr;
    for(auto* t = first_thread(); t; t = next_thread(t))
        if(t->process->pid == static_cast<uint64_t>(pid))
            child = t;
    ASSERT_NE(nullptr, child);
    EXPECT_TRUE(child->process->private_provider);
    os1_startup_record startup{};
    ASSERT_TRUE(copy_from_user(frames, child, child->frame.rdi, &startup, sizeof(startup)));
    EXPECT_EQ(42u, startup.constructor.schema_id);
    EXPECT_EQ(10, startup.constructor.payload[0]);
    EXPECT_EQ(nullptr, child->process->descriptors.entries[0].instance);
    EXPECT_EQ(nullptr, child->process->descriptors.entries[1].instance);
    descriptor_close_all(child->process->descriptors);
}

TEST_F(Vfs, SupervisedSelfExportRequiresTheMatchingClient)
{
    auto ipc = [&](unsigned op, int fd, auto& value) {
        EXPECT_TRUE(copy_to_user(frames, thread, kBuffer, &value, sizeof(value)));
        long result = sys_ipc(frames, thread, op, fd, kBuffer);
        EXPECT_TRUE(copy_from_user(frames, thread, kBuffer, &value, sizeof(value)));
        return result;
    };
    os1_ipc_pair pair{}, other{};
    ASSERT_EQ(0, ipc(OS1_IPC_CREATE, 0, pair));
    ASSERT_EQ(0, ipc(OS1_IPC_CREATE, 0, other));
    auto request = launch_request();
    request.grant_count = 2;
    request.grants[0] = {pair.provider, 3, OS1_RIGHT_READ | OS1_RIGHT_WRITE, 0, 0, 0};
    request.grants[1] = {other.client,
                         4,
                         OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE,
                         0,
                         0,
                         0};
    long image = open("bin/tool", OS1_RIGHT_EXECUTE);
    ASSERT_GE(image, 0);
    ProcessSyscallContext context{.frames = &frames, .kernel_root_cr3 = thread->address_space_cr3};
    auto spawn = [&]() {
        EXPECT_TRUE(copy_to_user(frames, thread, kRequest, &request, sizeof(request)));
        return sys_launch(context, image, OS1_LAUNCH_PRIVATE_PROVIDER, kRequest);
    };
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, spawn());
    request.grants[1].source = pair.client;
    request.grants[1].target = 5;
    EXPECT_EQ(OS1_ERR_INVALID, spawn());
    request.grants[1].target = 4;
    long pid = spawn();
    ASSERT_GT(pid, 0);
    Thread* child = nullptr;
    for(auto* t = first_thread(); t; t = next_thread(t))
        if(t->process->pid == static_cast<uint64_t>(pid))
            child = t;
    ASSERT_NE(nullptr, child);
    EXPECT_TRUE(child->process->private_provider);
    EXPECT_TRUE(ipc_matching_endpoints(child->process->descriptors.entries[3].instance->object,
                                       child->process->descriptors.entries[4].instance->object));
    descriptor_close_all(child->process->descriptors);
}
TEST_F(Vfs, CapabilityDirectoryCeilingsAndSharedFileCursor)
{
    auto ipc = [&](unsigned op, int fd, auto& value) {
        EXPECT_TRUE(copy_to_user(frames, thread, kBuffer, &value, sizeof(value)));
        long result = sys_ipc(frames, thread, op, fd, kBuffer);
        EXPECT_TRUE(copy_from_user(frames, thread, kBuffer, &value, sizeof(value)));
        return result;
    };
    os1_ipc_pair pair{};
    ASSERT_EQ(0, ipc(OS1_IPC_CREATE, 0, pair));
    os1_ipc_info info{1, sizeof(info), 0, 42, 1, 1};
    ASSERT_EQ(0, ipc(OS1_IPC_READY, pair.provider, info));
    long file = open("etc/motd", OS1_RIGHT_READ | OS1_RIGHT_SEEK | OS1_RIGHT_DELEGATE);
    ASSERT_GE(file, 0);
    ASSERT_EQ(2, read(file, 2));
    os1_ipc_message_caps m{};
    m.message = {1, sizeof(m.message), 0, 0, 42, 1, 1, 0, 0, {}};
    m.capabilities.count = 2;
    m.capabilities.entries[0] = {62, OS1_RIGHT_LOOKUP, OS1_RIGHT_READ, 0};
    m.capabilities.entries[1] = {static_cast<int32_t>(file), OS1_RIGHT_READ, 0, 0};
    long op = ipc(OS1_IPC_SUBMIT_CAPS, pair.client, m);
    ASSERT_GE(op, 0);
    os1_ipc_message_caps got{};
    ASSERT_EQ(0, ipc(OS1_IPC_RECEIVE_CAPS, pair.provider, got));
    ASSERT_EQ(2u, got.capabilities.count);
    auto dir = got.capabilities.entries[0].fd;
    EXPECT_EQ(OS1_RIGHT_READ, got.capabilities.entries[0].descendant_rights);
    EXPECT_EQ(OS1_ERR_ACCESS, open("bin/tool", OS1_RIGHT_EXECUTE, 0, dir));
    EXPECT_GE(open("etc/motd", OS1_RIGHT_READ, 0, dir), 0);
    EXPECT_EQ(3, read(got.capabilities.entries[1].fd, 3));
    EXPECT_EQ("llo", bytes(3));
    EXPECT_EQ(1, read(file, 1));
    EXPECT_EQ(" ", bytes(1));
    os1_ipc_result reply{
        1, sizeof(reply), got.message.generation, got.message.request_id, 0, 0, 0, 0, {}};
    EXPECT_EQ(0, ipc(OS1_IPC_REPLY, pair.provider, reply));
}
}  // namespace

namespace
{
class TmpfsContract : public Vfs
{
protected:
    int directory = -1, client = -1;
    VfsNode* node = nullptr;
    void SetUp() override
    {
        Vfs::SetUp();
        ASSERT_EQ(0, tmpfs_mount(root));
        InstanceRef root_open(open_instance(root));
        ASSERT_EQ(62,
                  descriptor_install(
                      table(), root_open.get(), kDirectoryRights, 0, 62, kNamespaceCeiling));
        directory = open("tmp", kTmpDirectoryRights, kNamespaceCeiling);
        ASSERT_GE(directory, 0);
        node = static_cast<VfsNode*>(table().entries[directory].instance->object);
        client = bind(directory, OS1_DIRECTORY_INTERFACE_ID, kTmpDirectoryRights);
        ASSERT_GE(client, 0);
    }
    void TearDown() override
    {
        host_tmpfs_allocation_budget = -1;
        while(ipc_run_kernel_request())
        {
        }
        cancel_ipc_wait(thread);
        Vfs::TearDown();
    }
    template<class T>
    long ipc(unsigned operation, int fd, T& value)
    {
        EXPECT_TRUE(copy_to_user(frames, thread, kRequest, &value, sizeof(value)));
        long result = sys_ipc(frames, thread, operation, fd, kRequest);
        EXPECT_TRUE(copy_from_user(frames, thread, kRequest, &value, sizeof(value)));
        return result;
    }
    int bind(int fd, uint64_t id, uint32_t rights)
    {
        os1_ipc_kernel_binding b{1, sizeof(b), id, 1, rights};
        return ipc(OS1_IPC_BIND_KERNEL, fd, b);
    }
    int submit(
        int endpoint, uint64_t id, unsigned member, const void* bytes = nullptr, size_t length = 0)
    {
        os1_ipc_message m{};
        m.version = 1;
        m.size = sizeof(m);
        m.interface_id = id;
        m.interface_version = 1;
        m.member = member;
        m.payload_bytes = length;
        if(length)
            std::memcpy(m.payload, bytes, length);
        return ipc(OS1_IPC_SUBMIT, endpoint, m);
    }
    os1_ipc_result result(int operation)
    {
        EXPECT_TRUE(ipc_run_kernel_request());
        os1_ipc_result r{};
        EXPECT_EQ(0, ipc(OS1_IPC_RESULT, operation, r));
        EXPECT_EQ(0, r.transport_status);
        return r;
    }
    os1_ipc_result invoke(
        int endpoint, uint64_t id, unsigned member, const void* bytes = nullptr, size_t length = 0)
    {
        int operation = submit(endpoint, id, member, bytes, length);
        EXPECT_GE(operation, 0);
        auto r = result(operation);
        EXPECT_EQ(0, descriptor_close(table(), operation));
        return r;
    }
    os1_directory_open args(const char* name,
                            unsigned kind = OS1_OBJECT_FILE,
                            uint32_t rights = kTmpFileRights,
                            uint32_t ceiling = 0)
    {
        os1_directory_open a{rights, ceiling, static_cast<uint32_t>(std::strlen(name)), kind, {}};
        std::memcpy(a.name, name, a.name_bytes);
        return a;
    }
    long create(const char* name,
                unsigned kind = OS1_OBJECT_FILE,
                uint32_t rights = kTmpFileRights,
                uint32_t ceiling = 0)
    {
        auto a = args(name, kind, rights, ceiling);
        int operation =
            submit(client, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_CREATE, &a, sizeof(a));
        EXPECT_GE(operation, 0);
        auto r = result(operation);
        long value = r.application_status;
        if(!value)
        {
            os1_ipc_result_caps caps{};
            EXPECT_EQ(0, ipc(OS1_IPC_RESULT_CAPS, operation, caps));
            EXPECT_EQ(1u, caps.capabilities.count);
            value = caps.capabilities.entries[0].fd;
        }
        descriptor_close(table(), operation);
        return value;
    }
    long remove(const char* name)
    {
        os1_directory_name a{static_cast<uint32_t>(std::strlen(name)), 0, {}};
        std::memcpy(a.name, name, a.name_bytes);
        return invoke(client, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_REMOVE, &a, sizeof(a))
            .application_status;
    }
    long rename(const char* from, const char* to)
    {
        os1_directory_rename a{static_cast<uint32_t>(std::strlen(from)),
                               static_cast<uint32_t>(std::strlen(to)),
                               {},
                               {}};
        std::memcpy(a.source, from, a.source_bytes);
        std::memcpy(a.destination, to, a.destination_bytes);
        return invoke(client, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_RENAME, &a, sizeof(a))
            .application_status;
    }
    long file_call(int f, unsigned member, const void* bytes = nullptr, size_t count = 0)
    {
        return invoke(f, OS1_FILE_INTERFACE_ID, member, bytes, count).application_status;
    }
};
TEST_F(TmpfsContract, MountedEmptyAndInitrdImmutable)
{
    EXPECT_EQ(80u, sizeof(os1_directory_open));
    EXPECT_EQ(72u, sizeof(os1_directory_name));
    EXPECT_EQ(136u, sizeof(os1_directory_rename));
    EXPECT_EQ(72u, sizeof(os1_directory_entry));
    EXPECT_EQ(8u, sizeof(os1_file_read));
    EXPECT_EQ(32u, sizeof(os1_filesystem_info));
    EXPECT_EQ(0u, invoke(client, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_READ).payload_bytes);
    EXPECT_EQ(OS1_ERR_EXISTS, tmpfs_mount(root));
    EXPECT_EQ(OS1_ERR_ACCESS, open("etc/motd", OS1_RIGHT_WRITE));
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, bind(62, OS1_DIRECTORY_INTERFACE_ID, OS1_RIGHT_LOOKUP));
    EXPECT_EQ(OS1_ERR_ACCESS, open("tmp", OS1_RIGHT_EXECUTE));
    auto info = tmpfs_info(node);
    EXPECT_EQ(1u, info.nodes);
    EXPECT_EQ(1u, info.open_instances);
    EXPECT_EQ(0u, info.data_pages);
}
TEST_F(TmpfsContract, NativeCreateOpenAndCompatibilityShareStateAndCursors)
{
    int fd = create("file");
    ASSERT_GE(fd, 0);
    int f = bind(fd, OS1_FILE_INTERFACE_ID, kTmpFileRights);
    ASSERT_GE(f, 0);
    EXPECT_EQ(0, file_call(f, OS1_FILE_WRITE, "hello", 5));
    EXPECT_EQ(5u, table().entries[fd].instance->cursor);
    os1_seek_request seek{1, sizeof(seek), 0, OS1_SEEK_SET, 0};
    EXPECT_EQ(0, file_call(f, OS1_FILE_SEEK, &seek, sizeof(seek)));
    long read = -1;
    ASSERT_TRUE(read_or_block(frames, thread, fd, kBuffer, 2, read));
    EXPECT_EQ(2, read);
    char text[6]{};
    EXPECT_TRUE(copy_from_user(frames, thread, kBuffer, text, 2));
    EXPECT_STREQ("he", text);
    os1_file_read request{3, 0};
    auto r = invoke(f, OS1_FILE_INTERFACE_ID, OS1_FILE_READ, &request, sizeof(request));
    EXPECT_EQ(3u, r.payload_bytes);
    EXPECT_EQ(0, std::memcmp(r.payload, "llo", 3));
    int second = open("tmp/file", OS1_RIGHT_READ | OS1_RIGHT_SEEK);
    ASSERT_GE(second, 0);
    EXPECT_EQ(0u, table().entries[second].instance->cursor);
    auto a = args("file", 0, OS1_RIGHT_READ);
    int op = submit(client, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_OPEN, &a, sizeof(a));
    EXPECT_EQ(0, result(op).application_status);
    os1_ipc_result_caps caps{};
    ASSERT_EQ(0, ipc(OS1_IPC_RESULT_CAPS, op, caps));
    EXPECT_EQ(1u, caps.capabilities.count);
    EXPECT_EQ(OS1_RIGHT_READ, caps.capabilities.entries[0].rights);
    descriptor_close(table(), op);
    EXPECT_EQ(OS1_ERR_EXISTS, create("file"));
    EXPECT_EQ(OS1_ERR_ACCESS, open("tmp/file", OS1_RIGHT_EXECUTE));
}
TEST_F(TmpfsContract, NativeReadResultFaultRetainsBytesWithoutAdvancingAgain)
{
    int fd = create("file");
    ASSERT_GE(fd, 0);
    auto* file = static_cast<VfsNode*>(table().entries[fd].instance->object);
    ASSERT_EQ(3, tmpfs_write(file, 0, "abc", 3));
    int f = bind(fd, OS1_FILE_INTERFACE_ID, OS1_RIGHT_READ);
    ASSERT_GE(f, 0);
    os1_file_read read{2, 0};
    int op = submit(f, OS1_FILE_INTERFACE_ID, OS1_FILE_READ, &read, sizeof(read));
    auto r = result(op);
    EXPECT_EQ(2u, r.payload_bytes);
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_RESULT, op, 0));
    os1_ipc_result retry{};
    EXPECT_EQ(0, ipc(OS1_IPC_RESULT, op, retry));
    EXPECT_EQ(0, std::memcmp(retry.payload, "ab", 2));
    EXPECT_EQ(2u, table().entries[fd].instance->cursor);
    descriptor_close(table(), op);
    auto last = invoke(f, OS1_FILE_INTERFACE_ID, OS1_FILE_READ, &read, sizeof(read));
    EXPECT_EQ(1u, last.payload_bytes);
    EXPECT_EQ('c', last.payload[0]);
}
TEST_F(TmpfsContract, CapabilityDeliveryFaultAndTableFullDoNotRepeatCreate)
{
    auto a = args("once");
    int op = submit(client, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_CREATE, &a, sizeof(a));
    EXPECT_EQ(0, result(op).application_status);
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_RESULT_CAPS, op, 0));
    std::vector<int> aliases;
    for(;;)
    {
        long fd = duplicate(directory, OS1_RIGHT_LOOKUP);
        if(fd < 0)
        {
            EXPECT_EQ(OS1_ERR_LIMIT, fd);
            break;
        }
        aliases.push_back(fd);
    }
    os1_ipc_result_caps caps{};
    EXPECT_EQ(OS1_ERR_LIMIT, ipc(OS1_IPC_RESULT_CAPS, op, caps));
    ASSERT_FALSE(aliases.empty());
    descriptor_close(table(), aliases.back());
    aliases.pop_back();
    EXPECT_EQ(0, ipc(OS1_IPC_RESULT_CAPS, op, caps));
    EXPECT_EQ(1u, caps.capabilities.count);
    EXPECT_EQ(2u, tmpfs_info(node).nodes);
    caps = {};
    EXPECT_EQ(0, ipc(OS1_IPC_RESULT_CAPS, op, caps));
    EXPECT_EQ(0u, caps.capabilities.count);
    descriptor_close(table(), op);
}
TEST_F(TmpfsContract, ServiceRightsAndDescendantCeilingsCannotBeAmplified)
{
    int fd = create("file");
    ASSERT_GE(fd, 0);
    int observer = bind(directory, OS1_DIRECTORY_INTERFACE_ID, OS1_RIGHT_INSPECT);
    ASSERT_GE(observer, 0);
    auto a = args("denied");
    EXPECT_EQ(OS1_ERR_ACCESS,
              invoke(observer, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_CREATE, &a, sizeof(a))
                  .application_status);
    int restricted = duplicate(directory, OS1_RIGHT_LOOKUP | OS1_RIGHT_DELEGATE, OS1_RIGHT_READ);
    ASSERT_GE(restricted, 0);
    int c = bind(restricted, OS1_DIRECTORY_INTERFACE_ID, OS1_RIGHT_LOOKUP | OS1_RIGHT_DELEGATE);
    ASSERT_GE(c, 0);
    a = args("file", 0, OS1_RIGHT_WRITE);
    EXPECT_EQ(OS1_ERR_ACCESS,
              invoke(c, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_OPEN, &a, sizeof(a))
                  .application_status);
    int no_delegate = bind(directory, OS1_DIRECTORY_INTERFACE_ID, OS1_RIGHT_LOOKUP);
    ASSERT_GE(no_delegate, 0);
    a = args("file", 0, OS1_RIGHT_READ | OS1_RIGHT_DELEGATE);
    EXPECT_EQ(OS1_ERR_ACCESS,
              invoke(no_delegate, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_OPEN, &a, sizeof(a))
                  .application_status);
    int readonly = bind(fd, OS1_FILE_INTERFACE_ID, OS1_RIGHT_READ);
    ASSERT_GE(readonly, 0);
    EXPECT_EQ(OS1_ERR_ACCESS, file_call(readonly, OS1_FILE_WRITE, "x", 1));
    EXPECT_EQ(2u, tmpfs_info(node).nodes);
}
TEST_F(TmpfsContract, RenameAndRemovalAreAtomicAndRetainOpenFiles)
{
    int fd = create("first");
    ASSERT_GE(fd, 0);
    int other = create("other");
    ASSERT_GE(other, 0);
    EXPECT_EQ(OS1_ERR_EXISTS, rename("first", "other"));
    EXPECT_EQ(0, rename("first", "first"));
    EXPECT_EQ(0, rename("first", "renamed"));
    EXPECT_EQ(OS1_ERR_NOT_FOUND, open("tmp/first", OS1_RIGHT_READ));
    EXPECT_EQ(0, remove("renamed"));
    EXPECT_EQ(OS1_ERR_NOT_FOUND, open("tmp/renamed", OS1_RIGHT_READ));
    auto* file = static_cast<VfsNode*>(table().entries[fd].instance->object);
    EXPECT_EQ(1, tmpfs_write(file, 0, "x", 1));
    EXPECT_EQ(3u, tmpfs_info(node).nodes);
    EXPECT_EQ(0, descriptor_close(table(), fd));
    EXPECT_EQ(2u, tmpfs_info(node).nodes);
    EXPECT_EQ(0u, tmpfs_info(node).data_pages);
}
TEST_F(TmpfsContract, NestedDirectoryNonemptyRemovalAndOrphanMutation)
{
    int dir = create("child", OS1_OBJECT_DIRECTORY, kTmpDirectoryRights, kNamespaceCeiling);
    ASSERT_GE(dir, 0);
    auto* child = static_cast<VfsNode*>(table().entries[dir].instance->object);
    OpenInstance* file = nullptr;
    ASSERT_EQ(0, tmpfs_create(child, "nested", 6, OS1_OBJECT_FILE, file));
    EXPECT_EQ(OS1_ERR_NOT_EMPTY, remove("child"));
    int nested = open("tmp/child/nested", OS1_RIGHT_READ);
    ASSERT_GE(nested, 0);
    descriptor_close(table(), nested);
    EXPECT_EQ(0, tmpfs_remove(child, "nested", 6));
    instance_release(file);
    EXPECT_EQ(0, remove("child"));
    file = nullptr;
    EXPECT_EQ(OS1_ERR_CLOSED, tmpfs_create(child, "later", 5, OS1_OBJECT_FILE, file));
    EXPECT_EQ(nullptr, file);
    EXPECT_EQ(OS1_ERR_INVALID, open("tmp/../etc/motd", OS1_RIGHT_READ));
}
TEST_F(TmpfsContract, LiveEnumerationUsesCookiesAndFaultDoesNotAdvance)
{
    int a = create("a"), b = create("b");
    ASSERT_GE(a, 0);
    ASSERT_GE(b, 0);
    auto first = invoke(client, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_READ);
    os1_directory_entry entry{};
    ASSERT_EQ(sizeof(entry), first.payload_bytes);
    std::memcpy(&entry, first.payload, sizeof(entry));
    EXPECT_STREQ("a", entry.name);
    EXPECT_EQ(0, remove("b"));
    EXPECT_EQ(0, rename("a", "again"));
    auto next = invoke(client, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_READ);
    std::memcpy(&entry, next.payload, sizeof(entry));
    EXPECT_STREQ("again", entry.name);
    EXPECT_EQ(0u, invoke(client, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_READ).payload_bytes);
    EXPECT_EQ(0,
              invoke(client, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_REWIND).application_status);
    os1_directory_request req{1, sizeof(req), 0, 128, 0};
    EXPECT_EQ(OS1_ERR_FAULT, call(directory, OS1_RESOURCE_READ_DIRECTORY, req));
    EXPECT_EQ(0u, table().entries[directory].instance->cursor);
    req.buffer = kBuffer;
    EXPECT_GT(call(directory, OS1_RESOURCE_READ_DIRECTORY, req), 0);
}
TEST_F(TmpfsContract, SparseGrowthTruncateZeroesAndReleasesPages)
{
    int fd = create("file");
    ASSERT_GE(fd, 0);
    int f = bind(fd, OS1_FILE_INTERFACE_ID, kTmpFileRights);
    ASSERT_GE(f, 0);
    os1_seek_request seek{1, sizeof(seek), 4095, OS1_SEEK_SET, 0};
    EXPECT_EQ(0, file_call(f, OS1_FILE_SEEK, &seek, sizeof(seek)));
    EXPECT_EQ(0, file_call(f, OS1_FILE_WRITE, "xy", 2));
    EXPECT_EQ(2u, tmpfs_info(node).data_pages);
    auto* file = static_cast<VfsNode*>(table().entries[fd].instance->object);
    char bytes[3]{1, 1, 1};
    EXPECT_EQ(3, tmpfs_read(file, 4094, bytes, 3));
    EXPECT_EQ(0, bytes[0]);
    EXPECT_EQ('x', bytes[1]);
    EXPECT_EQ('y', bytes[2]);
    os1_file_truncate trunc{4095};
    EXPECT_EQ(0, file_call(f, OS1_FILE_TRUNCATE, &trunc, sizeof(trunc)));
    EXPECT_EQ(1u, tmpfs_info(node).data_pages);
    EXPECT_EQ(4097u, table().entries[fd].instance->cursor);
    trunc.byte_size = OS1_TMPFS_FILE_MAX;
    EXPECT_EQ(0, file_call(f, OS1_FILE_TRUNCATE, &trunc, sizeof(trunc)));
    EXPECT_EQ(2, tmpfs_read(file, 4095, bytes, 2));
    EXPECT_EQ(0, bytes[0]);
    EXPECT_EQ(0, bytes[1]);
    trunc.byte_size++;
    EXPECT_EQ(OS1_ERR_RANGE, file_call(f, OS1_FILE_TRUNCATE, &trunc, sizeof(trunc)));
    seek.offset = INT64_MIN;
    EXPECT_EQ(OS1_ERR_RANGE, file_call(f, OS1_FILE_SEEK, &seek, sizeof(seek)));
    seek.offset = OS1_TMPFS_FILE_MAX;
    EXPECT_EQ(0, file_call(f, OS1_FILE_SEEK, &seek, sizeof(seek)));
    EXPECT_EQ(OS1_ERR_RANGE, file_call(f, OS1_FILE_WRITE, "x", 1));
    EXPECT_EQ(0, file_call(f, OS1_FILE_WRITE));
}
TEST_F(TmpfsContract, AllocationFailureRollsBackCreateAndTwoPageWrite)
{
    host_tmpfs_allocation_budget = 0;
    EXPECT_EQ(OS1_ERR_NO_MEMORY, create("fail"));
    host_tmpfs_allocation_budget = -1;
    EXPECT_EQ(1u, tmpfs_info(node).nodes);
    EXPECT_EQ(1u, tmpfs_info(node).open_instances);
    int fd = create("file");
    ASSERT_GE(fd, 0);
    auto* file = static_cast<VfsNode*>(table().entries[fd].instance->object);
    host_tmpfs_allocation_budget = 1;
    EXPECT_EQ(OS1_ERR_NO_MEMORY, tmpfs_write(file, 4095, "xy", 2));
    host_tmpfs_allocation_budget = -1;
    EXPECT_EQ(0u, tmpfs_info(file).byte_size);
    EXPECT_EQ(0u, tmpfs_info(node).data_pages);
    EXPECT_EQ(2, tmpfs_write(file, 4095, "xy", 2));
    EXPECT_EQ(2u, tmpfs_info(node).data_pages);
}
TEST_F(TmpfsContract, OpenQuotaCountsInstancesNotAliasesAndReleases)
{
    std::vector<OpenInstance*> opens;
    long error = 0;
    for(unsigned i = 1; i < OS1_TMPFS_OPEN_LIMIT; ++i)
    {
        auto* instance = open_instance(node, &error);
        ASSERT_NE(nullptr, instance);
        opens.push_back(instance);
    }
    EXPECT_EQ(nullptr, open_instance(node, &error));
    EXPECT_EQ(OS1_ERR_LIMIT, error);
    EXPECT_EQ(OS1_ERR_LIMIT, create("no-open"));
    EXPECT_EQ(1u, tmpfs_info(node).nodes);
    int alias = duplicate(directory, OS1_RIGHT_LOOKUP);
    ASSERT_GE(alias, 0);
    EXPECT_EQ(OS1_TMPFS_OPEN_LIMIT, tmpfs_info(node).open_instances);
    for(auto* instance : opens)
        instance_release(instance);
    EXPECT_EQ(1u, tmpfs_info(node).open_instances);
    EXPECT_GE(create("yes"), 0);
}
TEST_F(TmpfsContract, NodeQuotaIncludesUnlinkedRetainedFiles)
{
    for(unsigned i = 1; i < OS1_TMPFS_NODE_LIMIT; ++i)
    {
        auto name = std::to_string(i);
        OpenInstance* file = nullptr;
        ASSERT_EQ(0, tmpfs_create(node, name.data(), name.size(), OS1_OBJECT_FILE, file));
        instance_release(file);
    }
    EXPECT_EQ(OS1_ERR_LIMIT, create("full"));
    int held = open("tmp/1", OS1_RIGHT_READ);
    ASSERT_GE(held, 0);
    EXPECT_EQ(0, remove("1"));
    EXPECT_EQ(OS1_ERR_LIMIT, create("full"));
    descriptor_close(table(), held);
    EXPECT_GE(create("room"), 0);
}
TEST_F(TmpfsContract, DataPageQuotaRecoversAfterUnlinkFinalClose)
{
    std::vector<int> files;
    for(unsigned f = 0; f < 4; ++f)
    {
        auto name = std::to_string(f);
        int fd = create(name.c_str());
        ASSERT_GE(fd, 0);
        files.push_back(fd);
        auto* file = static_cast<VfsNode*>(table().entries[fd].instance->object);
        for(unsigned p = 0; p < 16; ++p)
            ASSERT_EQ(1, tmpfs_write(file, p * 4096, "x", 1));
    }
    EXPECT_EQ(OS1_TMPFS_PAGE_LIMIT, tmpfs_info(node).data_pages);
    int extra = create("extra");
    ASSERT_GE(extra, 0);
    auto* file = static_cast<VfsNode*>(table().entries[extra].instance->object);
    EXPECT_EQ(OS1_ERR_LIMIT, tmpfs_write(file, 0, "x", 1));
    EXPECT_EQ(0u, tmpfs_info(file).byte_size);
    EXPECT_EQ(0, remove("0"));
    EXPECT_EQ(OS1_ERR_LIMIT, tmpfs_write(file, 0, "x", 1));
    descriptor_close(table(), files[0]);
    EXPECT_EQ(1, tmpfs_write(file, 0, "x", 1));
    EXPECT_EQ(49u, tmpfs_info(node).data_pages);
}
TEST_F(TmpfsContract, MalformedRecordsAndUnsupportedModesDoNotMutate)
{
    auto a = args("bad");
    a.name[63] = 'x';
    EXPECT_EQ(OS1_ERR_INVALID,
              invoke(client, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_CREATE, &a, sizeof(a))
                  .application_status);
    a = args("../bad");
    EXPECT_EQ(OS1_ERR_INVALID,
              invoke(client, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_CREATE, &a, sizeof(a))
                  .application_status);
    a = args(".");
    EXPECT_EQ(OS1_ERR_INVALID,
              invoke(client, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_CREATE, &a, sizeof(a))
                  .application_status);
    a = args("bad", 99);
    EXPECT_EQ(OS1_ERR_INVALID,
              invoke(client, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_CREATE, &a, sizeof(a))
                  .application_status);
    a = args("exec", OS1_OBJECT_FILE, OS1_RIGHT_EXECUTE);
    EXPECT_EQ(OS1_ERR_ACCESS,
              invoke(client, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_CREATE, &a, sizeof(a))
                  .application_status);
    EXPECT_EQ(OS1_ERR_UNSUPPORTED,
              invoke(client, OS1_DIRECTORY_INTERFACE_ID, 999).application_status);
    EXPECT_EQ(
        OS1_ERR_INVALID,
        invoke(client, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_INFO, "x", 1).application_status);
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, sys_ipc(frames, thread, OS1_IPC_SUBMIT_CAPS, client, 0));
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, sys_ipc(frames, thread, OS1_IPC_SUBMIT_BUFFERS, client, 0));
    EXPECT_EQ(1u, tmpfs_info(node).nodes);
}
TEST_F(TmpfsContract, AcceptedMutationSurvivesHandleCloseAndTimeout)
{
    auto a = args("retained");
    int op = submit(client, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_CREATE, &a, sizeof(a));
    ASSERT_GE(op, 0);
    os1_ipc_deadline deadline{1, sizeof(deadline), 0};
    EXPECT_EQ(OS1_ERR_TIMED_OUT, ipc(OS1_IPC_WAIT_UNTIL, op, deadline));
    descriptor_close(table(), client);
    client = -1;
    descriptor_close(table(), op);
    EXPECT_TRUE(ipc_run_kernel_request());
    EXPECT_EQ(2u, tmpfs_info(node).nodes);
    EXPECT_EQ(1u, tmpfs_info(node).open_instances);
    EXPECT_GE(open("tmp/retained", OS1_RIGHT_READ), 0);
}
TEST_F(TmpfsContract, CompatibilityFaultAndCursorContentionRetainExactInstance)
{
    int fd = create("file");
    ASSERT_GE(fd, 0);
    auto* instance = table().entries[fd].instance;
    ASSERT_EQ(3, tmpfs_write(static_cast<VfsNode*>(instance->object), 0, "abc", 3));
    long value = 0;
    EXPECT_TRUE(read_or_block(frames, thread, fd, 0, 2, value));
    EXPECT_EQ(OS1_ERR_FAULT, value);
    EXPECT_EQ(0u, instance->cursor);
    int f = bind(fd, OS1_FILE_INTERFACE_ID, OS1_RIGHT_READ);
    ASSERT_GE(f, 0);
    instance->cursor_busy = true;
    os1_file_read req{1, 0};
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, file_call(f, OS1_FILE_READ, &req, sizeof(req)));
    EXPECT_TRUE(copy_to_user(frames, thread, kBuffer, "xyz", 3));
    EXPECT_EQ(kVfsBlocked, sys_write({.frames = &frames}, fd, kBuffer, 3));
    descriptor_close(table(), fd);
    EXPECT_EQ(0, remove("file"));
    instance->cursor_busy = false;
    set_current_thread(nullptr);
    wake_vfs_waiters(frames);
    EXPECT_EQ(ThreadState::Ready, thread->state);
    set_current_thread(thread);
    EXPECT_EQ(3u, thread->frame.rax);
    EXPECT_EQ(3u, instance->cursor);
    char bytes[3]{};
    EXPECT_EQ(3, tmpfs_read(static_cast<VfsNode*>(instance->object), 0, bytes, 3));
    EXPECT_EQ(0, std::memcmp(bytes, "xyz", 3));
}
TEST_F(TmpfsContract, ConcurrentExclusiveCreateHasOneWinnerAndNoQuotaLeak)
{
    long status[2]{};
    OpenInstance* instances[2]{};
    std::thread a(
        [&] { status[0] = tmpfs_create(node, "same", 4, OS1_OBJECT_FILE, instances[0]); });
    std::thread b(
        [&] { status[1] = tmpfs_create(node, "same", 4, OS1_OBJECT_FILE, instances[1]); });
    a.join();
    b.join();
    EXPECT_TRUE((status[0] == 0 && status[1] == OS1_ERR_EXISTS) ||
                (status[1] == 0 && status[0] == OS1_ERR_EXISTS));
    for(auto* instance : instances)
        instance_release(instance);
    EXPECT_EQ(2u, tmpfs_info(node).nodes);
    EXPECT_EQ(1u, tmpfs_info(node).open_instances);
}
TEST_F(TmpfsContract, DepthAndNameBoundsRejectBeforePublication)
{
    VfsNode* parent = node;
    for(unsigned depth = 0; depth < 32; ++depth)
    {
        OpenInstance* opened = nullptr;
        ASSERT_EQ(0, tmpfs_create(parent, "d", 1, OS1_OBJECT_DIRECTORY, opened));
        parent = static_cast<VfsNode*>(opened->object);
        instance_release(opened);
    }
    OpenInstance* opened = nullptr;
    EXPECT_EQ(OS1_ERR_LIMIT, tmpfs_create(parent, "x", 1, OS1_OBJECT_FILE, opened));
    EXPECT_EQ(nullptr, opened);
    EXPECT_EQ(33u, tmpfs_info(node).nodes);
    std::string name(63, 'x');
    ASSERT_EQ(0, tmpfs_create(node, name.data(), name.size(), OS1_OBJECT_FILE, opened));
    instance_release(opened);
    name += 'x';
    EXPECT_EQ(OS1_ERR_INVALID,
              tmpfs_create(node, name.data(), name.size(), OS1_OBJECT_FILE, opened));
    EXPECT_EQ(34u, tmpfs_info(node).nodes);
}
TEST_F(TmpfsContract, MalformedFileRecordsAndBoundaryOffsetsPreserveState)
{
    int fd = create("file");
    ASSERT_GE(fd, 0);
    int f = bind(fd, OS1_FILE_INTERFACE_ID, kTmpFileRights);
    ASSERT_GE(f, 0);
    os1_file_read read_request{257, 0};
    EXPECT_EQ(OS1_ERR_INVALID, file_call(f, OS1_FILE_READ, &read_request, sizeof(read_request)));
    read_request = {1, 1};
    EXPECT_EQ(OS1_ERR_INVALID, file_call(f, OS1_FILE_READ, &read_request, sizeof(read_request)));
    EXPECT_EQ(OS1_ERR_INVALID,
              file_call(f, OS1_FILE_READ, &read_request, sizeof(read_request) - 1));
    EXPECT_EQ(OS1_ERR_INVALID, file_call(f, OS1_FILE_TRUNCATE));
    EXPECT_EQ(OS1_ERR_INVALID, file_call(f, OS1_FILE_INFO, "x", 1));
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, file_call(f, 999));
    EXPECT_EQ(0, file_call(f, OS1_FILE_WRITE));
    EXPECT_EQ(0u, table().entries[fd].instance->cursor);
    EXPECT_EQ(OS1_TMPFS_FILE_MAX, seek(fd, OS1_TMPFS_FILE_MAX));
    EXPECT_EQ(OS1_ERR_RANGE, file_call(f, OS1_FILE_WRITE, "x", 1));
    read_request = {1, 0};
    auto r = invoke(f, OS1_FILE_INTERFACE_ID, OS1_FILE_READ, &read_request, sizeof(read_request));
    EXPECT_EQ(0, r.application_status);
    EXPECT_EQ(0u, r.payload_bytes);
    EXPECT_EQ(OS1_ERR_RANGE, seek(fd, INT64_MIN, OS1_SEEK_CUR));
    EXPECT_EQ(OS1_ERR_RANGE, seek(fd, INT64_MAX, OS1_SEEK_CUR));
    EXPECT_EQ(0u,
              tmpfs_info(static_cast<VfsNode*>(table().entries[fd].instance->object)).byte_size);
    EXPECT_EQ(0u, tmpfs_info(node).data_pages);
}
TEST_F(TmpfsContract, CompatibilityPartialCopyFaultCommitsOnlyDeliveredChunks)
{
    int fd = create("file");
    ASSERT_GE(fd, 0);
    std::array<char, 256> data;
    data.fill('x');
    const auto boundary = kRequest + 4096 - data.size();
    ASSERT_TRUE(copy_to_user(frames, thread, boundary, data.data(), data.size()));
    EXPECT_EQ(256, sys_write({.frames = &frames}, fd, boundary, 512));
    EXPECT_EQ(256u, table().entries[fd].instance->cursor);
    auto* file = static_cast<VfsNode*>(table().entries[fd].instance->object);
    ASSERT_EQ(256, tmpfs_write(file, 256, data.data(), data.size()));
    ASSERT_EQ(0, seek(fd, 0));
    EXPECT_EQ(256, read(fd, 512, boundary));
    EXPECT_EQ(256u, table().entries[fd].instance->cursor);
    EXPECT_EQ(256, read(fd, 256));
    EXPECT_EQ(std::string(256, 'x'), bytes(256));
}
TEST_F(TmpfsContract, CancelBlockedWriterReleasesUnlinkedNodeWithoutMutation)
{
    int fd = create("file");
    ASSERT_GE(fd, 0);
    auto* instance = table().entries[fd].instance;
    instance->cursor_busy = true;
    EXPECT_EQ(kVfsBlocked, sys_write({.frames = &frames}, fd, kBuffer, 1));
    EXPECT_EQ(0, descriptor_close(table(), fd));
    EXPECT_EQ(0, remove("file"));
    EXPECT_EQ(2u, tmpfs_info(node).nodes);
    instance->cursor_busy = false;
    cancel_vfs_wait(thread);
    EXPECT_EQ(1u, tmpfs_info(node).nodes);
    EXPECT_EQ(1u, tmpfs_info(node).open_instances);
    EXPECT_EQ(0u, tmpfs_info(node).data_pages);
}
TEST_F(TmpfsContract, MountAllocationFailureLeavesUnpublishedTreeUnchanged)
{
    NativeObject* fresh = nullptr;
    ASSERT_EQ(0, vfs_import_initrd(archive.bytes.data(), archive.bytes.size(), fresh));
    auto* first = static_cast<VfsNode*>(fresh)->children;
    for(int budget : {0, 1})
    {
        host_tmpfs_allocation_budget = budget;
        EXPECT_EQ(OS1_ERR_NO_MEMORY, tmpfs_mount(fresh));
        EXPECT_EQ(first, static_cast<VfsNode*>(fresh)->children);
    }
    host_tmpfs_allocation_budget = -1;
    ASSERT_EQ(0, tmpfs_mount(fresh));
    auto info = tmpfs_info(static_cast<VfsNode*>(fresh)->children);
    EXPECT_EQ(1u, info.nodes);
    EXPECT_EQ(0u, info.open_instances);
    EXPECT_EQ(0u, info.data_pages);
    object_release(fresh);
}
TEST_F(TmpfsContract, NativeAndDescriptorAppendIgnoreCursorAndShareStatus)
{
    int fd = create("append");
    ASSERT_GE(fd, 0);
    int f = bind(fd, OS1_FILE_INTERFACE_ID, OS1_RIGHT_WRITE);
    ASSERT_GE(f, 0);
    EXPECT_EQ(0, file_call(f, OS1_FILE_WRITE, "a", 1));
    EXPECT_EQ(0, seek(fd, 0));
    EXPECT_EQ(0, file_call(f, OS1_FILE_APPEND, "b", 1));
    EXPECT_EQ(2u, table().entries[fd].instance->cursor);
    int alias = duplicate(fd, OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT);
    ASSERT_GE(alias, 0);
    EXPECT_EQ(0, descriptor_set_status(table(), alias, OS1_STATUS_APPEND));
    EXPECT_EQ(0, seek(fd, 0));
    ASSERT_TRUE(copy_to_user(frames, thread, kBuffer, "c", 1));
    EXPECT_EQ(1, sys_write({.frames = &frames}, alias, kBuffer, 1));
    EXPECT_EQ(3u, table().entries[fd].instance->cursor);
    EXPECT_EQ(0, seek(fd, 0));
    EXPECT_EQ(0, file_call(f, OS1_FILE_WRITE, "d", 1));
    int read_only = duplicate(fd, OS1_RIGHT_READ);
    ASSERT_GE(read_only, 0);
    EXPECT_EQ(OS1_ERR_ACCESS, descriptor_set_status(table(), read_only, 0));
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, descriptor_set_status(table(), fd, OS1_STATUS_NONBLOCK));
    char bytes[4]{};
    EXPECT_EQ(4,
              tmpfs_read(static_cast<VfsNode*>(table().entries[fd].instance->object), 0, bytes, 4));
    EXPECT_EQ(0, std::memcmp(bytes, "abcd", 4));
}
TEST_F(TmpfsContract, ConcurrentAppendRecordsDoNotOverlapOrInterleave)
{
    int fd = create("append");
    ASSERT_GE(fd, 0);
    auto* file = static_cast<VfsNode*>(table().entries[fd].instance->object);
    auto writer = [&](char value) {
        char bytes[8];
        std::memset(bytes, value, 8);
        for(unsigned i = 0; i < 32; ++i)
            EXPECT_EQ(8, tmpfs_write(file, 0, bytes, 8, true));
    };
    std::thread a(writer, 'a'), b(writer, 'b');
    a.join();
    b.join();
    char data[512];
    ASSERT_EQ(512, tmpfs_read(file, 0, data, sizeof(data)));
    unsigned count_a = 0, count_b = 0;
    for(unsigned i = 0; i < sizeof(data); i += 8)
    {
        if(data[i] == 'a')
            ++count_a;
        else
        {
            EXPECT_EQ('b', data[i]);
            ++count_b;
        }
        for(unsigned j = 1; j < 8; ++j)
            EXPECT_EQ(data[i], data[i + j]);
    }
    EXPECT_EQ(32u, count_a);
    EXPECT_EQ(32u, count_b);
    EXPECT_EQ(0, tmpfs_truncate(file, OS1_TMPFS_FILE_MAX));
    EXPECT_EQ(OS1_ERR_RANGE, tmpfs_write(file, 0, "x", 1, true));
    EXPECT_EQ(OS1_TMPFS_FILE_MAX, tmpfs_info(file).byte_size);
}
}  // namespace

TEST_F(TmpfsContract, BackendReadFailurePreservesCursorAndReturnsNoPayload)
{
    int fd = create("failure");
    ASSERT_GE(fd, 0);
    auto* instance = table().entries[fd].instance;
    auto* file = static_cast<VfsNode*>(instance->object);
    VfsOperations failing = *file->operations;
    const auto* original = file->operations;
    failing.read = [](VfsNode*, uint64_t, void*, size_t) -> long { return OS1_ERR_IO; };
    file->operations = &failing;
    int endpoint = bind(fd, OS1_FILE_INTERFACE_ID, OS1_RIGHT_READ);
    ASSERT_GE(endpoint, 0);
    os1_file_read request{4, 0};
    auto response =
        invoke(endpoint, OS1_FILE_INTERFACE_ID, OS1_FILE_READ, &request, sizeof(request));
    EXPECT_EQ(response.application_status, OS1_ERR_IO);
    EXPECT_EQ(response.payload_bytes, 0u);
    EXPECT_EQ(instance->cursor, 0u);
    long value = 0;
    EXPECT_TRUE(read_or_block(frames, thread, fd, kBuffer, 4, value));
    EXPECT_EQ(value, OS1_ERR_IO);
    EXPECT_EQ(instance->cursor, 0u);
    file->operations = original;
}
TEST_F(TmpfsContract, SyncValidatesCapturedAuthorityAndEmptyPayload)
{
    int fd = create("sync");
    ASSERT_GE(fd, 0);
    int write = bind(fd, OS1_FILE_INTERFACE_ID, OS1_RIGHT_WRITE);
    int read = bind(fd, OS1_FILE_INTERFACE_ID, OS1_RIGHT_READ);
    ASSERT_GE(write, 0);
    ASSERT_GE(read, 0);
    EXPECT_EQ(file_call(write, OS1_FILE_SYNC), 0);
    EXPECT_EQ(file_call(read, OS1_FILE_SYNC), OS1_ERR_ACCESS);
    EXPECT_EQ(file_call(write, OS1_FILE_SYNC, "x", 1), OS1_ERR_INVALID);
    EXPECT_EQ(invoke(client, OS1_DIRECTORY_INTERFACE_ID, OS1_DIRECTORY_SYNC).application_status, 0);
}
