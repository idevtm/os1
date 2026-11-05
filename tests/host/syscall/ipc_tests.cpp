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
    EXPECT_EQ(16u, sizeof(os1_ipc_capability));
    EXPECT_EQ(72u, sizeof(os1_ipc_capabilities));
    EXPECT_EQ(376u, sizeof(os1_ipc_message_caps));
    EXPECT_EQ(368u, sizeof(os1_ipc_result_caps));
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

TEST_F(Ipc, BroadcastSubscriptionsHaveNoHistoryAndIndependentOverflow)
{
    ASSERT_EQ(0, ready());
    os1_ipc_event event{};
    event.version = 1;
    event.size = sizeof(event);
    event.member = 9;
    event.payload_bytes = 1;
    event.payload[0] = 42;
    ASSERT_EQ(0, call(OS1_IPC_EMIT, pair.provider, event));
    os1_ipc_subscription req{1, sizeof(req), 9, 0};
    long slow = call(OS1_IPC_SUBSCRIBE, pair.client, req),
         fast = call(OS1_IPC_SUBSCRIBE, pair.client, req);
    ASSERT_GE(slow, 0);
    ASSERT_GE(fast, 0);
    os1_ipc_event got{};
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, call(OS1_IPC_EVENT_READ, slow, got));
    for(unsigned i = 0; i < 10; ++i)
    {
        ASSERT_EQ(0, call(OS1_IPC_EMIT, pair.provider, event));
        ASSERT_EQ(0, call(OS1_IPC_EVENT_READ, fast, got));
        EXPECT_EQ(0u, got.dropped);
        EXPECT_EQ(i + 2u, got.sequence);
        EXPECT_EQ(42, got.payload[0]);
    }
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_EVENT_READ, slow, 0x1000));
    ASSERT_EQ(0, call(OS1_IPC_EVENT_READ, slow, got));
    EXPECT_EQ(2u, got.dropped);
    EXPECT_EQ(2u, got.sequence);
    ASSERT_EQ(0, call(OS1_IPC_EVENT_READ, slow, got));
    EXPECT_EQ(0u, got.dropped);
    EXPECT_EQ(3u, got.sequence);
    EXPECT_EQ(0, sys_ipc(frames, thread, OS1_IPC_UNSUBSCRIBE, slow, 0));
    EXPECT_EQ(OS1_ERR_CLOSED, call(OS1_IPC_EVENT_READ, slow, got));
    EXPECT_EQ(0, sys_ipc(frames, thread, OS1_IPC_UNSUBSCRIBE, slow, 0));
}
TEST_F(Ipc, SubscriptionRightsBoundsAndQuotaAreTransactional)
{
    os1_ipc_subscription req{1, sizeof(req), 9, 0};
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, call(OS1_IPC_SUBSCRIBE, pair.client, req));
    ASSERT_EQ(0, ready());
    req.reserved = 1;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_SUBSCRIBE, pair.client, req));
    req.reserved = 0;
    long subscriptions[OS1_IPC_SUBSCRIPTION_LIMIT];
    for(auto& fd : subscriptions)
    {
        fd = call(OS1_IPC_SUBSCRIBE, pair.client, req);
        ASSERT_GE(fd, 0);
    }
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SUBSCRIBE, pair.client, req));
    long readonly = alias(subscriptions[0], OS1_RIGHT_READ);
    ASSERT_GE(readonly, 0);
    EXPECT_EQ(OS1_ERR_ACCESS, sys_ipc(frames, thread, OS1_IPC_UNSUBSCRIBE, readonly, 0));
    EXPECT_EQ(0, sys_ipc(frames, thread, OS1_IPC_UNSUBSCRIBE, subscriptions[0], 0));
    EXPECT_GE(call(OS1_IPC_SUBSCRIBE, pair.client, req), 0);
    os1_ipc_event event{};
    event.version = 1;
    event.size = sizeof(event);
    event.member = 9;
    event.sequence = 1;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_EMIT, pair.provider, event));
    event.sequence = 0;
    event.payload_bytes = 257;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_EMIT, pair.provider, event));
    event.payload_bytes = 0;
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, call(OS1_IPC_EMIT, pair.client, event));
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, call(OS1_IPC_SUBSCRIBE, pair.provider, req));
}
TEST_F(Ipc, SubscriptionWaitCloseAndUnsubscribeWakeSafely)
{
    ASSERT_EQ(0, ready());
    os1_ipc_subscription req{1, sizeof(req), 9, 0};
    long sub = call(OS1_IPC_SUBSCRIBE, pair.client, req);
    ASSERT_GE(sub, 0);
    ASSERT_EQ(kIpcBlocked, sys_ipc(frames, thread, OS1_IPC_WAIT, sub, 0));
    EXPECT_EQ(0, sys_ipc(frames, thread, OS1_IPC_UNSUBSCRIBE, sub, 0));
    EXPECT_EQ(ThreadState::Blocked, thread->state);
    set_current_thread(nullptr);
    ipc_drain_wakeups();
    EXPECT_EQ(ThreadState::Ready, thread->state);
    os1_ipc_event event{};
    EXPECT_EQ(OS1_ERR_CLOSED, call(OS1_IPC_EVENT_READ, sub, event));
    long another = call(OS1_IPC_SUBSCRIBE, pair.client, req);
    ASSERT_GE(another, 0);
    descriptor_close(table(), pair.provider);
    EXPECT_EQ(OS1_ERR_PEER_CLOSED, call(OS1_IPC_EVENT_READ, another, event));
}
TEST_F(Ipc, DeadlineEndsWaitWithoutCancellingOperationAndReadyWins)
{
    ASSERT_EQ(0, ready());
    long op = submit();
    ASSERT_GE(op, 0);
    long now = sys_ipc(frames, thread, OS1_IPC_CLOCK, 0, 0);
    ASSERT_GE(now, 0);
    os1_ipc_deadline deadline{1, sizeof(deadline), static_cast<uint64_t>(now) + 100};
    ASSERT_EQ(kIpcBlocked, call(OS1_IPC_WAIT_UNTIL, op, deadline));
    ipc_timer_tick(100);
    EXPECT_EQ(ThreadState::Blocked, thread->state);
    set_current_thread(nullptr);
    ipc_drain_wakeups();
    EXPECT_EQ(ThreadState::Ready, thread->state);
    EXPECT_EQ(OS1_ERR_TIMED_OUT, static_cast<long>(thread->frame.rax));
    os1_ipc_result result{};
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, call(OS1_IPC_RESULT, op, result));
    os1_ipc_message m{};
    ASSERT_EQ(0, call(OS1_IPC_RECEIVE, pair.provider, m));
    auto response_value = response(m);
    ASSERT_EQ(0, call(OS1_IPC_REPLY, pair.provider, response_value));
    EXPECT_EQ(0, call(OS1_IPC_WAIT_UNTIL, op, deadline));
    EXPECT_EQ(0, call(OS1_IPC_RESULT, op, result));
    EXPECT_EQ(0, result.transport_status);
}
TEST_F(Ipc, ExpiredAndMalformedDeadlinesDoNotRegisterWaits)
{
    os1_ipc_deadline deadline{1, sizeof(deadline), 0};
    EXPECT_EQ(OS1_ERR_TIMED_OUT, call(OS1_IPC_WAIT_UNTIL, pair.client, deadline));
    EXPECT_EQ(ThreadWaitReason::None, thread->wait.reason);
    deadline.version = 2;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_WAIT_UNTIL, pair.client, deadline));
    deadline.version = 1;
    deadline.nanoseconds = UINT64_MAX;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_WAIT_UNTIL, pair.client, deadline));
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_WAIT_UNTIL, pair.client, 0x1000));
    EXPECT_EQ(OS1_ERR_INVALID, sys_ipc(frames, thread, OS1_IPC_CLOCK, 1, 0));
}
TEST_F(Ipc, StopRequiresOwnedPrivateProviderAndWakesItsIpcWait)
{
    auto* child_process = create_user_process("private-provider", thread->address_space_cr3);
    ASSERT_NE(nullptr, child_process);
    child_process->parent = thread->process;
    auto* child = create_user_thread(child_process, address, address + 4096, frames, false);
    ASSERT_NE(nullptr, child);
    EXPECT_EQ(OS1_ERR_ACCESS, sys_ipc(frames, thread, OS1_IPC_STOP_CHILD, 0, child_process->pid));
    child_process->private_provider = true;
    OpenInstance* provider = nullptr;
    ASSERT_EQ(0, descriptor_acquire(table(), pair.provider, 0, provider));
    ASSERT_EQ(3,
              descriptor_install(
                  child_process->descriptors, provider, OS1_RIGHT_READ | OS1_RIGHT_WRITE, 0, 3));
    instance_release(provider);
    ASSERT_EQ(kIpcBlocked, sys_ipc(frames, child, OS1_IPC_WAIT, 3, 0));
    EXPECT_EQ(0, sys_ipc(frames, thread, OS1_IPC_STOP_CHILD, 0, child_process->pid));
    EXPECT_TRUE(ipc_stop_requested(child));
    EXPECT_EQ(ThreadState::Ready, child->state);
    EXPECT_EQ(OS1_ERR_CANCELLED, static_cast<long>(child->frame.rax));
    descriptor_close_all(child_process->descriptors);
}

