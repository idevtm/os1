#include "arch/x86_64/cpu/cpu.hpp"
#include "ipc/channel.hpp"
#include "mm/kmem.hpp"
#include "mm/user_copy.hpp"
#include "proc/startup.hpp"
#include "proc/user_program.hpp"
#include "support/initrd_builder.hpp"
#include "support/physical_memory.hpp"
#include "syscall/console_read.hpp"
#include "syscall/resource.hpp"
#include "syscall/vfs.hpp"
#include "vfs/initrd.hpp"
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
