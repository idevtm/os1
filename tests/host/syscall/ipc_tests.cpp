#include "arch/x86_64/cpu/cpu.hpp"
#include "handoff/memory_layout.h"
#include "ipc/channel.hpp"
#include "mm/kmem.hpp"
#include "mm/user_copy.hpp"
#include "proc/user_program.hpp"
#include "support/physical_memory.hpp"
#ifdef assert
#undef assert
#endif
#ifdef static_assert
#undef static_assert
#endif
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>
namespace
{
constexpr uint64_t address = kUserImageBase;
class Ipc : public ::testing::Test
{
protected:
    os1::host_test::PhysicalMemoryArena arena{32ull * 1024 * 1024};
    PageFrameContainer frames;
    Thread* thread = nullptr;
    os1_ipc_pair pair{};
    DescriptorTable& table()
    {
        return thread->process->descriptors;
    }
    void SetUp() override
    {
        std::array<BootMemoryRegion, 1> regions{{{0, arena.size(), BootMemoryType::Usable, 0}}};
        ASSERT_TRUE(frames.initialize(regions, 0x300000, kPageFrameBitmapQwordLimit));
        kmem_init(frames);
        ASSERT_TRUE(init_tasks(frames));
        VirtualMemory vm(frames);
        ASSERT_TRUE(vm.allocate_and_map(address, 1, PageFlags::User | PageFlags::Write));
        auto* process = create_user_process("ipc-test", vm.root());
        ASSERT_NE(nullptr, process);
        thread = create_user_thread(process, address, address + 4096, frames, false);
        ASSERT_NE(nullptr, thread);
        set_current_thread(thread);
        ASSERT_EQ(0, sys_ipc(frames, thread, OS1_IPC_CREATE, 0, address));
        ASSERT_TRUE(copy_from_user(frames, thread, address, &pair, sizeof(pair)));
    }
    void TearDown() override
    {
        cancel_ipc_wait(thread);
        descriptor_close_all(table());
        set_current_thread(nullptr);
    }
    template<class T>
    long call(unsigned op, int fd, T& value)
    {
        EXPECT_TRUE(copy_to_user(frames, thread, address, &value, sizeof(value)));
        long status = sys_ipc(frames, thread, op, fd, address);
        EXPECT_TRUE(copy_from_user(frames, thread, address, &value, sizeof(value)));
        return status;
    }
    long ready()
    {
        os1_ipc_info info{1, sizeof(info), 0, 42, 1, 1};
        return call(OS1_IPC_READY, pair.provider, info);
    }
    os1_ipc_message request()
    {
        os1_ipc_message m{};
        m.version = 1;
        m.size = sizeof(m);
        m.interface_id = 42;
        m.interface_version = 1;
        m.member = 1;
        m.payload_bytes = 3;
        std::memcpy(m.payload, "ABC", 3);
        return m;
    }
    long submit()
    {
        auto m = request();
        return call(OS1_IPC_SUBMIT, pair.client, m);
    }
    os1_ipc_result response(os1_ipc_message m)
    {
        os1_ipc_result r{};
        r.version = 1;
        r.size = sizeof(r);
        r.generation = m.generation;
        r.request_id = m.request_id;
        r.payload_bytes = 3;
        std::memcpy(r.payload, "abc", 3);
        return r;
    }
    long alias(int fd, uint32_t rights)
    {
        os1_duplicate_request r{
            OS1_RESOURCE_ABI_VERSION, sizeof(r), -1, rights, OS1_DESCRIPTOR_CLOEXEC, 0};
        return descriptor_duplicate(table(), fd, r);
    }
};
TEST(IpcAbi, FixedLayouts)
{
    EXPECT_EQ(8u, sizeof(os1_ipc_pair));
    EXPECT_EQ(32u, sizeof(os1_ipc_info));
    EXPECT_EQ(304u, sizeof(os1_ipc_message));
    EXPECT_EQ(296u, sizeof(os1_ipc_result));
}
TEST_F(Ipc, CopiedRequestsRetainedResultsAndOneUseReceipts)
{
    ASSERT_EQ(0, ready());
    long op = submit();
    ASSERT_GE(op, 0);
    os1_ipc_message m{};
    ASSERT_EQ(0, call(OS1_IPC_RECEIVE, pair.provider, m));
    EXPECT_EQ(0, std::memcmp(m.payload, "ABC", 3));
    EXPECT_NE(0u, m.generation);
    EXPECT_NE(0u, m.request_id);
    os1_ipc_result out{};
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, call(OS1_IPC_RESULT, op, out));
    auto r = response(m);
    auto bad = r;
    bad.generation++;
    EXPECT_EQ(OS1_ERR_STALE_REPLY, call(OS1_IPC_REPLY, pair.provider, bad));
    ASSERT_EQ(0, call(OS1_IPC_REPLY, pair.provider, r));
    EXPECT_EQ(OS1_ERR_STALE_REPLY, call(OS1_IPC_REPLY, pair.provider, r));
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_RESULT, op, 0x1000));
    long duplicate = alias(op, OS1_RIGHT_READ);
    ASSERT_GE(duplicate, 0);
    descriptor_close(table(), op);
    descriptor_close(table(), pair.client);
    descriptor_close(table(), pair.provider);
    for(int i = 0; i < 2; ++i)
    {
        ASSERT_EQ(0, call(OS1_IPC_RESULT, duplicate, out));
        EXPECT_EQ(0, out.transport_status);
        EXPECT_EQ(0, std::memcmp(out.payload, "abc", 3));
    }
}
TEST_F(Ipc, ValidatesEnvelopesRightsAndReadiness)
{
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, submit());
    ASSERT_EQ(0, ready());
    EXPECT_EQ(OS1_ERR_INVALID, ready());
    auto m = request();
    m.version = 2;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_SUBMIT, pair.client, m));
    m = request();
    m.payload_bytes = 257;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_SUBMIT, pair.client, m));
    m = request();
    m.payload[255] = 1;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_SUBMIT, pair.client, m));
    m = request();
    m.interface_id++;
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, call(OS1_IPC_SUBMIT, pair.client, m));
    m = request();
    m.request_id = 1;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_SUBMIT, pair.client, m));
    long readonly = alias(pair.client, OS1_RIGHT_READ);
    ASSERT_GE(readonly, 0);
    m = request();
    EXPECT_EQ(OS1_ERR_ACCESS, call(OS1_IPC_SUBMIT, readonly, m));
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, call(OS1_IPC_RECEIVE, pair.client, m));
    EXPECT_EQ(OS1_ERR_BAD_DESCRIPTOR, sys_ipc(frames, thread, OS1_IPC_WAIT, 64, 0));
    EXPECT_EQ(OS1_ERR_INVALID, sys_ipc(frames, thread, OS1_IPC_WAIT, pair.client, 1));
}
TEST_F(Ipc, FailedReceiveDoesNotDeliverAndProviderCloseClassifiesOutcome)
{
    ASSERT_EQ(0, ready());
    long first = submit(), second = submit();
    ASSERT_GE(first, 0);
    ASSERT_GE(second, 0);
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_RECEIVE, pair.provider, 0x1000));
    os1_ipc_message m{};
    ASSERT_EQ(0, call(OS1_IPC_RECEIVE, pair.provider, m));
    descriptor_close(table(), pair.provider);
    os1_ipc_result r{};
    ASSERT_EQ(0, call(OS1_IPC_RESULT, first, r));
    EXPECT_EQ(OS1_ERR_OUTCOME_UNKNOWN, r.transport_status);
    ASSERT_EQ(0, call(OS1_IPC_RESULT, second, r));
    EXPECT_EQ(OS1_ERR_PEER_CLOSED, r.transport_status);
    EXPECT_EQ(OS1_ERR_PEER_CLOSED, submit());
}
TEST_F(Ipc, RetainedResultsConsumeQuotaAndReleasingThemRestoresIt)
{
    ASSERT_EQ(0, ready());
    long operations[OS1_IPC_OPERATION_LIMIT];
    for(auto& op : operations)
    {
        op = submit();
        ASSERT_GE(op, 0);
    }
    EXPECT_EQ(OS1_ERR_LIMIT, submit());
    os1_ipc_message m{};
    ASSERT_EQ(0, call(OS1_IPC_RECEIVE, pair.provider, m));
    auto r = response(m);
    ASSERT_EQ(0, call(OS1_IPC_REPLY, pair.provider, r));
    EXPECT_EQ(OS1_ERR_LIMIT, submit());
    descriptor_close(table(), operations[0]);
    EXPECT_GE(submit(), 0);
}
TEST_F(Ipc, PairFaultRollsBackAndDescriptorExhaustionNeverPublishes)
{
    unsigned before = 0;
    for(auto& e : table().entries)
        before += !!e.instance;
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_CREATE, 0, 0x1000));
    unsigned after = 0;
    for(auto& e : table().entries)
        after += !!e.instance;
    EXPECT_EQ(before, after);
    ASSERT_EQ(0, ready());
    while(alias(pair.client, OS1_RIGHT_READ) >= 0)
    {
    }
    EXPECT_EQ(OS1_ERR_LIMIT, submit());
    os1_ipc_message m{};
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, call(OS1_IPC_RECEIVE, pair.provider, m));
}
TEST_F(Ipc, WaitRegistrationSurvivesCloseAndDefersWakeUntilOffCpu)
{
    ASSERT_EQ(kIpcBlocked, sys_ipc(frames, thread, OS1_IPC_WAIT, pair.client, 0));
    EXPECT_EQ(ThreadWaitReason::Ipc, thread->wait.reason);
    ASSERT_EQ(0, ready());
    EXPECT_EQ(ThreadState::Blocked, thread->state);
    descriptor_close(table(), pair.client);
    set_current_thread(nullptr);
    ipc_drain_wakeups();
    EXPECT_EQ(ThreadState::Ready, thread->state);
    EXPECT_EQ(ThreadWaitReason::None, thread->wait.reason);
    EXPECT_EQ(0u, thread->io_pins);
}
TEST_F(Ipc, WaitCancellationAndProviderAliasesPreserveLifetime)
{
    long duplicate = alias(pair.provider, OS1_RIGHT_READ | OS1_RIGHT_WRITE);
    ASSERT_GE(duplicate, 0);
    ASSERT_EQ(kIpcBlocked, sys_ipc(frames, thread, OS1_IPC_WAIT, pair.client, 0));
    cancel_ipc_wait(thread);
    EXPECT_EQ(ThreadWaitReason::None, thread->wait.reason);
    descriptor_close(table(), pair.provider);
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, submit());
    descriptor_close(table(), duplicate);
    EXPECT_EQ(OS1_ERR_PEER_CLOSED, submit());
}

