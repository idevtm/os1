#include "arch/x86_64/apic/ipi.hpp"
#include "arch/x86_64/cpu/cpu.hpp"
#include "console/console_input.hpp"
#include "fs/initrd.hpp"
#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "mm/user_copy.hpp"
#include "proc/user_elf.hpp"
#include "proc/user_program.hpp"
#include "resource/descriptor.hpp"
#include "support/lapic_stub.hpp"
#include "support/physical_memory.hpp"
#include "sync/wait_queue.hpp"
#include "syscall/console_read.hpp"
#include "syscall/process.hpp"
#include "syscall/resource.hpp"
#ifdef assert
#undef assert
#endif
#ifdef static_assert
#undef static_assert
#endif
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

void host_console_output_clear();
const std::string& host_console_output();
namespace
{
constexpr uint64_t arena_bytes = 32ull * 1024 * 1024;
constexpr uint32_t input_rights = OS1_RIGHT_READ | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE;
constexpr uint32_t output_rights = OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE;
os1_duplicate_request duplicate(uint32_t rights,
                                int target = -1,
                                uint32_t flags = OS1_DESCRIPTOR_CLOEXEC)
{
    return {OS1_RESOURCE_ABI_VERSION, sizeof(os1_duplicate_request), target, rights, flags, 0};
}
class Resources : public ::testing::Test
{
protected:
    os1::host_test::PhysicalMemoryArena arena{arena_bytes};
    PageFrameContainer frames;
    Process* process = nullptr;
    Thread* thread = nullptr;
    void SetUp() override
    {
        std::array<BootMemoryRegion, 1> regions{{{0, arena_bytes, BootMemoryType::Usable, 0}}};
        ASSERT_TRUE(frames.initialize(regions, 0x300000, kPageFrameBitmapQwordLimit));
        kmem_init(frames);
        ASSERT_TRUE(init_tasks(frames));
        console_input_initialize();
        host_console_output_clear();
        VirtualMemory vm(frames);
        ASSERT_TRUE(vm.allocate_and_map(kUserImageBase, 1, PageFlags::User | PageFlags::Write));
        process = create_user_process("resource-test", vm.root());
        ASSERT_NE(nullptr, process);
        ASSERT_EQ(0, descriptor_bootstrap_console(process->descriptors));
        thread = create_user_thread(process, kUserImageBase, kUserImageBase + 4096, frames, false);
        ASSERT_NE(nullptr, thread);
        set_current_thread(thread);
    }
    void TearDown() override
    {
        bind_initrd_boot_info(nullptr);
        lapic_stub_on_reschedule(nullptr);
        if(thread)
            cancel_console_read(thread);
        if(process)
            descriptor_close_all(process->descriptors);
        set_current_thread(nullptr);
        console_input_initialize();
    }
    DescriptorTable& table()
    {
        return process->descriptors;
    }
    void input(const char* text)
    {
        while(*text)
            console_input_on_keyboard_char(*text++);
    }
    long read(uint64_t fd, size_t size, uint64_t address = kUserImageBase)
    {
        long result = 123;
        EXPECT_TRUE(read_or_block(frames, thread, fd, address, size, result));
        return result;
    }
    std::string copied(size_t size)
    {
        std::string bytes(size, '\0');
        EXPECT_TRUE(copy_from_user(frames, thread, kUserImageBase, bytes.data(), size));
        return bytes;
    }
};
TEST(ResourceAbi, FixedRecords)
{
    EXPECT_EQ(24u, sizeof(os1_duplicate_request));
    EXPECT_EQ(48u, sizeof(os1_resource_info));
    EXPECT_EQ(4u, alignof(os1_resource_info));
    EXPECT_EQ(8u, offsetof(os1_duplicate_request, target));
    EXPECT_EQ(36u, offsetof(os1_resource_info, reserved));
}
TEST_F(Resources, DuplicateSharesStatusButAttenuatesRightsAndKeepsEntryFlagsIndependent)
{
    ASSERT_EQ(3, descriptor_duplicate(table(), 0, duplicate(OS1_RIGHT_READ | OS1_RIGHT_INSPECT)));
    ASSERT_EQ(0, descriptor_set_status(table(), 3, OS1_STATUS_NONBLOCK));
    os1_resource_info original{}, copy{};
    ASSERT_EQ(0, descriptor_query(table(), 0, original));
    ASSERT_EQ(0, descriptor_query(table(), 3, copy));
    EXPECT_EQ(OS1_STATUS_NONBLOCK, original.status_flags);
    EXPECT_EQ(0u, original.descriptor_flags);
    EXPECT_EQ(OS1_DESCRIPTOR_CLOEXEC, copy.descriptor_flags);
    EXPECT_EQ(OS1_ERR_ACCESS, descriptor_duplicate(table(), 3, duplicate(OS1_RIGHT_READ)));
    EXPECT_EQ(OS1_ERR_ACCESS, descriptor_duplicate(table(), 0, duplicate(OS1_RIGHT_WRITE)));
    OpenInstance* instance = nullptr;
    EXPECT_EQ(OS1_ERR_ACCESS, descriptor_acquire(table(), 3, OS1_RIGHT_WRITE, instance));
    EXPECT_EQ(nullptr, instance);
}
TEST_F(Resources, AcceptedReferenceSurvivesCloseAndSlotReuse)
{
    OpenInstance* instance = nullptr;
    ASSERT_EQ(0, descriptor_acquire(table(), 0, OS1_RIGHT_READ, instance));
    InstanceRef pin(instance);
    ASSERT_EQ(0, descriptor_close(table(), 0));
    ASSERT_EQ(0, descriptor_duplicate(table(), 1, duplicate(output_rights, 0)));
    EXPECT_EQ(OS1_INTERFACE_BYTE_INPUT, pin.get()->object->interfaces);
    EXPECT_EQ(1u, pin.get()->references);
    os1_resource_info info{};
    ASSERT_EQ(0, descriptor_query(table(), 0, info));
    EXPECT_EQ(OS1_INTERFACE_BYTE_OUTPUT, info.interfaces);
    EXPECT_EQ(0, descriptor_close(table(), 0));
    EXPECT_EQ(OS1_ERR_BAD_DESCRIPTOR, descriptor_close(table(), 0));
}
TEST_F(Resources, FullTableAndRejectedReplacementLeaveEntriesUnchanged)
{
    for(int fd = 3; fd < OS1_DESCRIPTOR_LIMIT; ++fd)
        ASSERT_EQ(fd, descriptor_duplicate(table(), 1, duplicate(output_rights)));
    EXPECT_EQ(OS1_ERR_LIMIT, descriptor_duplicate(table(), 1, duplicate(output_rights)));
    auto bad = duplicate(input_rights, 2);
    EXPECT_EQ(OS1_ERR_ACCESS, descriptor_duplicate(table(), 1, bad));
    bad = duplicate(output_rights, 2);
    bad.reserved = 1;
    EXPECT_EQ(OS1_ERR_INVALID, descriptor_duplicate(table(), 1, bad));
    bad.reserved = 0;
    bad.size--;
    EXPECT_EQ(OS1_ERR_INVALID, descriptor_duplicate(table(), 1, bad));
    EXPECT_EQ(OS1_ERR_INVALID, descriptor_duplicate(table(), 1, duplicate(output_rights, 1)));
    EXPECT_EQ(1, descriptor_duplicate(table(), 1, duplicate(output_rights, 1, 0)));
    EXPECT_EQ(2, descriptor_duplicate(table(), 1, duplicate(OS1_RIGHT_WRITE, 2)));
    EXPECT_EQ(OS1_ERR_ACCESS, sys_write({.frames = &frames}, 0, 0, 0));
    EXPECT_EQ(0, sys_write({.frames = &frames}, 2, 0, 0));
}
TEST_F(Resources, CloseExecAndExitReleaseOnlyTheirOwnedReferences)
{
    ASSERT_EQ(3, descriptor_duplicate(table(), 1, duplicate(output_rights)));
    ASSERT_EQ(4, descriptor_duplicate(table(), 1, duplicate(output_rights, -1, 0)));
    descriptor_close_exec(table());
    os1_resource_info info{};
    EXPECT_EQ(OS1_ERR_BAD_DESCRIPTOR, descriptor_query(table(), 3, info));
    EXPECT_EQ(0, descriptor_query(table(), 4, info));
    OpenInstance* instance = nullptr;
    ASSERT_EQ(0, descriptor_acquire(table(), 4, OS1_RIGHT_WRITE, instance));
    InstanceRef pin(instance);
    descriptor_close_all(table());
    EXPECT_EQ(1u, instance->references);
    EXPECT_EQ(OS1_ERR_CLOSED, descriptor_duplicate(table(), 4, duplicate(output_rights)));
    descriptor_close_all(table());  // idempotent teardown
    EXPECT_EQ(1u, instance->references);
}
TEST_F(Resources, StandardChildMapPreservesHolesAndRejectsUndelegatableGrants)
{
    DescriptorTable child;
    ASSERT_EQ(3, descriptor_duplicate(table(), 1, duplicate(output_rights)));
    ASSERT_EQ(0, descriptor_close(table(), 0));
    ASSERT_EQ(0, descriptor_set_flags(table(), 1, OS1_DESCRIPTOR_CLOEXEC));
    ASSERT_EQ(0, descriptor_inherit_standard(table(), child));
    os1_resource_info info{};
    EXPECT_EQ(OS1_ERR_BAD_DESCRIPTOR, descriptor_query(child, 0, info));
    EXPECT_EQ(OS1_ERR_BAD_DESCRIPTOR, descriptor_query(child, 3, info));
    ASSERT_EQ(0, descriptor_query(child, 1, info));
    EXPECT_EQ(0u, info.descriptor_flags);
    EXPECT_EQ(output_rights, info.rights);
    descriptor_close_all(child);
    descriptor_table_initialize(child);
    ASSERT_EQ(2, descriptor_duplicate(table(), 1, duplicate(OS1_RIGHT_WRITE, 2)));
    EXPECT_EQ(OS1_ERR_ACCESS, descriptor_inherit_standard(table(), child));
    EXPECT_EQ(OS1_ERR_BAD_DESCRIPTOR, descriptor_query(child, 1, info));
}
TEST_F(Resources, WritesReportProgressAndHaveNoClosedNumberFallback)
{
    const std::string text(128, 'x');
    ASSERT_TRUE(
        copy_to_user(frames, thread, kUserImageBase + 4096 - 128, text.data(), text.size()));
    EXPECT_EQ(128, sys_write({.frames = &frames}, 1, kUserImageBase + 4096 - 128, 256));
    EXPECT_EQ(text, host_console_output());
    EXPECT_EQ(OS1_ERR_FAULT, sys_write({.frames = &frames}, 1, 0x1000, 1));
    EXPECT_EQ(0, sys_write({.frames = &frames}, 1, 0, 0));
    EXPECT_EQ(OS1_ERR_INVALID,
              sys_write({.frames = &frames}, 1, kUserImageBase, OS1_IO_MAX_BYTES + 1));
    ASSERT_EQ(3, descriptor_duplicate(table(), 1, duplicate(output_rights)));
    ASSERT_EQ(0, descriptor_close(table(), 1));
    EXPECT_EQ(OS1_ERR_BAD_DESCRIPTOR, sys_write({.frames = &frames}, 1, 0, 0));
    EXPECT_EQ(128, sys_write({.frames = &frames}, 3, kUserImageBase + 4096 - 128, 128));
}
TEST_F(Resources, ReadFaultDoesNotConsumeAndShortReadKeepsRemainder)
{
    input("hello\n");
    EXPECT_EQ(OS1_ERR_FAULT, read(0, 6, kUserImageBase + 4094));
    EXPECT_TRUE(console_input_has_line());
    EXPECT_EQ(2, read(0, 2));
    EXPECT_EQ("he", copied(2));
    EXPECT_EQ(4, read(0, 8));
    EXPECT_EQ("llo\n", copied(4));
    EXPECT_FALSE(console_input_has_line());
    EXPECT_EQ(0, read(0, 0, 0));
    ASSERT_EQ(0, descriptor_set_status(table(), 0, OS1_STATUS_NONBLOCK));
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, read(0, 2));
    EXPECT_EQ(OS1_ERR_INVALID, read(0, OS1_IO_MAX_BYTES + 1));
    EXPECT_EQ(OS1_ERR_BAD_DESCRIPTOR, read(uint64_t{1} << 32, 0));
}
TEST_F(Resources, WakeDefersUntilOffCpuAndUsesAcceptedInstanceAfterReuse)
{
    long result = 0;
    ASSERT_FALSE(read_or_block(frames, thread, 0, kUserImageBase, 3, result));
    EXPECT_EQ(1u, wait_queue_count(console_input_read_wait_queue()));
    ASSERT_EQ(0, descriptor_close(table(), 0));
    ASSERT_EQ(0, descriptor_duplicate(table(), 1, duplicate(output_rights, 0)));
    input("abc\n");
    wake_console_readers(frames);
    EXPECT_EQ(ThreadState::Blocked, thread->state);
    EXPECT_EQ(1u, wait_queue_count(console_input_read_wait_queue()));
    set_current_thread(nullptr);  // model completion of the outgoing CPU switch
    wake_console_readers(frames);
    EXPECT_EQ(ThreadState::Ready, thread->state);
    EXPECT_EQ(3u, thread->frame.rax);
    EXPECT_EQ("abc", copied(3));
    EXPECT_EQ(0u, thread->io_pins);
    EXPECT_TRUE(wait_queue_empty(console_input_read_wait_queue()));
    EXPECT_TRUE(console_input_has_line());  // newline not discarded
}
TEST_F(Resources, InputBeforeRegistrationCompletesWithoutSleeping)
{
    input("x\n");
    EXPECT_EQ(2, read(0, 2));
    EXPECT_TRUE(wait_queue_empty(console_input_read_wait_queue()));
}
TEST_F(Resources, CancellationDetachesAndReleasesWaitOwnedReference)
{
    OpenInstance* instance = nullptr;
    ASSERT_EQ(0, descriptor_acquire(table(), 0, OS1_RIGHT_READ, instance));
    InstanceRef pin(instance);
    long result = 0;
    ASSERT_FALSE(read_or_block(frames, thread, 0, kUserImageBase, 8, result));
    ASSERT_EQ(0, descriptor_close(table(), 0));
    EXPECT_EQ(2u, instance->references);
    cancel_console_read(thread);
    EXPECT_EQ(1u, instance->references);
    EXPECT_TRUE(wait_queue_empty(console_input_read_wait_queue()));
    EXPECT_EQ(ThreadWaitReason::None, thread->wait.reason);
    input("retained\n");
    set_current_thread(nullptr);
    wake_console_readers(frames);
    EXPECT_TRUE(console_input_has_line());
}
TEST_F(Resources, SyscallValidatesWidthsPointersAndRecordsBeforeMutation)
{
    EXPECT_EQ(OS1_ERR_BAD_DESCRIPTOR,
              sys_resource(frames, thread, uint64_t{1} << 32, OS1_RESOURCE_CLOSE, 0));
    EXPECT_EQ(OS1_ERR_FAULT, sys_resource(frames, thread, 1, OS1_RESOURCE_DUPLICATE, 0x1000));
    EXPECT_EQ(
        OS1_ERR_INVALID,
        sys_resource(frames, thread, 1, OS1_RESOURCE_SET_DESCRIPTOR_FLAGS, uint64_t{1} << 32));
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, sys_resource(frames, thread, 1, 99, 0));
    EXPECT_EQ(OS1_ERR_INVALID, sys_resource(frames, thread, 1, OS1_RESOURCE_CLOSE, 1));
    auto request = duplicate(output_rights);
    ASSERT_TRUE(copy_to_user(frames, thread, kUserImageBase, &request, sizeof(request)));
    EXPECT_EQ(3, sys_resource(frames, thread, 1, OS1_RESOURCE_DUPLICATE, kUserImageBase));
    EXPECT_EQ(0, sys_resource(frames, thread, 3, OS1_RESOURCE_QUERY, kUserImageBase));
    os1_resource_info info{};
    ASSERT_TRUE(copy_from_user(frames, thread, kUserImageBase, &info, sizeof(info)));
    EXPECT_EQ(OS1_RESOURCE_ABI_VERSION, info.version);
    EXPECT_EQ(OS1_DESCRIPTOR_LIMIT, info.descriptor_limit);
    EXPECT_EQ(0u, info.reserved[0] | info.reserved[1] | info.reserved[2]);
}

