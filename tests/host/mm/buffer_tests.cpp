#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <string_view>
#include <thread>
#include <vector>

#ifdef assert
#undef assert
#endif
#include "arch/x86_64/apic/ipi.hpp"
#include "arch/x86_64/cpu/cpu.hpp"
#include "buffer/buffer.hpp"
#include "handoff/memory_layout.h"
#include "mm/kmem.hpp"
#include "mm/user_copy.hpp"
#include "mm/user_copy_lock.hpp"
#include "proc/thread.hpp"
#include "proc/user_program.hpp"
#include "support/physical_memory.hpp"
#include "syscall/console_read.hpp"
#include "syscall/process.hpp"
#include "syscall/vfs.hpp"
#include "syscall/wait.hpp"

extern void (*host_tlb_retirement_wait)(cpu*);

namespace
{
constexpr uint64_t control = kUserImageBase;
class BufferMemory : public ::testing::Test
{
protected:
    os1::host_test::PhysicalMemoryArena arena{32ull * 1024 * 1024};
    PageFrameContainer frames;
    Process caller, provider;
    Thread a{}, b{};
    void SetUp() override
    {
        std::array<BootMemoryRegion, 1> regions{{{0, arena.size(), BootMemoryType::Usable, 0}}};
        ASSERT_TRUE(frames.initialize(regions, 0x300000, kPageFrameBitmapQwordLimit));
        kmem_init(frames);
        init(caller, a);
        init(provider, b);
    }
    void init(Process& p, Thread& t)
    {
        VirtualMemory vm(frames);
        ASSERT_TRUE(vm.allocate_and_map(control, 1, PageFlags::User | PageFlags::Write));
        p.address_space.cr3 = vm.root();
        t.process = &p;
        t.address_space_cr3 = vm.root();
        descriptor_table_initialize(p.descriptors);
    }
    void TearDown() override
    {
        buffer_process_cleanup(frames, &caller);
        buffer_process_cleanup(frames, &provider);
        descriptor_close_all(caller.descriptors);
        descriptor_close_all(provider.descriptors);
    }
    template<class T>
    long call(Thread& t, int fd, uint64_t op, T& value)
    {
        EXPECT_TRUE(copy_to_user(frames, &t, control, &value, sizeof(value)));
        const long status = sys_buffer(frames, &t, fd, op, control);
        EXPECT_TRUE(copy_from_user(frames, &t, control, &value, sizeof(value)));
        return status;
    }
    long create(Thread& t, uint64_t bytes = 4096)
    {
        os1_buffer_create request{1, sizeof(request), bytes, OS1_BUFFER_EXCLUSIVE, 0};
        return call(t, 0, OS1_RESOURCE_BUFFER_CREATE, request);
    }
    uint64_t map(Thread& t, int fd, bool writable = true)
    {
        os1_buffer_map request{1, sizeof(request), 0, writable ? 1u : 0u, 0};
        EXPECT_EQ(0, call(t, fd, OS1_RESOURCE_BUFFER_MAP, request));
        return request.address;
    }
    os1_buffer_info info(Thread& t, int fd)
    {
        os1_buffer_info result{1, sizeof(result)};
        EXPECT_EQ(0, call(t, fd, OS1_RESOURCE_BUFFER_INFO, result));
        return result;
    }
    os1_ipc_buffers attachment(int fd, uint32_t disposition = OS1_BUFFER_LOAN)
    {
        os1_ipc_buffers out{};
        out.count = 1;
        out.entries[0] = {fd, disposition};
        return out;
    }
    bool readable(Thread& t, uint64_t address)
    {
        uint8_t value = 0;
        return copy_from_user(frames, &t, address, &value, 1);
    }
};
TEST(BufferAbi, FixedRecords)
{
    EXPECT_EQ(24u, sizeof(os1_buffer_create));
    EXPECT_EQ(40u, sizeof(os1_buffer_info));
    EXPECT_EQ(24u, sizeof(os1_buffer_map));
    EXPECT_EQ(8u, sizeof(os1_ipc_buffer));
    EXPECT_EQ(40u, sizeof(os1_ipc_buffers));
}
TEST_F(BufferMemory, CreationBoundsZeroPaddingAndNonExecutableMaps)
{
    EXPECT_EQ(OS1_ERR_INVALID, create(a, 0));
    EXPECT_EQ(OS1_ERR_INVALID, create(a, OS1_BUFFER_BYTES_MAX + 1));
    EXPECT_EQ(OS1_ERR_INVALID, create(a, UINT64_MAX));
    const int fd = create(a, 1);
    ASSERT_GE(fd, 0);
    const auto address = map(a, fd);
    std::array<uint8_t, 4096> data;
    data.fill(0xff);
    ASSERT_TRUE(copy_from_user(frames, &a, address, data.data(), data.size()));
    for(auto value : data)
        EXPECT_EQ(0, value);
    VirtualMemory vm(frames, a.address_space_cr3);
    uint64_t physical = 0, flags = 0;
    ASSERT_TRUE(vm.translate(address, physical, flags));
    EXPECT_NE(0u, flags & static_cast<uint64_t>(PageFlags::NoExecute));
    EXPECT_NE(0u, flags & static_cast<uint64_t>(PageFlags::SharedBacking));
    os1_duplicate_request dup{1, sizeof(dup), -1, OS1_RIGHT_EXECUTE, 0, 0};
    EXPECT_EQ(OS1_ERR_ACCESS, descriptor_duplicate(caller.descriptors, fd, dup));
}
TEST_F(BufferMemory, AllAliasesRevokeRestoreAndOldBorrowerCannotMap)
{
    const int fd = create(a);
    const auto x = map(a, fd), y = map(a, fd);
    const auto identity = info(a, fd).identity;
    VirtualMemory origin_vm(frames, a.address_space_cr3);
    uint64_t original_physical = 0, flags = 0;
    ASSERT_TRUE(origin_vm.translate(x, original_physical, flags));
    BufferLoanBatch* loans = nullptr;
    ASSERT_EQ(0, buffer_prepare_loans(frames, &a, attachment(fd), loans));
    EXPECT_FALSE(readable(a, x));
    EXPECT_FALSE(readable(a, y));
    EXPECT_EQ(0u, info(a, fd).owned);
    os1_ipc_buffers received{};
    ASSERT_EQ(0, buffer_receive_loans(loans, &b, received));
    const auto z = map(b, received.entries[0].fd);
    VirtualMemory borrower_vm(frames, b.address_space_cr3);
    uint64_t borrowed_physical = 0;
    ASSERT_TRUE(borrower_vm.translate(z, borrowed_physical, flags));
    EXPECT_EQ(original_physical, borrowed_physical);
    uint8_t value = 0x4a;
    ASSERT_TRUE(copy_to_user(frames, &b, z, &value, 1));
    buffer_finish_loans(loans);
    EXPECT_FALSE(readable(b, z));
    EXPECT_TRUE(readable(a, x));
    EXPECT_TRUE(readable(a, y));
    uint64_t restored_physical = 0;
    ASSERT_TRUE(origin_vm.translate(x, restored_physical, flags));
    EXPECT_EQ(original_physical, restored_physical);
    uint8_t actual = 0;
    ASSERT_TRUE(copy_from_user(frames, &a, x, &actual, 1));
    EXPECT_EQ(value, actual);
    EXPECT_EQ(identity, info(a, fd).identity);
    EXPECT_EQ(0u, info(b, received.entries[0].fd).owned);
    buffer_release_loans(loans);
    ASSERT_EQ(0, buffer_prepare_loans(frames, &a, attachment(fd), loans));
    os1_ipc_buffers again{};
    ASSERT_EQ(0, buffer_receive_loans(loans, &b, again));
    os1_buffer_map stale{1, sizeof(stale), 0, 1, 0};
    EXPECT_EQ(OS1_ERR_ACCESS, call(b, received.entries[0].fd, OS1_RESOURCE_BUFFER_MAP, stale));
    buffer_cancel_loans(loans);
}
TEST_F(BufferMemory, InvalidBatchLeavesEveryAliasOwned)
{
    int fd = create(a);
    const auto address = map(a, fd);
    auto input = attachment(fd);
    input.count = 2;
    input.entries[1] = input.entries[0];
    BufferLoanBatch* loans = nullptr;
    EXPECT_EQ(OS1_ERR_INVALID, buffer_prepare_loans(frames, &a, input, loans));
    EXPECT_EQ(nullptr, loans);
    EXPECT_TRUE(readable(a, address));
    EXPECT_EQ(1u, info(a, fd).owned);
    input = attachment(fd, OS1_BUFFER_MOVE);
    EXPECT_EQ(OS1_ERR_INVALID, buffer_prepare_loans(frames, &a, input, loans));
}
TEST_F(BufferMemory, FailedLoanRestoreReleasesReservationWhenPageTablesAreMissing)
{
    const int fd = create(a, 2 * kPageSize);
    ASSERT_GE(fd, 0);
    const auto address = map(a, fd);
    BufferLoanBatch* loans = nullptr;
    ASSERT_EQ(buffer_prepare_loans(frames, &a, attachment(fd), loans), 0);

    // Force a missing reservation table without advancing the address-space generation.
    VirtualMemory vm(frames, a.address_space_cr3);
    ASSERT_EQ(address >> 39, control >> 39);
    ASSERT_TRUE(vm.destroy_user_slot(control >> 39));
    ASSERT_TRUE(vm.allocate_and_map(control, 1, PageFlags::User | PageFlags::Write));
    std::vector<uint64_t> held;
    while(frames.free_page_count())
    {
        uint64_t page = 0;
        ASSERT_TRUE(frames.allocate(page));
        held.push_back(page);
    }

    buffer_finish_loans(loans);
    EXPECT_EQ(frames.free_page_count(), 0u);
    EXPECT_FALSE(readable(a, address));
    EXPECT_FALSE(readable(a, address + kPageSize));
    EXPECT_EQ(info(a, fd).owned, 1u);
    buffer_release_loans(loans);
    for(auto page : held)
        frames.free(page);
    EXPECT_EQ(map(a, fd), address);
}

TEST_F(BufferMemory, FailedLoanRestoreRemovesPartialAliasWithoutOverwritingConflict)
{
    const int fd = create(a, 2 * kPageSize);
    ASSERT_GE(fd, 0);
    const auto address = map(a, fd);
    const uint8_t buffer_value = 0x71;
    ASSERT_TRUE(copy_to_user(frames, &a, address, &buffer_value, 1));
    BufferLoanBatch* loans = nullptr;
    ASSERT_EQ(buffer_prepare_loans(frames, &a, attachment(fd), loans), 0);

    // Force a conflict on the second page after the first page can be restored.
    VirtualMemory vm(frames, a.address_space_cr3);
    const auto conflict = address + kPageSize;
    ASSERT_TRUE(vm.allocate_and_map(conflict, 1, PageFlags::User | PageFlags::Write));
    const uint8_t conflict_value = 0xa5;
    ASSERT_TRUE(copy_to_user(frames, &a, conflict, &conflict_value, 1));

    buffer_finish_loans(loans);
    EXPECT_FALSE(readable(a, address));
    uint8_t actual = 0;
    ASSERT_TRUE(copy_from_user(frames, &a, conflict, &actual, 1));
    EXPECT_EQ(actual, conflict_value);
    EXPECT_EQ(info(a, fd).owned, 1u);
    buffer_release_loans(loans);
    ASSERT_TRUE(vm.free(conflict, 1));
    ASSERT_EQ(map(a, fd), address);
    ASSERT_TRUE(copy_from_user(frames, &a, address, &actual, 1));
    EXPECT_EQ(actual, buffer_value);
}

TEST_F(BufferMemory, ExplicitUnmapDiscardsReservationAndDescriptorCloseKeepsMap)
{
    int fd = create(a);
    const auto address = map(a, fd);
    BufferLoanBatch* loans = nullptr;
    ASSERT_EQ(0, buffer_prepare_loans(frames, &a, attachment(fd), loans));
    os1_buffer_map unmap{1, sizeof(unmap), address, 0, 0};
    ASSERT_EQ(0, call(a, fd, OS1_RESOURCE_BUFFER_UNMAP, unmap));
    buffer_cancel_loans(loans);
    EXPECT_FALSE(readable(a, address));
    const auto remapped = map(a, fd);
    ASSERT_EQ(0, descriptor_close(caller.descriptors, fd));
    EXPECT_TRUE(readable(a, remapped));
}
TEST_F(BufferMemory, ExecDiscardsOldReservations)
{
    int fd = create(a);
    auto address = map(a, fd);
    BufferLoanBatch* loans = nullptr;
    ASSERT_EQ(0, buffer_prepare_loans(frames, &a, attachment(fd), loans));
    buffer_process_exec(frames, &caller);
    buffer_cancel_loans(loans);
    EXPECT_FALSE(readable(a, address));
    EXPECT_EQ(1u, info(a, fd).owned);
}
TEST_F(BufferMemory, MappingQuotaIncludesReservations)
{
    int fd = create(a);
    for(unsigned i = 0; i < OS1_BUFFER_OBJECT_MAP_LIMIT; ++i)
        map(a, fd);
    os1_buffer_map fifth{1, sizeof(fifth), 0, 1, 0};
    EXPECT_EQ(OS1_ERR_LIMIT, call(a, fd, OS1_RESOURCE_BUFFER_MAP, fifth));
    BufferLoanBatch* loans = nullptr;
    ASSERT_EQ(0, buffer_prepare_loans(frames, &a, attachment(fd), loans));
    os1_ipc_buffers received{};
    ASSERT_EQ(0, buffer_receive_loans(loans, &b, received));
    EXPECT_EQ(OS1_ERR_LIMIT, call(b, received.entries[0].fd, OS1_RESOURCE_BUFFER_MAP, fifth));
    buffer_cancel_loans(loans);
}
TEST_F(BufferMemory, ObjectAndGlobalPageBudgetsRemainCharged)
{
    std::array<int, 8> descriptors{};
    for(auto& fd : descriptors)
    {
        fd = create(a, OS1_BUFFER_BYTES_MAX);
        ASSERT_GE(fd, 0);
    }
    EXPECT_EQ(OS1_ERR_LIMIT, create(a, 1));
    EXPECT_EQ(OS1_ERR_LIMIT, create(b, 1));
    ASSERT_EQ(0, descriptor_close(caller.descriptors, descriptors[0]));
    EXPECT_GE(create(b, 1), 0);
}
TEST_F(BufferMemory, DistinctOutputRetryOneClaimAndOriginReservationRemoval)
{
    int input = create(a), output = create(b);
    const auto source_address = map(b, output);
    BufferLoanBatch* loans = nullptr;
    ASSERT_EQ(0, buffer_prepare_loans(frames, &a, attachment(input), loans));
    os1_ipc_buffers received{};
    ASSERT_EQ(0, buffer_receive_loans(loans, &b, received));
    BufferOutputBatch* outputs = nullptr;
    ASSERT_EQ(
        0, buffer_prepare_outputs(frames, &b, loans, attachment(output, OS1_BUFFER_MOVE), outputs));
    EXPECT_FALSE(readable(b, source_address));
    buffer_finish_loans(loans);
    os1_ipc_buffers claimed{};
    ASSERT_EQ(0, buffer_claim_outputs(outputs, &a, claimed));
    const auto claimed_address = map(a, claimed.entries[0].fd);
    buffer_rollback_claim(outputs, &a, claimed);
    EXPECT_FALSE(readable(a, claimed_address));
    ASSERT_EQ(0, buffer_claim_outputs(outputs, &a, claimed));
    buffer_commit_claim(outputs);
    os1_ipc_buffers repeated{};
    ASSERT_EQ(0, buffer_claim_outputs(outputs, &a, repeated));
    EXPECT_EQ(0u, repeated.count);
    EXPECT_EQ(1u, info(a, claimed.entries[0].fd).owned);
    EXPECT_EQ(0u, info(b, output).owned);
    buffer_release_outputs(outputs);
    buffer_release_loans(loans);
}
TEST_F(BufferMemory, ReceiveDescriptorExhaustionPreservesEscrow)
{
    int fd = create(a);
    const auto address = map(a, fd);
    BufferLoanBatch* loans = nullptr;
    ASSERT_EQ(0, buffer_prepare_loans(frames, &a, attachment(fd), loans));
    int seed = create(b);
    ASSERT_GE(seed, 0);
    os1_duplicate_request dup{1, sizeof(dup), -1, OS1_RIGHT_READ, 0, 0};
    std::vector<int> aliases;
    for(;;)
    {
        auto next = descriptor_duplicate(provider.descriptors, seed, dup);
        if(next < 0)
            break;
        aliases.push_back(next);
    }
    os1_ipc_buffers received{};
    EXPECT_EQ(OS1_ERR_LIMIT, buffer_receive_loans(loans, &b, received));
    EXPECT_EQ(0u, received.count);
    EXPECT_FALSE(readable(a, address));
    ASSERT_EQ(0, descriptor_close(provider.descriptors, aliases.back()));
    ASSERT_EQ(0, buffer_receive_loans(loans, &b, received));
    buffer_cancel_loans(loans);
    EXPECT_TRUE(readable(a, address));
}
TEST_F(BufferMemory, AdmittedKernelCopyGateDelaysRevocation)
{
    int fd = create(a);
    const auto address = map(a, fd);
    std::atomic<bool> started = false, done = false;
    BufferLoanBatch* loans = nullptr;
    std::thread worker;
    {
        UserCopyGuard admitted(&caller);
        worker = std::thread([&] {
            started = true;
            EXPECT_EQ(0, buffer_prepare_loans(frames, &a, attachment(fd), loans));
            done = true;
        });
        while(!started.load())
            std::this_thread::yield();
        for(unsigned i = 0; i < 1000; ++i)
            std::this_thread::yield();
        EXPECT_FALSE(done.load());
        VirtualMemory vm(frames, a.address_space_cr3);
        uint64_t physical = 0, flags = 0;
        EXPECT_TRUE(vm.translate(address, physical, flags));
    }
    worker.join();
    EXPECT_TRUE(done);
    EXPECT_FALSE(readable(a, address));
    buffer_cancel_loans(loans);
}
TEST_F(BufferMemory, AnUnrelatedAddressSpaceCanCopyBeforeRetirementAcknowledgment)
{
    const int fd = create(a);
    ASSERT_GE(fd, 0);
    const auto address = map(a, fd);
    ASSERT_TRUE(readable(a, address));
    ASSERT_TRUE(ipi_initialize());

    cpu target{};
    target.id = 209;
    target.booted = static_cast<uint32_t>(ApStartupState::Online);
    target.next = g_cpu_boot->next;
    g_cpu_boot->next = &target;
    auto previous_wait = host_tlb_retirement_wait;

    std::atomic<bool> done = false;
    std::thread remote;
    auto check_and_acknowledge = [&](cpu* waiting_for) {
        if(waiting_for == &target)
        {
            EXPECT_TRUE(caller.address_space.copy_gate.locked());
            EXPECT_FALSE(provider.address_space.copy_gate.locked());
            remote = std::thread([&] {
                EXPECT_TRUE(readable(b, control));
                done.store(true, std::memory_order_release);
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while(!done.load(std::memory_order_acquire) &&
                  std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            EXPECT_TRUE(done.load(std::memory_order_acquire));
        }
        auto* previous = g_cpu_host_current;
        g_cpu_host_current = waiting_for;
        ipi_poll_tlb_shootdown();
        g_cpu_host_current = previous;
    };
    static decltype(check_and_acknowledge)* active_callback;
    active_callback = &check_and_acknowledge;
    host_tlb_retirement_wait = [](cpu* waiting_for) { (*active_callback)(waiting_for); };
    BufferLoanBatch* loans = nullptr;
    EXPECT_EQ(0, buffer_prepare_loans(frames, &a, attachment(fd), loans));
    host_tlb_retirement_wait = previous_wait;
    active_callback = nullptr;
    g_cpu_boot->next = target.next;
    ASSERT_TRUE(remote.joinable());
    remote.join();
    EXPECT_FALSE(readable(a, address));
    buffer_cancel_loans(loans);
}

TEST_F(BufferMemory, ProcessRevocationRetiresOnlyPresentMatchingAliases)
{
    const int mapped = create(a);
    const int unmapped = create(a);
    const int unrelated = create(b);
    ASSERT_GE(mapped, 0);
    ASSERT_GE(unmapped, 0);
    ASSERT_GE(unrelated, 0);
    const auto first = map(a, mapped);
    const auto second = map(a, mapped);
    const auto other = map(b, unrelated);
    ASSERT_NE(first, second);
    ASSERT_TRUE(readable(a, first));
    ASSERT_TRUE(readable(a, second));
    ASSERT_TRUE(readable(b, other));
    ASSERT_TRUE(ipi_initialize());

    constexpr uint8_t target_id = 210;
    cpu target{};
    target.id = target_id;
    target.booted = static_cast<uint32_t>(ApStartupState::Online);
    target.next = g_cpu_boot->next;
    g_cpu_boot->next = &target;
    auto previous_wait = host_tlb_retirement_wait;
    static unsigned acknowledgments;
    acknowledgments = 0;
    host_tlb_retirement_wait = [](cpu* waiting_for) {
        if(waiting_for->id == target_id)
            ++acknowledgments;
        auto* previous = g_cpu_host_current;
        g_cpu_host_current = waiting_for;
        ipi_poll_tlb_shootdown();
        g_cpu_host_current = previous;
    };

    buffer_process_exec(frames, &caller);
    EXPECT_EQ(1u, acknowledgments);
    EXPECT_FALSE(readable(a, first));
    EXPECT_FALSE(readable(a, second));
    EXPECT_TRUE(readable(b, other));

    buffer_process_cleanup(frames, &caller);
    EXPECT_EQ(1u, acknowledgments);
    buffer_process_cleanup(frames, &provider);
    EXPECT_EQ(2u, acknowledgments);
    EXPECT_FALSE(readable(b, other));

    host_tlb_retirement_wait = previous_wait;
    g_cpu_boot->next = target.next;
}

TEST_F(BufferMemory, StringCopiesStopAtNulBeforeCheckingTheNextPage)
{
    const uint64_t end = control + kPageSize;
    char destination[8] = {'?', '?', '?', '?', '?', '?', '?', '?'};
    const char final_bytes[] = {'a', 'b', '\0'};
    ASSERT_TRUE(
        copy_to_user(frames, &a, end - sizeof(final_bytes), final_bytes, sizeof(final_bytes)));
    EXPECT_TRUE(
        copy_user_string(frames, &a, end - sizeof(final_bytes), destination, sizeof(destination)));
    EXPECT_EQ(std::string_view("ab\0", 3), std::string_view(destination, 3));
    EXPECT_EQ('?', destination[3]);

    const char unterminated = 'c';
    ASSERT_TRUE(copy_to_user(frames, &a, end - 1, &unterminated, 1));
    EXPECT_FALSE(
        copy_user_string(frames, &a, end - sizeof(final_bytes), destination, sizeof(destination)));
    EXPECT_EQ('c', destination[2]);
    EXPECT_EQ('?', destination[3]);

    VirtualMemory vm(frames, a.address_space_cr3);
    ASSERT_TRUE(vm.allocate_and_map(end, 1, PageFlags::User | PageFlags::Write));
    const char suffix[] = {'d', '\0'};
    ASSERT_TRUE(copy_to_user(frames, &a, end, suffix, sizeof(suffix)));
    EXPECT_TRUE(
        copy_user_string(frames, &a, end - sizeof(final_bytes), destination, sizeof(destination)));
    EXPECT_EQ(std::string_view("abcd\0", 5), std::string_view(destination, 5));

    char small[3]{};
    EXPECT_FALSE(copy_user_string(frames, &a, end - sizeof(final_bytes), small, sizeof(small)));
    EXPECT_EQ(std::string_view("ab\0", 3), std::string_view(small, sizeof(small)));
}

TEST_F(BufferMemory, SameProcessLoanKeepsOriginalReservations)
{
    int fd = create(a);
    const auto original = map(a, fd);
    BufferLoanBatch* loans = nullptr;
    ASSERT_EQ(0, buffer_prepare_loans(frames, &a, attachment(fd), loans));
    os1_ipc_buffers received{};
    ASSERT_EQ(0, buffer_receive_loans(loans, &a, received));
    const auto borrowed = map(a, received.entries[0].fd);
    buffer_finish_loans(loans);
    EXPECT_TRUE(readable(a, original));
    EXPECT_FALSE(readable(a, borrowed));
    buffer_release_loans(loans);
}
}  // namespace

namespace
{
TEST_F(BufferMemory, PartialBackingAllocationFailureRestoresPageAndObjectBudgets)
{
    int warm = create(a);
    ASSERT_GE(warm, 0);
    ASSERT_EQ(0, descriptor_close(caller.descriptors, warm));
    std::vector<uint64_t> held;
    while(frames.free_page_count() > 1)
    {
        uint64_t page = 0;
        ASSERT_TRUE(frames.allocate(page));
        held.push_back(page);
    }
    const auto before = frames.free_page_count();
    EXPECT_EQ(OS1_ERR_NO_MEMORY, create(a, 8192));
    EXPECT_EQ(before, frames.free_page_count());
    for(auto page : held)
        frames.free(page);
    for(unsigned i = 0; i < OS1_BUFFER_PROCESS_OBJECT_LIMIT; ++i)
        EXPECT_GE(create(a), 0);
    EXPECT_EQ(OS1_ERR_LIMIT, create(a));
}
}  // namespace

namespace
{
TEST_F(BufferMemory, GenericIoCannotChangeBorrowedAuthorityGeneration)
{
    const int fd = create(a);
    BufferLoanBatch* loans = nullptr;
    ASSERT_EQ(0, buffer_prepare_loans(frames, &a, attachment(fd), loans));
    os1_ipc_buffers received{};
    ASSERT_EQ(0, buffer_receive_loans(loans, &b, received));
    const int borrowed = received.entries[0].fd;
    OpenInstance* instance = nullptr;
    ASSERT_EQ(0, descriptor_acquire(provider.descriptors, borrowed, OS1_RIGHT_READ, instance));
    const auto generation = instance->cursor;
    ASSERT_NE(0u, generation);
    EXPECT_EQ(OS1_ERR_ACCESS, sys_vfs(frames, &b, borrowed, OS1_RESOURCE_SEEK, control));
    EXPECT_EQ(OS1_ERR_ACCESS, sys_vfs(frames, &b, borrowed, OS1_RESOURCE_READ_DIRECTORY, control));
    long result = 0;
    EXPECT_TRUE(read_or_block(frames, &b, borrowed, control, 1, result));
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, result);
    auto* saved = current_thread();
    set_current_thread(&b);
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, sys_write({&frames, 0, nullptr}, borrowed, control, 1));
    set_current_thread(saved);
    EXPECT_EQ(generation, instance->cursor);
    instance_release(instance);
    buffer_cancel_loans(loans);
}
}  // namespace

namespace
{
TEST_F(BufferMemory, ZombieExitReleasesReservationsBeforeParentWaitpid)
{
    ASSERT_TRUE(init_tasks(frames));
    VirtualMemory child_vm(frames);
    ASSERT_TRUE(child_vm.allocate_and_map(control, 1, PageFlags::User | PageFlags::Write));
    const auto child_root = child_vm.root();
    auto* child = create_user_process("buffer-zombie", child_root);
    ASSERT_NE(nullptr, child);
    child->parent = &caller;
    auto* child_thread = create_user_thread(child, control, control + kPageSize, frames, false);
    ASSERT_NE(nullptr, child_thread);
    const auto child_pid = child->pid;
    const int fd = create(*child_thread, OS1_BUFFER_BYTES_MAX);
    ASSERT_GE(fd, 0);
    ASSERT_NE(0u, map(*child_thread, fd));
    BufferLoanBatch* loans = nullptr;
    ASSERT_EQ(0, buffer_prepare_loans(frames, child_thread, attachment(fd), loans));
    os1_ipc_buffers received{};
    ASSERT_EQ(0, buffer_receive_loans(loans, &b, received));
    const auto borrowed = map(b, received.entries[0].fd);
    set_current_thread(child_thread);
    mark_current_thread_dying(7);
    set_current_thread(nullptr);
    reap_dead_threads(frames);
    EXPECT_EQ(ProcessState::Zombie, child->state);
    EXPECT_FALSE(process_has_threads(child));
    EXPECT_TRUE(child->exit_cleanup_complete);
    ASSERT_EQ(0u, child->address_space.cr3);
    EXPECT_FALSE(frames.free(child_root));
    buffer_finish_loans(loans);
    EXPECT_FALSE(readable(b, borrowed));
    buffer_release_loans(loans);
    ASSERT_EQ(0, descriptor_close(provider.descriptors, received.entries[0].fd));
    // The dead child's backing charge must be gone before parent waitpid.
    for(unsigned i = 0; i < OS1_BUFFER_PROCESS_OBJECT_LIMIT; ++i)
        EXPECT_GE(create(b, OS1_BUFFER_BYTES_MAX), 0);
    long status = 0;
    EXPECT_TRUE(try_complete_wait_pid(frames, &a, child_pid, 0, status));
    EXPECT_EQ(static_cast<long>(child_pid), status);
}

TEST_F(BufferMemory, ExecReplacesAndFreesCr3BeforeLoanFinishes)
{
    const int fd = create(a);
    const auto origin = map(a, fd);
    BufferLoanBatch* loans = nullptr;
    ASSERT_EQ(0, buffer_prepare_loans(frames, &a, attachment(fd), loans));
    os1_ipc_buffers received{};
    ASSERT_EQ(0, buffer_receive_loans(loans, &b, received));
    const auto borrowed = map(b, received.entries[0].fd);
    uint8_t value = 0x37;
    ASSERT_TRUE(copy_to_user(frames, &b, borrowed, &value, 1));
    const auto old_cr3 = a.address_space_cr3;
    VirtualMemory replacement(frames);
    ASSERT_TRUE(replacement.allocate_and_map(control, 1, PageFlags::User | PageFlags::Write));
    buffer_process_exec(frames, &caller);
    descriptor_close_exec(caller.descriptors);
    a.address_space_cr3 = replacement.root();
    caller.address_space.cr3 = replacement.root();
    ASSERT_TRUE(destroy_user_address_space(frames, old_cr3));
    // Reuse the freed root as data, so stale restoration cannot pass silently.
    std::vector<uint64_t> reclaimed;
    for(;;)
    {
        uint64_t page = 0;
        ASSERT_TRUE(frames.allocate(page));
        reclaimed.push_back(page);
        if(page == old_cr3)
            break;
    }
    auto* old_root = kernel_physical_pointer<uint64_t>(old_cr3);
    for(unsigned i = 0; i < kPageSize / sizeof(uint64_t); ++i)
        old_root[i] = 0;
    const auto before_finish = frames.free_page_count();
    buffer_finish_loans(loans);
    EXPECT_EQ(before_finish, frames.free_page_count());
    for(unsigned i = 0; i < kPageSize / sizeof(uint64_t); ++i)
        EXPECT_EQ(0u, old_root[i]);
    uint64_t physical = 0, flags = 0;
    EXPECT_FALSE(replacement.translate(origin, physical, flags));
    EXPECT_FALSE(readable(b, borrowed));
    buffer_release_loans(loans);
    ASSERT_EQ(0, descriptor_close(provider.descriptors, received.entries[0].fd));
    for(auto page : reclaimed)
        frames.free(page);
    EXPECT_GE(create(a), 0);
}

TEST_F(BufferMemory, RepeatedConsumedCommitDoesNotDiscardNewLoanReservations)
{
    const int output = create(a);
    BufferLoanBatch* first = nullptr;
    ASSERT_EQ(0, buffer_prepare_loans(frames, &a, {}, first));
    os1_ipc_buffers received{};
    ASSERT_EQ(0, buffer_receive_loans(first, &a, received));
    BufferOutputBatch* outputs = nullptr;
    ASSERT_EQ(
        0, buffer_prepare_outputs(frames, &a, first, attachment(output, OS1_BUFFER_MOVE), outputs));
    buffer_finish_loans(first);
    os1_ipc_buffers claimed{};
    ASSERT_EQ(0, buffer_claim_outputs(outputs, &a, claimed));
    buffer_commit_claim(outputs);
    const int claimed_fd = claimed.entries[0].fd;
    const auto origin = map(a, claimed_fd);
    BufferLoanBatch* second = nullptr;
    ASSERT_EQ(0, buffer_prepare_loans(frames, &a, attachment(claimed_fd), second));
    EXPECT_FALSE(readable(a, origin));
    os1_ipc_buffers repeated{};
    ASSERT_EQ(0, buffer_claim_outputs(outputs, &a, repeated));
    EXPECT_EQ(0u, repeated.count);
    buffer_commit_claim(outputs);
    buffer_finish_loans(second);
    EXPECT_TRUE(readable(a, origin));
    buffer_release_loans(second);
    buffer_release_outputs(outputs);
    buffer_release_loans(first);
}
}  // namespace

namespace
{
TEST_F(BufferMemory, RepeatedLifecycleMeasuresPageChargesAndRecovery)
{
    for(unsigned repeat = 0; repeat < 3; ++repeat)
    {
        const auto baseline = frames.free_page_count();
        const int fd = create(a, OS1_BUFFER_BYTES_MAX);
        ASSERT_GE(fd, 0);
        const auto created = frames.free_page_count();
        const auto origin = map(a, fd);
        const auto mapped = frames.free_page_count();
        BufferLoanBatch* loans = nullptr;
        ASSERT_EQ(0, buffer_prepare_loans(frames, &a, attachment(fd), loans));
        os1_ipc_buffers received{};
        ASSERT_EQ(0, buffer_receive_loans(loans, &b, received));
        const auto borrowed = map(b, received.entries[0].fd);
        const auto loaned = frames.free_page_count();
        buffer_finish_loans(loans);
        EXPECT_FALSE(readable(b, borrowed));
        EXPECT_TRUE(readable(a, origin));
        const auto finished = frames.free_page_count();
        buffer_release_loans(loans);
        ASSERT_EQ(0, descriptor_close(provider.descriptors, received.entries[0].fd));
        os1_buffer_map unmap{1, sizeof(unmap), origin, 0, 0};
        ASSERT_EQ(0, call(a, fd, OS1_RESOURCE_BUFFER_UNMAP, unmap));
        const auto before_final_close = frames.free_page_count();
        ASSERT_EQ(0, descriptor_close(caller.descriptors, fd));
        const auto released = frames.free_page_count();
        EXPECT_EQ(before_final_close + OS1_BUFFER_BYTES_MAX / kPageSize, released);
        std::printf(
            "BUFFER_MEMORY_PAGES repeat=%u baseline=%llu created=%llu mapped=%llu loaned=%llu "
            "finished=%llu released=%llu\n",
            repeat + 1,
            static_cast<unsigned long long>(baseline),
            static_cast<unsigned long long>(created),
            static_cast<unsigned long long>(mapped),
            static_cast<unsigned long long>(loaned),
            static_cast<unsigned long long>(finished),
            static_cast<unsigned long long>(released));
    }
}
}  // namespace