TEST_F(Ipc, ConcurrentProviderCompletesCopiedRequestsWithoutLostResults)
{
    ASSERT_EQ(0, ready());
    std::atomic<bool> failed{false};
    std::thread provider([&] {
        for(unsigned i = 0; i < 100 && !failed; ++i)
        {
            long status = OS1_ERR_WOULD_BLOCK;
            for(unsigned tries = 0; tries < 100000 && status == OS1_ERR_WOULD_BLOCK && !failed;
                ++tries)
            {
                status = sys_ipc(frames, thread, OS1_IPC_RECEIVE, pair.provider, address + 512);
                if(status == OS1_ERR_WOULD_BLOCK)
                    std::this_thread::yield();
            }
            if(status != 0)
            {
                failed = true;
                break;
            }
            os1_ipc_message m{};
            if(!copy_from_user(frames, thread, address + 512, &m, sizeof(m)) || m.payload[0] != 'A')
            {
                failed = true;
                break;
            }
            auto r = response(m);
            if(!copy_to_user(frames, thread, address + 1024, &r, sizeof(r)) ||
               sys_ipc(frames, thread, OS1_IPC_REPLY, pair.provider, address + 1024) != 0)
            {
                failed = true;
                break;
            }
        }
    });
    for(unsigned i = 0; i < 100 && !failed; ++i)
    {
        long op = submit();
        if(op < 0)
        {
            failed = true;
            break;
        }
        long status = OS1_ERR_WOULD_BLOCK;
        for(unsigned tries = 0; tries < 100000 && status == OS1_ERR_WOULD_BLOCK && !failed; ++tries)
        {
            status = sys_ipc(frames, thread, OS1_IPC_RESULT, op, address + 1536);
            if(status == OS1_ERR_WOULD_BLOCK)
                std::this_thread::yield();
        }
        os1_ipc_result r{};
        if(status != 0 || !copy_from_user(frames, thread, address + 1536, &r, sizeof(r)) ||
           r.transport_status || r.payload[0] != 'a')
            failed = true;
        descriptor_close(table(), op);
    }
    provider.join();
    EXPECT_FALSE(failed.load());
}
TEST_F(Ipc, RepliesCannotCrossConnectionsAndFaultsDoNotConsumeAuthority)
{
    ASSERT_EQ(0, ready());
    const auto first = pair;
    long operation = submit();
    ASSERT_GE(operation, 0);
    os1_ipc_message m{};
    ASSERT_EQ(0, call(OS1_IPC_RECEIVE, pair.provider, m));
    auto r = response(m);
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_REPLY, pair.provider, 0x1000));
    auto malformed = r;
    malformed.payload_bytes = 257;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_REPLY, pair.provider, malformed));
    malformed = r;
    malformed.transport_status = OS1_ERR_PEER_CLOSED;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_REPLY, pair.provider, malformed));
    long readonly = alias(pair.provider, OS1_RIGHT_READ);
    EXPECT_EQ(OS1_ERR_ACCESS, call(OS1_IPC_REPLY, readonly, r));
    ASSERT_EQ(0, sys_ipc(frames, thread, OS1_IPC_CREATE, 0, address));
    ASSERT_TRUE(copy_from_user(frames, thread, address, &pair, sizeof(pair)));
    ASSERT_EQ(0, ready());
    EXPECT_EQ(OS1_ERR_STALE_REPLY, call(OS1_IPC_REPLY, pair.provider, r));
    ASSERT_EQ(0, call(OS1_IPC_REPLY, first.provider, r));
    EXPECT_EQ(0, sys_ipc(frames, thread, OS1_IPC_WAIT, operation, 0));
}
TEST_F(Ipc, OperationWaitAndClientClosureDoNotCancelAcceptedWork)
{
    ASSERT_EQ(0, ready());
    long op = submit();
    ASSERT_GE(op, 0);
    ASSERT_EQ(kIpcBlocked, sys_ipc(frames, thread, OS1_IPC_WAIT, op, 0));
    descriptor_close(table(), pair.client);
    descriptor_close(table(), op);
    os1_ipc_message m{};
    ASSERT_EQ(0, call(OS1_IPC_RECEIVE, pair.provider, m));
    auto r = response(m);
    ASSERT_EQ(0, call(OS1_IPC_REPLY, pair.provider, r));
    set_current_thread(nullptr);
    ipc_drain_wakeups();
    EXPECT_EQ(ThreadState::Ready, thread->state);
    EXPECT_EQ(ThreadWaitReason::None, thread->wait.reason);
    EXPECT_EQ(OS1_ERR_PEER_CLOSED, call(OS1_IPC_RECEIVE, pair.provider, m));
}