struct ApScope
{
    cpu ap{};
    cpu* previous = g_cpu_boot->next;
    ApScope()
    {
        ap.self = &ap;
        ap.id = 1;
        ap.booted = 1;
        ap.magic = CPU_MAGIC;
        g_cpu_boot->next = &ap;
    }
    ~ApScope()
    {
        g_cpu_boot->next = previous;
        g_cpu_host_current = g_cpu_boot;
    }
};
struct ImageFixture
{
    BootInfo info{};
    BootModuleInfo module{};
    ImageFixture()
    {
        std::vector<uint8_t> image(0x200);
        auto* header = reinterpret_cast<elf::Elf64Header*>(image.data());
        header->magic = elf::kMagic;
        header->type = elf::kTypeExec;
        header->machine = elf::kMachineX86_64;
        header->entry = kUserImageBase;
        header->phoff = sizeof(*header);
        header->ehsize = sizeof(*header);
        header->phentsize = sizeof(elf::Elf64ProgramHeader);
        header->phnum = 1;
        auto* segment = reinterpret_cast<elf::Elf64ProgramHeader*>(image.data() + header->phoff);
        segment->type = elf::kProgramTypeLoad;
        segment->flags = elf::kProgramFlagExecute;
        segment->offset = 0x100;
        segment->vaddr = kUserImageBase;
        segment->filesz = 4;
        segment->memsz = kPageSize;
        image[0x100] = 0xcc;
        std::vector<uint8_t> archive;
        auto append = [&](const char* name, const std::vector<uint8_t>& bytes, uint32_t mode) {
            const auto name_size = static_cast<uint32_t>(std::char_traits<char>::length(name) + 1);
            archive.insert(archive.end(), {'0', '7', '0', '7', '0', '1'});
            uint32_t fields[] = {
                1, mode, 0, 0, 1, 0, static_cast<uint32_t>(bytes.size()), 0, 0, 0, 0, name_size, 0};
            for(auto field : fields)
            {
                char hex[9];
                std::snprintf(hex, sizeof(hex), "%08x", field);
                archive.insert(archive.end(), hex, hex + 8);
            }
            archive.insert(archive.end(), name, name + name_size);
            while(archive.size() % 4)
                archive.push_back(0);
            archive.insert(archive.end(), bytes.begin(), bytes.end());
            while(archive.size() % 4)
                archive.push_back(0);
        };
        append("bin/test", image, 0100755);
        append("TRAILER!!!", {}, 0);
        // Reserved kernel image range in the host arena, outside allocator pages.
        auto* destination = kernel_physical_pointer<uint8_t>(0x100000);
        for(size_t i = 0; i < archive.size(); ++i)
            destination[i] = archive[i];
        module = {0x100000, archive.size(), "test-initrd"};
        info.modules = &module;
        info.module_count = 1;
        bind_initrd_boot_info(&info);
    }
    ~ImageFixture()
    {
        bind_initrd_boot_info(nullptr);
    }
};
uint64_t published_pid = 0;
PageFrameContainer* publication_frames = nullptr;
TEST_F(Resources, ChildFrameAndGrantsAreCompleteBeforeRemotePublication)
{
    ApScope cpus;
    ImageFixture image;
    ASSERT_TRUE(ipi_initialize());
    published_pid = 0;
    lapic_stub_on_reschedule([](const cpu* target) {
        Thread* child = target->runq.head;
        ASSERT_NE(nullptr, child);
        EXPECT_EQ(kUserImageBase, child->frame.rip);
        EXPECT_EQ(0x23u, child->frame.cs);
        EXPECT_EQ(0x202u, child->frame.rflags);
        os1_resource_info info{};
        ASSERT_EQ(0, descriptor_query(child->process->descriptors, 1, info));
        EXPECT_EQ(output_rights, info.rights);
        published_pid = child->process->pid;
        child->frame.rip += 4;  // AP has consumed and advanced the published frame
        child->frame.rax = 12345;
    });
    Thread* child = load_user_program(frames, thread->address_space_cr3, "/bin/test", process);
    ASSERT_NE(nullptr, child);
    EXPECT_EQ(child->process->pid, published_pid);
    EXPECT_EQ(kUserImageBase + 4, child->frame.rip);
    EXPECT_EQ(12345u, child->frame.rax);
    descriptor_close_all(child->process->descriptors);
}
TEST_F(Resources, SpawnNeverDereferencesChildAfterPublication)
{
    ApScope cpus;
    ImageFixture image;
    ASSERT_TRUE(ipi_initialize());
    constexpr char path[] = "/bin/test";
    ASSERT_TRUE(copy_to_user(frames, thread, kUserImageBase, path, sizeof(path)));
    published_pid = 0;
    publication_frames = &frames;
    lapic_stub_on_reschedule([](const cpu* target) {
        Thread* child = target->runq.head;
        ASSERT_NE(nullptr, child);
        published_pid = child->process->pid;
        cpu* saved = g_cpu_host_current;
        g_cpu_host_current = const_cast<cpu*>(target);
        set_current_thread(child);
        mark_current_thread_dying(0);
        set_current_thread(nullptr);
        g_cpu_host_current = saved;
        reap_dead_threads(*publication_frames);  // poisons/frees the Thread record
    });
    const long result = sys_spawn({.frames = &frames, .kernel_root_cr3 = thread->address_space_cr3},
                                  kUserImageBase);
    EXPECT_EQ(static_cast<long>(published_pid), result);
    EXPECT_NE(0u, published_pid);
}
TEST_F(Resources, ExecFailurePreservesImageAndDescriptorsThenSuccessCommits)
{
    ImageFixture image;
    ASSERT_EQ(3, descriptor_duplicate(table(), 1, duplicate(output_rights)));
    const uint64_t old_root = thread->address_space_cr3;
    constexpr char missing[] = "/missing";
    ASSERT_TRUE(copy_to_user(frames, thread, kUserImageBase, missing, sizeof(missing)));
    const ProcessSyscallContext context{
        .frames = &frames, .kernel_root_cr3 = old_root, .write_cr3 = [](uint64_t) {}};
    EXPECT_LT(sys_exec(context, kUserImageBase), 0);
    EXPECT_EQ(old_root, thread->address_space_cr3);
    os1_resource_info info{};
    EXPECT_EQ(0, descriptor_query(table(), 3, info));
    constexpr char path[] = "/bin/test";
    ASSERT_TRUE(copy_to_user(frames, thread, kUserImageBase, path, sizeof(path)));
    ASSERT_EQ(0, sys_exec(context, kUserImageBase));
    EXPECT_NE(old_root, thread->address_space_cr3);
    EXPECT_EQ(OS1_ERR_BAD_DESCRIPTOR, descriptor_query(table(), 3, info));
    EXPECT_EQ(0, descriptor_query(table(), 1, info));
    EXPECT_EQ(kUserImageBase, thread->frame.rip);
}
TEST_F(Resources, ReaperWaitsForClaimedReadAndCancellationRetiresItsReference)
{
    long result = 0;
    ASSERT_FALSE(read_or_block(frames, thread, 0, kUserImageBase, 8, result));
    OpenInstance* instance = nullptr;
    ASSERT_EQ(0, descriptor_acquire(table(), 0, OS1_RIGHT_READ, instance));
    InstanceRef pin(instance);
    // Model the claim interval; the production queue is detached under its lock.
    EXPECT_EQ(thread, wait_queue_dequeue(console_input_read_wait_queue()));
    thread->io_pins = 1;
    cancel_console_read(thread);
    descriptor_close_all(table());
    EXPECT_EQ(2u, instance->references);
    thread->state = ThreadState::Dying;
    process->state = ProcessState::Dying;
    set_current_thread(nullptr);
    reap_dead_threads(frames);
    EXPECT_EQ(thread, first_thread());
    EXPECT_EQ(1u, thread->io_pins);
    // A claimed waker retires its reference before dropping the pin; use cancel
    // again after the modelled access ends to exercise real reaper cleanup.
    thread->io_pins = 0;
    reap_dead_threads(frames);
    thread = nullptr;
    process = nullptr;
    EXPECT_EQ(1u, instance->references);
    EXPECT_EQ(nullptr, first_thread());
    EXPECT_EQ(nullptr, first_process());
}