TEST_F(Ipc, ParentExitRequestsStopOnlyForItsPrivateChildren)
{
    auto make = [&](bool supervised) {
        auto* p = create_user_process("child", thread->address_space_cr3);
        p->parent = thread->process;
        p->private_provider = supervised;
        return create_user_thread(p, address, address + 4096, frames, false);
    };
    auto* private_child = make(true);
    auto* ordinary_child = make(false);
    ASSERT_NE(nullptr, private_child);
    ASSERT_NE(nullptr, ordinary_child);
    ipc_stop_children(thread->process);
    EXPECT_TRUE(ipc_stop_requested(private_child));
    EXPECT_FALSE(ipc_stop_requested(ordinary_child));
}

TEST_F(Ipc, CapabilityRequestsAttenuateAndSurviveSenderClose)
{
    ASSERT_EQ(0, ready());
    os1_ipc_pair newer{};
    ASSERT_EQ(0, call(OS1_IPC_CREATE, 0, newer));
    os1_ipc_message_caps sent{};
    sent.message = request();
    sent.capabilities.count = 1;
    sent.capabilities.entries[0] = {newer.client, OS1_RIGHT_READ | OS1_RIGHT_INSPECT, 0, 0};
    long op = call(OS1_IPC_SUBMIT_CAPS, pair.client, sent);
    ASSERT_GE(op, 0);
    ASSERT_EQ(0, descriptor_close(table(), newer.client));
    os1_ipc_message plain{};
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, call(OS1_IPC_RECEIVE, pair.provider, plain));
    os1_ipc_message_caps received{};
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_RECEIVE_CAPS, pair.provider, 0x1000));
    ASSERT_EQ(0, call(OS1_IPC_RECEIVE_CAPS, pair.provider, received));
    ASSERT_EQ(1u, received.capabilities.count);
    int fd = received.capabilities.entries[0].fd;
    os1_resource_info info{};
    ASSERT_EQ(0, descriptor_query(table(), fd, info));
    EXPECT_EQ(OS1_RIGHT_READ | OS1_RIGHT_INSPECT, info.rights);
    EXPECT_EQ(OS1_DESCRIPTOR_CLOEXEC, info.descriptor_flags);
    EXPECT_EQ(OS1_ERR_ACCESS, alias(fd, OS1_RIGHT_READ));
    os1_ipc_info ready_info{1, sizeof(ready_info), 0, 77, 1, 1};
    ASSERT_EQ(0, call(OS1_IPC_READY, newer.provider, ready_info));
    ASSERT_EQ(0, call(OS1_IPC_INFO, fd, ready_info));
    EXPECT_EQ(77u, ready_info.interface_id);
    auto r = response(received.message);
    EXPECT_EQ(0, call(OS1_IPC_REPLY, pair.provider, r));
}
TEST_F(Ipc, CapabilityValidationRejectsAmplificationAndCycles)
{
    ASSERT_EQ(0, ready());
    os1_ipc_pair newer{};
    ASSERT_EQ(0, call(OS1_IPC_CREATE, 0, newer));
    os1_ipc_message_caps m{};
    m.message = request();
    m.capabilities.count = 1;
    m.capabilities.entries[0] = {pair.client, OS1_RIGHT_READ, 0, 0};
    EXPECT_EQ(OS1_ERR_ACCESS, call(OS1_IPC_SUBMIT_CAPS, pair.client, m));
    m.capabilities.entries[0].fd = newer.provider;
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, call(OS1_IPC_SUBMIT_CAPS, pair.client, m));
    m.capabilities.entries[0].fd = newer.client;
    m.capabilities.entries[0].rights = OS1_RIGHT_EXECUTE;
    EXPECT_EQ(OS1_ERR_ACCESS, call(OS1_IPC_SUBMIT_CAPS, pair.client, m));
    m.capabilities.entries[0].rights = OS1_RIGHT_READ;
    m.capabilities.entries[0].descendant_rights = OS1_RIGHT_READ;
    EXPECT_EQ(OS1_ERR_ACCESS, call(OS1_IPC_SUBMIT_CAPS, pair.client, m));
    m.capabilities.entries[0].descendant_rights = 0;
    long restricted = alias(newer.client, OS1_RIGHT_READ);
    ASSERT_GE(restricted, 0);
    m.capabilities.entries[0].fd = restricted;
    EXPECT_EQ(OS1_ERR_ACCESS, call(OS1_IPC_SUBMIT_CAPS, pair.client, m));
    m.capabilities.entries[0].fd = newer.client;
    m.capabilities.count = 5;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_SUBMIT_CAPS, pair.client, m));
    m.capabilities.count = 1;
    m.capabilities.entries[3].rights = OS1_RIGHT_READ;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_SUBMIT_CAPS, pair.client, m));
    m.capabilities.entries[3] = {};
    m.capabilities.entries[0].reserved = 1;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_SUBMIT_CAPS, pair.client, m));
    m.capabilities.entries[0].reserved = 0;
    m.capabilities.reserved = 1;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_SUBMIT_CAPS, pair.client, m));
    m.capabilities.reserved = 0;
    m.capabilities.entries[0].fd = 64;
    EXPECT_EQ(OS1_ERR_BAD_DESCRIPTOR, call(OS1_IPC_SUBMIT_CAPS, pair.client, m));
}
TEST_F(Ipc, CapabilityDeliveryQuotaRollbackAndSingleClaimAcrossAliases)
{
    ASSERT_EQ(0, ready());
    long op = submit();
    ASSERT_GE(op, 0);
    os1_ipc_message m{};
    ASSERT_EQ(0, call(OS1_IPC_RECEIVE, pair.provider, m));
    os1_ipc_result_caps reply{};
    reply.result = response(m);
    reply.capabilities.count = 2;
    for(auto i : {0, 1})
        reply.capabilities.entries[i] = {pair.client, OS1_RIGHT_READ, 0, 0};
    ASSERT_EQ(0, call(OS1_IPC_REPLY_CAPS, pair.provider, reply));
    EXPECT_EQ(OS1_ERR_STALE_REPLY, call(OS1_IPC_REPLY_CAPS, pair.provider, reply));
    long duplicate = alias(op, OS1_RIGHT_READ);
    ASSERT_GE(duplicate, 0);
    std::vector<int> fillers;
    for(;;)
    {
        long fd = alias(pair.client, OS1_RIGHT_READ);
        if(fd < 0)
        {
            EXPECT_EQ(OS1_ERR_LIMIT, fd);
            break;
        }
        fillers.push_back(fd);
    }
    ASSERT_GE(fillers.size(), 2u);
    descriptor_close(table(), fillers.back());
    fillers.pop_back();
    os1_ipc_result_caps out{};
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_RESULT_CAPS, op, out));
    // The one free slot was rolled back, so it can still be allocated.
    long spare = alias(pair.client, OS1_RIGHT_READ);
    ASSERT_GE(spare, 0);
    descriptor_close(table(), spare);
    descriptor_close(table(), fillers.back());
    fillers.pop_back();
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_RESULT_CAPS, op, 0x1000));
    ASSERT_EQ(0, call(OS1_IPC_RESULT_CAPS, duplicate, out));
    ASSERT_EQ(2u, out.capabilities.count);
    EXPECT_NE(out.capabilities.entries[0].fd, out.capabilities.entries[1].fd);
    EXPECT_EQ(0, call(OS1_IPC_RESULT_CAPS, op, out));
    EXPECT_EQ(0u, out.capabilities.count);
    EXPECT_EQ(0, std::memcmp(out.result.payload, "abc", 3));
}
TEST_F(Ipc, DroppingUnclaimedReplyReleasesAttachedClient)
{
    ASSERT_EQ(0, ready());
    os1_ipc_pair returned{};
    ASSERT_EQ(0, call(OS1_IPC_CREATE, 0, returned));
    long op = submit();
    ASSERT_GE(op, 0);
    os1_ipc_message m{};
    ASSERT_EQ(0, call(OS1_IPC_RECEIVE, pair.provider, m));
    os1_ipc_result_caps r{};
    r.result = response(m);
    r.capabilities.count = 1;
    r.capabilities.entries[0] = {returned.client, OS1_RIGHT_READ, 0, 0};
    ASSERT_EQ(0, call(OS1_IPC_REPLY_CAPS, pair.provider, r));
    descriptor_close(table(), returned.client);
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, call(OS1_IPC_RECEIVE, returned.provider, m));
    descriptor_close(table(), op);
    EXPECT_EQ(OS1_ERR_PEER_CLOSED, call(OS1_IPC_RECEIVE, returned.provider, m));
}
TEST_F(Ipc, ProviderShutdownReleasesUnreadRequestAttachments)
{
    ASSERT_EQ(0, ready());
    os1_ipc_pair attached{};
    ASSERT_EQ(0, call(OS1_IPC_CREATE, 0, attached));
    os1_ipc_message_caps r{};
    r.message = request();
    r.capabilities.count = 1;
    r.capabilities.entries[0] = {attached.client, OS1_RIGHT_READ, 0, 0};
    long op = call(OS1_IPC_SUBMIT_CAPS, pair.client, r);
    ASSERT_GE(op, 0);
    descriptor_close(table(), attached.client);
    descriptor_close(table(), pair.provider);
    os1_ipc_message m{};
    EXPECT_EQ(OS1_ERR_PEER_CLOSED, call(OS1_IPC_RECEIVE, attached.provider, m));
    os1_ipc_result_caps out{};
    EXPECT_EQ(0, call(OS1_IPC_RESULT_CAPS, op, out));
    EXPECT_EQ(OS1_ERR_PEER_CLOSED, out.result.transport_status);
    EXPECT_EQ(0u, out.capabilities.count);
}