TEST_F(Ipc, AllocationExhaustionRejectsWithoutPublishingAndRecovers)
{
    ASSERT_EQ(0, ready());
    // Operation fits the 1024-byte kmalloc class. Exhaust that actual allocator,
    // including physical backing pages, without replacing the allocation path.
    std::vector<void*> held;
    while(void* p = kmalloc(1024))
        held.push_back(p);
    EXPECT_EQ(OS1_ERR_NO_MEMORY, submit());
    os1_ipc_message m{};
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, call(OS1_IPC_RECEIVE, pair.provider, m));
    for(void* p : held)
        kfree(p);
    EXPECT_GE(submit(), 0);
}
TEST_F(Ipc, ReadyAndInfoValidateIdentityAndClosure)
{
    os1_ipc_info info{};
    ASSERT_EQ(0, call(OS1_IPC_INFO, pair.client, info));
    EXPECT_EQ(1u, info.version);
    EXPECT_EQ(sizeof(info), info.size);
    EXPECT_NE(0u, info.generation);
    EXPECT_EQ(0u, info.ready);
    EXPECT_EQ(0u, info.interface_id);
    EXPECT_EQ(0u, info.interface_version);
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_INFO, pair.client, 0x1000));
    os1_ipc_info valid{1, sizeof(valid), 0, 42, 1, 1};
    auto bad = valid;
    bad.generation = 1;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_READY, pair.provider, bad));
    bad = valid;
    bad.ready = 0;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_READY, pair.provider, bad));
    bad = valid;
    bad.interface_id = 0;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_READY, pair.provider, bad));
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_READY, pair.provider, 0x1000));
    descriptor_close(table(), pair.client);
    EXPECT_EQ(OS1_ERR_PEER_CLOSED, ready());
}
}  // namespace