TEST_F(Resources, ConcurrentReplacementAndAcquisitionNeverRetargetAcceptedRights)
{
    ASSERT_EQ(4, descriptor_duplicate(table(), 0, duplicate(input_rights, 4)));
    std::atomic<bool> start{false};
    std::atomic<unsigned> failures{0};
    std::thread reader([&] {
        while(!start.load())
            std::this_thread::yield();
        for(unsigned i = 0; i < 10000; ++i)
        {
            OpenInstance* instance = nullptr;
            const long status = descriptor_acquire(table(), 4, OS1_RIGHT_READ, instance);
            if(status == 0)
            {
                if(instance->object->interfaces != OS1_INTERFACE_BYTE_INPUT)
                    ++failures;
                instance_release(instance);
            }
            else if(status != OS1_ERR_ACCESS)
                ++failures;
        }
    });
    start.store(true);
    for(unsigned i = 0; i < 10000; ++i)
    {
        const bool input = i % 2;
        if(descriptor_duplicate(
               table(), input ? 0 : 1, duplicate(input ? input_rights : output_rights, 4)) != 4)
            ++failures;
    }
    reader.join();
    EXPECT_EQ(0u, failures.load());
}
TEST_F(Resources, ConcurrentWakersCompleteOneRegistrationExactlyOnce)
{
    long result = 0;
    ASSERT_FALSE(read_or_block(frames, thread, 0, kUserImageBase, 2, result));
    set_current_thread(nullptr);
    input("a\nb\n");
    std::atomic<bool> start{false};
    auto wake = [&] {
        while(!start.load())
            std::this_thread::yield();
        wake_console_readers(frames);
    };
    std::thread first(wake), second(wake);
    start.store(true);
    first.join();
    second.join();
    EXPECT_EQ(ThreadState::Ready, thread->state);
    EXPECT_EQ(2u, thread->frame.rax);
    EXPECT_EQ("a\n", copied(2));
    EXPECT_EQ(0u, thread->io_pins);
    EXPECT_TRUE(wait_queue_empty(console_input_read_wait_queue()));
    EXPECT_EQ(2, read(0, 2));
    EXPECT_EQ("b\n", copied(2));
}