TEST_F(Ipc, PartialCapabilityCaptureRollsBackAndFourAttachmentsAreSupported)
{
    ASSERT_EQ(0, ready());
    os1_ipc_pair newer{};
    ASSERT_EQ(0, call(OS1_IPC_CREATE, 0, newer));
    os1_ipc_message_caps m{};
    m.message = request();
    m.capabilities.count = 2;
    m.capabilities.entries[0] = {newer.client, OS1_RIGHT_READ, 0, 0};
    m.capabilities.entries[1] = {newer.provider, OS1_RIGHT_READ, 0, 0};
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, call(OS1_IPC_SUBMIT_CAPS, pair.client, m));
    descriptor_close(table(), newer.client);
    os1_ipc_message unused{};
    EXPECT_EQ(OS1_ERR_PEER_CLOSED, call(OS1_IPC_RECEIVE, newer.provider, unused));
    long op = submit();
    ASSERT_GE(op, 0);
    ASSERT_EQ(0, call(OS1_IPC_RECEIVE, pair.provider, unused));
    os1_ipc_result_caps r{};
    r.result = response(unused);
    r.capabilities.count = OS1_IPC_CAP_LIMIT;
    for(auto& entry : r.capabilities.entries)
        entry = {pair.client, OS1_RIGHT_READ, 0, 0};
    ASSERT_EQ(0, call(OS1_IPC_REPLY_CAPS, pair.provider, r));
    os1_ipc_result_caps out{};
    ASSERT_EQ(0, call(OS1_IPC_RESULT_CAPS, op, out));
    EXPECT_EQ(OS1_IPC_CAP_LIMIT, out.capabilities.count);
    for(unsigned i = 0; i < out.capabilities.count; ++i)
        for(unsigned j = 0; j < i; ++j)
            EXPECT_NE(out.capabilities.entries[i].fd, out.capabilities.entries[j].fd);
}
}  // namespace