TEST_F(Resources, BootstrapAllocationFailureRollsBackPartialGrants)
{
    OpenInstance* source = nullptr;
    ASSERT_EQ(0, descriptor_acquire(table(), 0, OS1_RIGHT_READ, source));
    InstanceRef pin(source);
    std::vector<uint64_t> held_pages;
    uint64_t page;
    while(frames.allocate(page))
        held_pages.push_back(page);
    std::vector<OpenInstance*> held_instances;
    while(auto* instance = open_instance(source->object))
        held_instances.push_back(instance);
    ASSERT_FALSE(held_instances.empty());
    instance_release(held_instances.back());
    held_instances.pop_back();  // exactly one slot is available; bootstrap needs three
    const uint32_t before = source->object->references;
    DescriptorTable child;
    EXPECT_EQ(OS1_ERR_NO_MEMORY, descriptor_bootstrap_console(child));
    EXPECT_EQ(before, source->object->references);
    for(const auto& entry : child.entries)
        EXPECT_EQ(nullptr, entry.instance);
    for(auto* instance : held_instances)
        instance_release(instance);
    for(auto physical : held_pages)
        EXPECT_TRUE(frames.free(physical));
}

TEST_F(Resources, RemoteCurrentThreadPreventsPrematureCompletion)
{
    ApScope cpus;
    long result = 0;
    ASSERT_FALSE(read_or_block(frames, thread, 0, kUserImageBase, 2, result));
    set_current_thread(nullptr);
    cpus.ap.current_thread = thread;
    input("x\n");
    wake_console_readers(frames);
    EXPECT_EQ(ThreadState::Blocked, thread->state);
    EXPECT_EQ(1u, wait_queue_count(console_input_read_wait_queue()));
    cpus.ap.current_thread = nullptr;
    wake_console_readers(frames);
    EXPECT_EQ(ThreadState::Ready, thread->state);
    EXPECT_EQ("x\n", copied(2));
}
TEST_F(Resources, FinalObjectDestructionWaitsForAcceptedReferenceAndRunsUnlocked)
{
    struct TrackedObject : NativeObject
    {
        DescriptorTable* table;
        bool destroyed = false;
        bool unlocked = false;
    };
    TrackedObject object{};
    object.references = 1;
    object.allowed_rights = OS1_RIGHT_INSPECT;
    object.table = &table();
    object.destroy = [](NativeObject* base) {
        auto* tracked = static_cast<TrackedObject*>(base);
        tracked->destroyed = true;
        tracked->unlocked = tracked->table->lock.try_lock();
        if(tracked->unlocked)
            tracked->table->lock.unlock();
    };
    auto* instance = open_instance(&object);
    ASSERT_NE(nullptr, instance);
    ASSERT_EQ(4, descriptor_install(table(), instance, OS1_RIGHT_INSPECT, 0, 4));
    instance_release(instance);
    object_release(&object);
    OpenInstance* accepted = nullptr;
    ASSERT_EQ(0, descriptor_acquire(table(), 4, OS1_RIGHT_INSPECT, accepted));
    ASSERT_EQ(0, descriptor_close(table(), 4));
    EXPECT_FALSE(object.destroyed);
    instance_release(accepted);
    EXPECT_TRUE(object.destroyed);
    EXPECT_TRUE(object.unlocked);
}
}  // namespace
