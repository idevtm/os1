#include "arch/x86_64/cpu/cpu.hpp"
#include "buffer/buffer.hpp"
#include "console/console_input.hpp"
#include "handoff/memory_layout.h"
#include "ipc/channel.hpp"
#include "mm/kmem.hpp"
#include "mm/user_copy.hpp"
#include "platform/state.hpp"
#include "proc/user_program.hpp"
#include "support/physical_memory.hpp"
#ifdef assert
#undef assert
#endif
#ifdef static_assert
#undef static_assert
#endif
#include <gtest/gtest.h>
#include <os1/console.h>

#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
extern void (*host_buffer_before_prepare)();
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
    EXPECT_EQ(8u, sizeof(os1_ipc_buffer));
    EXPECT_EQ(40u, sizeof(os1_ipc_buffers));
    EXPECT_EQ(344u, sizeof(os1_ipc_message_buffers));
    EXPECT_EQ(336u, sizeof(os1_ipc_result_buffers));
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

TEST_F(Ipc, WaitManyValidatesWholeSelectionBeforeReportingReady)
{
    EXPECT_EQ(56u, sizeof(os1_ipc_wait_many));
    EXPECT_EQ(16u, sizeof(os1_ipc_account));
    os1_ipc_wait_many w{1, sizeof(w), 2, 0, UINT64_MAX, {pair.client, pair.provider}};
    ASSERT_EQ(0, ready());
    EXPECT_EQ(0, call(OS1_IPC_WAIT_MANY, 0, w));
    w.fds[1] = pair.client;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_WAIT_MANY, 0, w));
    w.fds[1] = alias(pair.client, OS1_RIGHT_READ);
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_WAIT_MANY, 0, w));
    w.fds[1] = 64;
    EXPECT_EQ(OS1_ERR_BAD_DESCRIPTOR, call(OS1_IPC_WAIT_MANY, 0, w));
    w.fds[1] = -1;
    EXPECT_EQ(OS1_ERR_BAD_DESCRIPTOR, call(OS1_IPC_WAIT_MANY, 0, w));
    w.fds[1] = alias(pair.provider, OS1_RIGHT_WRITE);
    EXPECT_EQ(OS1_ERR_ACCESS, call(OS1_IPC_WAIT_MANY, 0, w));
    w.fds[1] = pair.provider;
    w.fds[7] = 1;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_WAIT_MANY, 0, w));
    w.fds[7] = 0;
    for(unsigned count : {0u, OS1_IPC_WAIT_LIMIT + 1})
    {
        w.count = count;
        EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_WAIT_MANY, 0, w));
    }
    w.count = 2;
    w.reserved = 1;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_WAIT_MANY, 0, w));
    w.reserved = 0;
    w.version = 2;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_WAIT_MANY, 0, w));
    w.version = 1;
    w.size--;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_WAIT_MANY, 0, w));
    w.size++;
    w.nanoseconds = UINT64_MAX - 1;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_WAIT_MANY, 0, w));
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_WAIT_MANY, 1, w));
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_WAIT_MANY, 0, 0x1000));
    EXPECT_EQ(ThreadWaitReason::None, thread->wait.reason);
}
TEST_F(Ipc, WaitManyPinsExactInstancesAcrossCloseReuseAndCancellation)
{
    os1_ipc_pair other{};
    ASSERT_EQ(0, call(OS1_IPC_CREATE, 0, other));
    os1_ipc_wait_many w{1, sizeof(w), 2, 0, UINT64_MAX, {pair.client, other.client}};
    ASSERT_EQ(kIpcBlocked, call(OS1_IPC_WAIT_MANY, 0, w));
    auto* retained = thread->wait.ipc.instances[1];
    ASSERT_EQ(2u, thread->wait.ipc.count);
    EXPECT_EQ(0, descriptor_close(table(), other.client));
    long reused = alias(pair.provider, OS1_RIGHT_READ);
    EXPECT_EQ(other.client, reused);
    EXPECT_EQ(1u, retained->references);
    os1_ipc_info info{1, sizeof(info), 0, 42, 1, 1};
    ASSERT_EQ(0, call(OS1_IPC_READY, other.provider, info));
    EXPECT_EQ(ThreadState::Blocked, thread->state);
    // Corrupting the original record cannot affect the kernel-owned registration.
    w.fds[1] = 64;
    EXPECT_TRUE(copy_to_user(frames, thread, address, &w, sizeof(w)));
    set_current_thread(nullptr);
    ipc_drain_wakeups();
    EXPECT_EQ(1, static_cast<long>(thread->frame.rax));
    EXPECT_EQ(ThreadWaitReason::None, thread->wait.reason);
    os1_ipc_message m{};
    EXPECT_EQ(OS1_ERR_PEER_CLOSED, call(OS1_IPC_RECEIVE, other.provider, m));
    w = {1, sizeof(w), 1, 0, UINT64_MAX, {pair.client}};
    ASSERT_EQ(kIpcBlocked, call(OS1_IPC_WAIT_MANY, 0, w));
    retained = thread->wait.ipc.instances[0];
    EXPECT_EQ(2u, retained->references);
    cancel_ipc_wait(thread);
    EXPECT_EQ(1u, retained->references);
}
TEST_F(Ipc, WaitManyDeadlineStopClosureAndUnsubscribeSemantics)
{
    ASSERT_EQ(0, ready());
    long op = submit();
    ASSERT_GE(op, 0);
    os1_ipc_subscription req{1, sizeof(req), 9, 0};
    long sub = call(OS1_IPC_SUBSCRIBE, pair.client, req);
    ASSERT_GE(sub, 0);
    const auto now = sys_ipc(frames, thread, OS1_IPC_CLOCK, 0, 0);
    os1_ipc_wait_many w{1,
                        sizeof(w),
                        2,
                        0,
                        static_cast<uint64_t>(now) + 10,
                        {static_cast<int>(op), static_cast<int>(sub)}};
    ASSERT_EQ(kIpcBlocked, call(OS1_IPC_WAIT_MANY, 0, w));
    ipc_timer_tick(10);
    set_current_thread(nullptr);
    ipc_drain_wakeups();
    EXPECT_EQ(OS1_ERR_TIMED_OUT, static_cast<long>(thread->frame.rax));
    EXPECT_EQ(ThreadWaitReason::None, thread->wait.reason);
    EXPECT_EQ(OS1_ERR_TIMED_OUT, call(OS1_IPC_WAIT_MANY, 0, w));
    ASSERT_EQ(0, sys_ipc(frames, thread, OS1_IPC_UNSUBSCRIBE, sub, 0));
    EXPECT_EQ(1, call(OS1_IPC_WAIT_MANY, 0, w));
    thread->stop_requested = true;
    EXPECT_EQ(OS1_ERR_CANCELLED, call(OS1_IPC_WAIT_MANY, 0, w));
    thread->stop_requested = false;
    descriptor_close(table(), pair.provider);
    EXPECT_EQ(0, call(OS1_IPC_WAIT_MANY, 0, w));
}
TEST_F(Ipc, AccountReservationsAreBoundedSharedAndCannotBeReminted)
{
    os1_ipc_account request{1, sizeof(request), 4, 0};
    long root = call(OS1_IPC_ACCOUNT_CREATE, pair.provider, request);
    ASSERT_GE(root, 0);
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_ACCOUNT_CREATE, pair.provider, request));
    long duplicate = alias(root, OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_DELEGATE);
    ASSERT_GE(duplicate, 0);
    request.sessions = 2;
    long a = call(OS1_IPC_ACCOUNT_CREATE, duplicate, request);
    long b = call(OS1_IPC_ACCOUNT_CREATE, root, request);
    ASSERT_GE(a, 0);
    ASSERT_GE(b, 0);
    request.sessions = 1;
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_ACCOUNT_CREATE, root, request));
    EXPECT_EQ(OS1_ERR_ACCESS, call(OS1_IPC_ACCOUNT_CREATE, a, request));
    os1_ipc_pair session{};
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SESSION_CREATE, root, session));
    os1_ipc_pair sessions[4]{};
    for(unsigned i = 0; i < 4; ++i)
        ASSERT_EQ(0, call(OS1_IPC_SESSION_CREATE, i < 2 ? a : b, sessions[i]));
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SESSION_CREATE, a, session));
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SESSION_CREATE, b, session));
    EXPECT_EQ(OS1_ERR_ACCESS, call(OS1_IPC_ACCOUNT_CREATE, sessions[0].provider, request));
    // Child capacity remains reserved after all account descriptors close.
    descriptor_close(table(), a);
    descriptor_close(table(), b);
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SESSION_CREATE, duplicate, session));
    for(auto p : sessions)
    {
        descriptor_close(table(), p.client);
        descriptor_close(table(), p.provider);
    }
    EXPECT_EQ(0, call(OS1_IPC_SESSION_CREATE, duplicate, session));
    descriptor_close(table(), duplicate);
    descriptor_close(table(), root);
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_ACCOUNT_CREATE, pair.provider, request));
}
TEST_F(Ipc, SessionRetainedResultsAndInactiveSubscriptionsKeepAdmissionCharged)
{
    os1_ipc_account request{1, sizeof(request), 2, 0};
    long root = call(OS1_IPC_ACCOUNT_CREATE, pair.provider, request);
    ASSERT_GE(root, 0);
    request.sessions = 1;
    long grant = call(OS1_IPC_ACCOUNT_CREATE, root, request);
    ASSERT_GE(grant, 0);
    const auto control = pair;
    ASSERT_EQ(0, call(OS1_IPC_SESSION_CREATE, grant, pair));
    ASSERT_EQ(0, ready());
    long ops[OS1_IPC_SESSION_OPERATION_LIMIT];
    for(auto& op : ops)
    {
        op = submit();
        ASSERT_GE(op, 0);
        os1_ipc_message m{};
        ASSERT_EQ(0, call(OS1_IPC_RECEIVE, pair.provider, m));
        auto r = response(m);
        ASSERT_EQ(0, call(OS1_IPC_REPLY, pair.provider, r));
    }
    EXPECT_EQ(OS1_ERR_LIMIT, submit());
    os1_ipc_subscription req{1, sizeof(req), 9, 0};
    long subs[OS1_IPC_SESSION_SUBSCRIPTION_LIMIT];
    for(auto& sub : subs)
    {
        sub = call(OS1_IPC_SUBSCRIBE, pair.client, req);
        ASSERT_GE(sub, 0);
        EXPECT_EQ(0, sys_ipc(frames, thread, OS1_IPC_UNSUBSCRIBE, sub, 0));
    }
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SUBSCRIBE, pair.client, req));
    os1_ipc_pair spare{};
    EXPECT_EQ(0, call(OS1_IPC_SESSION_CREATE, root, spare));
    const auto saturated = pair;
    pair = spare;
    ASSERT_EQ(0, ready());
    EXPECT_GE(submit(), 0);  // Independent budget despite retained completions elsewhere.
    pair = control;
    ASSERT_EQ(0, ready());
    EXPECT_GE(submit(), 0);  // Control also has independent capacity.
    descriptor_close(table(), saturated.client);
    descriptor_close(table(), saturated.provider);
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SESSION_CREATE, grant, pair));
    for(auto op : ops)
        descriptor_close(table(), op);
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SESSION_CREATE, grant, pair));
    descriptor_close(table(), subs[0]);
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SESSION_CREATE, grant, pair));
    descriptor_close(table(), subs[1]);
    EXPECT_EQ(0, call(OS1_IPC_SESSION_CREATE, grant, pair));
}
TEST_F(Ipc, AccountValidationRightsFaultsAndTransferPreserveCapacity)
{
    os1_ipc_account request{1, sizeof(request), 1, 0};
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, call(OS1_IPC_ACCOUNT_CREATE, pair.client, request));
    for(unsigned size : {0u, 5u})
    {
        request.sessions = size;
        EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_ACCOUNT_CREATE, pair.provider, request));
    }
    request.sessions = 1;
    request.version = 2;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_ACCOUNT_CREATE, pair.provider, request));
    request.version = 1;
    request.size--;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_ACCOUNT_CREATE, pair.provider, request));
    request.size++;
    request.reserved = 1;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_ACCOUNT_CREATE, pair.provider, request));
    request.reserved = 0;
    EXPECT_EQ(OS1_ERR_FAULT,
              sys_ipc(frames, thread, OS1_IPC_ACCOUNT_CREATE, pair.provider, 0x1000));
    long root = call(OS1_IPC_ACCOUNT_CREATE, pair.provider, request);
    ASSERT_GE(root, 0);
    long readonly = alias(root, OS1_RIGHT_READ);
    EXPECT_EQ(OS1_ERR_ACCESS, call(OS1_IPC_ACCOUNT_CREATE, readonly, request));
    os1_ipc_pair session{};
    EXPECT_EQ(OS1_ERR_ACCESS, call(OS1_IPC_SESSION_CREATE, readonly, session));
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, call(OS1_IPC_SESSION_CREATE, pair.provider, session));
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_SESSION_CREATE, root, 0x1000));
    ASSERT_EQ(0, ready());
    os1_ipc_message_caps sent{};
    sent.message = this->request();
    sent.capabilities.count = 1;
    sent.capabilities.entries[0] = {static_cast<int>(root), OS1_RIGHT_READ | OS1_RIGHT_WRITE, 0, 0};
    ASSERT_GE(call(OS1_IPC_SUBMIT_CAPS, pair.client, sent), 0);
    descriptor_close(table(), root);
    os1_ipc_message_caps received{};
    ASSERT_EQ(0, call(OS1_IPC_RECEIVE_CAPS, pair.provider, received));
    ASSERT_EQ(1u, received.capabilities.count);
    long transferred = received.capabilities.entries[0].fd;
    ASSERT_EQ(0, call(OS1_IPC_SESSION_CREATE, transferred, session));
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SESSION_CREATE, transferred, session));
    os1_ipc_wait_many w{1, sizeof(w), 1, 0, 0, {static_cast<int>(transferred)}};
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, call(OS1_IPC_WAIT_MANY, 0, w));
}

TEST_F(Ipc, WaitManyMaximumRegistrationCancelsEveryPinAndDoesNotLoseRacingWake)
{
    os1_ipc_pair pairs[OS1_IPC_WAIT_LIMIT]{};
    os1_ipc_wait_many w{1, sizeof(w), OS1_IPC_WAIT_LIMIT, 0, UINT64_MAX, {}};
    for(unsigned i = 0; i < OS1_IPC_WAIT_LIMIT; ++i)
    {
        ASSERT_EQ(0, call(OS1_IPC_CREATE, 0, pairs[i]));
        w.fds[i] = pairs[i].client;
    }
    ASSERT_EQ(kIpcBlocked, call(OS1_IPC_WAIT_MANY, 0, w));
    OpenInstance* pins[OS1_IPC_WAIT_LIMIT]{};
    for(unsigned i = 0; i < OS1_IPC_WAIT_LIMIT; ++i)
    {
        pins[i] = thread->wait.ipc.instances[i];
        EXPECT_EQ(2u, pins[i]->references);
    }
    cancel_ipc_wait(thread);
    for(auto* pin : pins)
        EXPECT_EQ(1u, pin->references);
    os1_ipc_info info{1, sizeof(info), 0, 42, 1, 1};
    ASSERT_TRUE(copy_to_user(frames, thread, address + 1024, &info, sizeof(info)));
    std::atomic<bool> go{false};
    long status = OS1_ERR_INVALID;
    std::thread producer([&] {
        while(!go.load())
            std::this_thread::yield();
        status = sys_ipc(frames, thread, OS1_IPC_READY, pairs[7].provider, address + 1024);
    });
    go = true;
    long waited = call(OS1_IPC_WAIT_MANY, 0, w);
    producer.join();
    ASSERT_EQ(0, status);
    EXPECT_TRUE(waited == 7 || waited == kIpcBlocked);
    if(waited == kIpcBlocked)
    {
        set_current_thread(nullptr);
        ipc_drain_wakeups();
        EXPECT_EQ(ThreadState::Ready, thread->state);
        EXPECT_EQ(7, static_cast<long>(thread->frame.rax));
    }
    for(auto* pin : pins)
        EXPECT_EQ(1u, pin->references);
}
TEST_F(Ipc, RetainedSessionAttachmentKeepsGrantChargedUntilClaimAndFinalClose)
{
    os1_ipc_account request{1, sizeof(request), 1, 0};
    long root = call(OS1_IPC_ACCOUNT_CREATE, pair.provider, request);
    ASSERT_GE(root, 0);
    os1_ipc_pair returned{};
    ASSERT_EQ(0, call(OS1_IPC_SESSION_CREATE, root, returned));
    ASSERT_EQ(0, ready());
    long operation = submit();
    ASSERT_GE(operation, 0);
    os1_ipc_message m{};
    ASSERT_EQ(0, call(OS1_IPC_RECEIVE, pair.provider, m));
    os1_ipc_result_caps r{};
    r.result = response(m);
    r.capabilities.count = 1;
    r.capabilities.entries[0] = {returned.client, OS1_RIGHT_READ, 0, 0};
    ASSERT_EQ(0, call(OS1_IPC_REPLY_CAPS, pair.provider, r));
    descriptor_close(table(), returned.client);
    descriptor_close(table(), returned.provider);
    os1_ipc_pair another{};
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SESSION_CREATE, root, another));
    os1_ipc_result_caps out{};
    ASSERT_EQ(0, call(OS1_IPC_RESULT_CAPS, operation, out));
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SESSION_CREATE, root, another));
    descriptor_close(table(), operation);
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SESSION_CREATE, root, another));
    descriptor_close(table(), out.capabilities.entries[0].fd);
    EXPECT_EQ(0, call(OS1_IPC_SESSION_CREATE, root, another));
}
TEST_F(Ipc, AccountAndSessionDescriptorFailuresRollBackAdmission)
{
    os1_ipc_account request{1, sizeof(request), 1, 0};
    std::vector<int> fillers;
    for(;;)
    {
        long fd = alias(pair.client, OS1_RIGHT_READ);
        if(fd < 0)
            break;
        fillers.push_back(fd);
    }
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_ACCOUNT_CREATE, pair.provider, request));
    descriptor_close(table(), fillers.back());
    fillers.pop_back();
    long root = call(OS1_IPC_ACCOUNT_CREATE, pair.provider, request);
    ASSERT_GE(root, 0);  // Failed root installation did not burn the one creation.
    descriptor_close(table(), fillers.back());
    fillers.pop_back();
    os1_ipc_pair result{};
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SESSION_CREATE, root, result));
    descriptor_close(table(), fillers.back());
    fillers.pop_back();
    EXPECT_EQ(0, call(OS1_IPC_SESSION_CREATE, root, result));
}

TEST_F(Ipc, ConcurrentSessionAcquisitionCannotOverdrawAnAliasedAccount)
{
    os1_ipc_account request{1, sizeof(request), 1, 0};
    long root = call(OS1_IPC_ACCOUNT_CREATE, pair.provider, request);
    ASSERT_GE(root, 0);
    long duplicate = alias(root, OS1_RIGHT_READ | OS1_RIGHT_WRITE);
    ASSERT_GE(duplicate, 0);
    std::atomic<bool> go{false};
    long peer_status = OS1_ERR_INVALID;
    std::thread peer([&] {
        while(!go.load())
            std::this_thread::yield();
        peer_status = sys_ipc(frames, thread, OS1_IPC_SESSION_CREATE, duplicate, address + 512);
    });
    go = true;
    long own_status = sys_ipc(frames, thread, OS1_IPC_SESSION_CREATE, root, address);
    peer.join();
    EXPECT_TRUE((own_status == 0 && peer_status == OS1_ERR_LIMIT) ||
                (peer_status == 0 && own_status == OS1_ERR_LIMIT));
    os1_ipc_pair made{};
    ASSERT_TRUE(copy_from_user(
        frames, thread, own_status == 0 ? address : address + 512, &made, sizeof(made)));
    descriptor_close(table(), made.client);
    descriptor_close(table(), made.provider);
    EXPECT_EQ(0, call(OS1_IPC_SESSION_CREATE, root, made));
}
TEST_F(Ipc, WaitManyStopAndSignalledCancellationReleaseEveryPin)
{
    os1_ipc_wait_many w{1, sizeof(w), 2, 0, UINT64_MAX, {pair.client, pair.provider}};
    ASSERT_EQ(kIpcBlocked, call(OS1_IPC_WAIT_MANY, 0, w));
    OpenInstance* first = thread->wait.ipc.instances[0];
    OpenInstance* second = thread->wait.ipc.instances[1];
    thread->stop_requested = true;
    ipc_timer_tick(1);
    EXPECT_EQ(ThreadState::Blocked, thread->state);  // Already selected, still on CPU.
    EXPECT_EQ(OS1_ERR_CANCELLED, thread->wait.ipc.result);
    cancel_ipc_wait(thread);  // Must unlink deferred signal as well as release both pins.
    EXPECT_EQ(1u, first->references);
    EXPECT_EQ(1u, second->references);
    EXPECT_EQ(ThreadWaitReason::None, thread->wait.reason);
    set_current_thread(nullptr);
    ipc_drain_wakeups();
    thread->stop_requested = false;
}
class IpcBuffers : public Ipc
{
protected:
    Thread* caller = nullptr;
    Thread* provider = nullptr;
    int provider_fd = -1;
    void use(Thread* selected)
    {
        thread = selected;
        set_current_thread(thread);
    }
    void SetUp() override
    {
        Ipc::SetUp();
        caller = thread;
        ASSERT_EQ(0, ready());
        VirtualMemory vm(frames);
        ASSERT_TRUE(vm.allocate_and_map(address, 1, PageFlags::User | PageFlags::Write));
        auto* process = create_user_process("buffer-provider", vm.root());
        ASSERT_NE(nullptr, process);
        provider = create_user_thread(process, address, address + 4096, frames, false);
        ASSERT_NE(nullptr, provider);
        OpenInstance* raw = nullptr;
        ASSERT_EQ(0, descriptor_acquire(table(), pair.provider, OS1_RIGHT_READ, raw));
        InstanceRef pin(raw);
        provider_fd = descriptor_install(
            process->descriptors,
            raw,
            OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE,
            OS1_DESCRIPTOR_CLOEXEC);
        ASSERT_GE(provider_fd, 0);
        ASSERT_EQ(0, descriptor_close(table(), pair.provider));
    }
    void TearDown() override
    {
        if(provider)
        {
            use(provider);
            cancel_ipc_wait(provider);
            descriptor_close_all(table());
            buffer_process_cleanup(frames, provider->process);
        }
        if(caller)
        {
            use(caller);
            descriptor_close_all(table());
            buffer_process_cleanup(frames, caller->process);
        }
        set_current_thread(nullptr);
    }
    template<class T>
    long buffer_call(unsigned operation, int fd, T& value)
    {
        EXPECT_TRUE(copy_to_user(frames, thread, address, &value, sizeof(value)));
        long status = sys_buffer(frames, thread, fd, operation, address);
        EXPECT_TRUE(copy_from_user(frames, thread, address, &value, sizeof(value)));
        return status;
    }
    long create_buffer()
    {
        os1_buffer_create value{1, sizeof(value), 4096, OS1_BUFFER_EXCLUSIVE, 0};
        return buffer_call(OS1_RESOURCE_BUFFER_CREATE, 0, value);
    }
    long map_buffer(int fd, uint64_t& mapped)
    {
        os1_buffer_map value{1, sizeof(value), 0, 1, 0};
        long status = buffer_call(OS1_RESOURCE_BUFFER_MAP, fd, value);
        mapped = value.address;
        return status;
    }
    os1_ipc_message_buffers loan(int fd)
    {
        os1_ipc_message_buffers value{};
        value.message = request();
        value.buffers.count = 1;
        value.buffers.entries[0] = {fd, OS1_BUFFER_LOAN};
        return value;
    }
};
TEST_F(IpcBuffers, SameBufferReturnRetiresAliasesAndClaimsOnce)
{
    long fd = create_buffer();
    ASSERT_GE(fd, 0);
    uint64_t original = 0;
    ASSERT_EQ(0, map_buffer(fd, original));
    const uint8_t initial = 17;
    ASSERT_TRUE(copy_to_user(frames, thread, original, &initial, 1));
    long owner_alias =
        alias(fd, OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE);
    ASSERT_GE(owner_alias, 0);
    auto input = loan(fd);
    long operation = call(OS1_IPC_SUBMIT_BUFFERS, pair.client, input);
    ASSERT_GE(operation, 0);
    uint64_t denied = 0;
    EXPECT_EQ(OS1_ERR_ACCESS, map_buffer(owner_alias, denied));
    uint8_t byte = 0;
    EXPECT_FALSE(copy_from_user(frames, thread, original, &byte, 1));
    use(provider);
    os1_ipc_message legacy{};
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, call(OS1_IPC_RECEIVE, provider_fd, legacy));
    os1_ipc_message_buffers received{};
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_RECEIVE_BUFFERS, provider_fd, 0x1000));
    ASSERT_EQ(0, call(OS1_IPC_RECEIVE_BUFFERS, provider_fd, received));
    ASSERT_EQ(1u, received.buffers.count);
    const int borrowed = received.buffers.entries[0].fd;
    uint64_t mapped = 0;
    ASSERT_EQ(0, map_buffer(borrowed, mapped));
    ASSERT_TRUE(copy_from_user(frames, thread, mapped, &byte, 1));
    EXPECT_EQ(initial, byte);
    const uint8_t changed = 91;
    ASSERT_TRUE(copy_to_user(frames, thread, mapped, &changed, 1));
    os1_ipc_result_buffers reply{};
    reply.result = response(received.message);
    reply.result.application_status = -7;
    reply.buffers.count = 1;
    reply.buffers.entries[0] = {borrowed, OS1_BUFFER_MOVE};
    EXPECT_EQ(OS1_ERR_ACCESS, call(OS1_IPC_REPLY_BUFFERS, provider_fd, reply));
    EXPECT_TRUE(copy_from_user(frames, thread, mapped, &byte, 1));
    auto legacy_reply = reply.result;
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, call(OS1_IPC_REPLY, provider_fd, legacy_reply));
    reply.buffers.entries[0].disposition = OS1_BUFFER_RETURN;
    ASSERT_EQ(0, call(OS1_IPC_REPLY_BUFFERS, provider_fd, reply));
    EXPECT_FALSE(copy_from_user(frames, thread, mapped, &byte, 1));
    EXPECT_EQ(OS1_ERR_ACCESS, map_buffer(borrowed, denied));
    EXPECT_EQ(OS1_ERR_STALE_REPLY, call(OS1_IPC_REPLY_BUFFERS, provider_fd, reply));
    use(caller);
    ASSERT_TRUE(copy_from_user(frames, thread, original, &byte, 1));
    EXPECT_EQ(changed, byte);
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_RESULT_BUFFERS, operation, 0x1000));
    os1_ipc_result_buffers result{};
    ASSERT_EQ(0, call(OS1_IPC_RESULT_BUFFERS, operation, result));
    EXPECT_EQ(-7, result.result.application_status);
    ASSERT_EQ(1u, result.buffers.count);
    os1_buffer_info before{1, sizeof(before)};
    os1_buffer_info after{1, sizeof(after)};
    ASSERT_EQ(0, buffer_call(OS1_RESOURCE_BUFFER_INFO, fd, before));
    ASSERT_EQ(0, buffer_call(OS1_RESOURCE_BUFFER_INFO, result.buffers.entries[0].fd, after));
    EXPECT_EQ(before.identity, after.identity);
    long operation_alias = alias(operation, OS1_RIGHT_READ);
    ASSERT_GE(operation_alias, 0);
    ASSERT_EQ(0, call(OS1_IPC_RESULT_BUFFERS, operation_alias, result));
    EXPECT_EQ(0u, result.buffers.count);
}
TEST_F(IpcBuffers, DistinctOutputReturnsInputAndRetainsEscrowUntilClaim)
{
    long input_fd = create_buffer();
    ASSERT_GE(input_fd, 0);
    uint64_t original = 0;
    ASSERT_EQ(0, map_buffer(input_fd, original));
    auto input = loan(input_fd);
    long operation = call(OS1_IPC_SUBMIT_BUFFERS, pair.client, input);
    ASSERT_GE(operation, 0);
    use(provider);
    os1_ipc_message_buffers received{};
    ASSERT_EQ(0, call(OS1_IPC_RECEIVE_BUFFERS, provider_fd, received));
    long output_fd = create_buffer();
    ASSERT_GE(output_fd, 0);
    uint64_t output_address = 0;
    ASSERT_EQ(0, map_buffer(output_fd, output_address));
    uint8_t byte = 42;
    ASSERT_TRUE(copy_to_user(frames, thread, output_address, &byte, 1));
    os1_ipc_result_buffers reply{};
    reply.result = response(received.message);
    reply.buffers.count = 1;
    reply.buffers.entries[0] = {static_cast<int32_t>(output_fd), OS1_BUFFER_MOVE};
    ASSERT_EQ(0, call(OS1_IPC_REPLY_BUFFERS, provider_fd, reply));
    EXPECT_FALSE(copy_from_user(frames, thread, output_address, &byte, 1));
    use(caller);
    EXPECT_TRUE(copy_from_user(frames, thread, original, &byte, 1));
    std::vector<int> full;
    for(;;)
    {
        long extra = alias(operation, OS1_RIGHT_READ);
        if(extra < 0)
            break;
        full.push_back(extra);
    }
    os1_ipc_result_buffers result{};
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_RESULT_BUFFERS, operation, result));
    ASSERT_FALSE(full.empty());
    ASSERT_EQ(0, descriptor_close(table(), full.back()));
    full.pop_back();
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_RESULT_BUFFERS, operation, 0x1000));
    ASSERT_EQ(0, call(OS1_IPC_RESULT_BUFFERS, operation, result));
    ASSERT_EQ(1u, result.buffers.count);
    uint64_t claimed = 0;
    ASSERT_EQ(0, map_buffer(result.buffers.entries[0].fd, claimed));
    ASSERT_TRUE(copy_from_user(frames, thread, claimed, &byte, 1));
    EXPECT_EQ(42, byte);
    ASSERT_EQ(0, call(OS1_IPC_RESULT_BUFFERS, operation, result));
    EXPECT_EQ(0u, result.buffers.count);
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_RESULT_BUFFERS, operation, 0x1000));
    ASSERT_TRUE(copy_from_user(frames, thread, claimed, &byte, 1));
    EXPECT_EQ(42, byte);
}
TEST_F(IpcBuffers, LiveProviderClosureRestoresDeliveredInputBeforeCompletion)
{
    long fd = create_buffer();
    ASSERT_GE(fd, 0);
    uint64_t original = 0;
    ASSERT_EQ(0, map_buffer(fd, original));
    auto input = loan(fd);
    long operation = call(OS1_IPC_SUBMIT_BUFFERS, pair.client, input);
    ASSERT_GE(operation, 0);
    use(provider);
    os1_ipc_message_buffers received{};
    ASSERT_EQ(0, call(OS1_IPC_RECEIVE_BUFFERS, provider_fd, received));
    uint64_t borrowed = 0;
    ASSERT_EQ(0, map_buffer(received.buffers.entries[0].fd, borrowed));
    uint8_t byte = 23;
    ASSERT_TRUE(copy_to_user(frames, thread, borrowed, &byte, 1));
    ASSERT_EQ(0, descriptor_close(table(), provider_fd));
    EXPECT_FALSE(copy_from_user(frames, thread, borrowed, &byte, 1));
    use(caller);
    os1_ipc_result_buffers result{};
    ASSERT_EQ(0, call(OS1_IPC_RESULT_BUFFERS, operation, result));
    EXPECT_EQ(OS1_ERR_OUTCOME_UNKNOWN, result.result.transport_status);
    EXPECT_EQ(0u, result.buffers.count);
    ASSERT_TRUE(copy_from_user(frames, thread, original, &byte, 1));
    EXPECT_EQ(23, byte);
}
TEST_F(IpcBuffers, TimeoutAndClosedOperationKeepUndeliveredLoan)
{
    long fd = create_buffer();
    ASSERT_GE(fd, 0);
    uint64_t original = 0;
    ASSERT_EQ(0, map_buffer(fd, original));
    auto input = loan(fd);
    long operation = call(OS1_IPC_SUBMIT_BUFFERS, pair.client, input);
    ASSERT_GE(operation, 0);
    os1_ipc_deadline deadline{1, sizeof(deadline), 0};
    EXPECT_EQ(OS1_ERR_TIMED_OUT, call(OS1_IPC_WAIT_UNTIL, operation, deadline));
    ASSERT_EQ(0, descriptor_close(table(), operation));
    uint8_t byte = 0;
    EXPECT_FALSE(copy_from_user(frames, thread, original, &byte, 1));
    use(provider);
    ASSERT_EQ(0, descriptor_close(table(), provider_fd));
    use(caller);
    EXPECT_TRUE(copy_from_user(frames, thread, original, &byte, 1));
}
TEST_F(IpcBuffers, LegacyCapabilitiesAndDuplicateLoanRejectWithoutRevocation)
{
    long fd = create_buffer();
    ASSERT_GE(fd, 0);
    uint64_t original = 0;
    ASSERT_EQ(0, map_buffer(fd, original));
    os1_ipc_message_caps caps{};
    caps.message = request();
    caps.capabilities.count = 1;
    caps.capabilities.entries[0] = {static_cast<int32_t>(fd), OS1_RIGHT_READ, 0, 0};
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, call(OS1_IPC_SUBMIT_CAPS, pair.client, caps));
    auto input = loan(fd);
    input.buffers.count = 2;
    input.buffers.entries[1] = input.buffers.entries[0];
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_SUBMIT_BUFFERS, pair.client, input));
    uint8_t byte = 0;
    EXPECT_TRUE(copy_from_user(frames, thread, original, &byte, 1));
    use(provider);
    os1_ipc_message_buffers received{};
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, call(OS1_IPC_RECEIVE_BUFFERS, provider_fd, received));
}
TEST_F(IpcBuffers, ReceiveDescriptorExhaustionPreservesWholeBatch)
{
    long first = create_buffer();
    long second = create_buffer();
    ASSERT_GE(first, 0);
    ASSERT_GE(second, 0);
    auto input = loan(first);
    input.buffers.count = 2;
    input.buffers.entries[1] = {static_cast<int32_t>(second), OS1_BUFFER_LOAN};
    long operation = call(OS1_IPC_SUBMIT_BUFFERS, pair.client, input);
    ASSERT_GE(operation, 0);
    use(provider);
    std::vector<int> full;
    for(;;)
    {
        long extra = alias(provider_fd, OS1_RIGHT_READ);
        if(extra < 0)
            break;
        full.push_back(extra);
    }
    ASSERT_GE(full.size(), 2u);
    descriptor_close(table(), full.back());
    full.pop_back();
    os1_ipc_message_buffers received{};
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_RECEIVE_BUFFERS, provider_fd, received));
    descriptor_close(table(), full.back());
    full.pop_back();
    ASSERT_EQ(0, call(OS1_IPC_RECEIVE_BUFFERS, provider_fd, received));
    EXPECT_EQ(2u, received.buffers.count);
    os1_ipc_result_buffers reply{};
    reply.result = response(received.message);
    ASSERT_EQ(0, call(OS1_IPC_REPLY_BUFFERS, provider_fd, reply));
    use(caller);
    os1_ipc_result_buffers result{};
    ASSERT_EQ(0, call(OS1_IPC_RESULT_BUFFERS, operation, result));
    uint64_t first_map = 0, second_map = 0;
    EXPECT_EQ(0, map_buffer(first, first_map));
    EXPECT_EQ(0, map_buffer(second, second_map));
}
TEST_F(IpcBuffers, QueueExhaustionDoesNotRevokeOwner)
{
    long fd = create_buffer();
    ASSERT_GE(fd, 0);
    uint64_t original = 0;
    ASSERT_EQ(0, map_buffer(fd, original));
    for(unsigned i = 0; i < OS1_IPC_OPERATION_LIMIT; ++i)
        ASSERT_GE(submit(), 0);
    auto input = loan(fd);
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SUBMIT_BUFFERS, pair.client, input));
    uint8_t byte = 0;
    EXPECT_TRUE(copy_from_user(frames, thread, original, &byte, 1));
}
TEST_F(IpcBuffers, ConcurrentOperationAliasesClaimDistinctOutputOnce)
{
    os1_ipc_message_buffers input{};
    input.message = request();
    long operation = call(OS1_IPC_SUBMIT_BUFFERS, pair.client, input);
    ASSERT_GE(operation, 0);
    use(provider);
    os1_ipc_message_buffers received{};
    ASSERT_EQ(0, call(OS1_IPC_RECEIVE_BUFFERS, provider_fd, received));
    long fd = create_buffer();
    ASSERT_GE(fd, 0);
    os1_ipc_result_buffers reply{};
    reply.result = response(received.message);
    reply.buffers.count = 1;
    reply.buffers.entries[0] = {static_cast<int32_t>(fd), OS1_BUFFER_MOVE};
    ASSERT_EQ(0, call(OS1_IPC_REPLY_BUFFERS, provider_fd, reply));
    use(caller);
    long duplicate = alias(operation, OS1_RIGHT_READ);
    ASSERT_GE(duplicate, 0);
    std::atomic<unsigned> entered{0};
    long status[2]{};
    auto claim = [&](unsigned index, long descriptor) {
        entered.fetch_add(1);
        while(entered.load() < 2)
            std::this_thread::yield();
        status[index] = sys_ipc(
            frames, caller, OS1_IPC_RESULT_BUFFERS, descriptor, address + 512 + index * 512);
    };
    std::thread left(claim, 0, operation);
    std::thread right(claim, 1, duplicate);
    left.join();
    right.join();
    unsigned count = 0;
    for(unsigned i = 0; i < 2; ++i)
    {
        if(status[i] == OS1_ERR_WOULD_BLOCK)
            status[i] =
                sys_ipc(frames, caller, OS1_IPC_RESULT_BUFFERS, operation, address + 512 + i * 512);
        ASSERT_EQ(0, status[i]);
        os1_ipc_result_buffers result{};
        ASSERT_TRUE(
            copy_from_user(frames, caller, address + 512 + i * 512, &result, sizeof(result)));
        count += result.buffers.count;
    }
    EXPECT_EQ(1u, count);
}
std::atomic<bool> buffer_transition_entered{false};
std::atomic<bool> buffer_transition_continue{false};
void pause_buffer_transition()
{
    buffer_transition_entered.store(true);
    while(!buffer_transition_continue.load())
        std::this_thread::yield();
}
TEST_F(IpcBuffers, ProviderCloseDuringReservedSubmitCancelsBeforePublication)
{
    long fd = create_buffer();
    ASSERT_GE(fd, 0);
    uint64_t original = 0;
    ASSERT_EQ(0, map_buffer(fd, original));
    auto input = loan(fd);
    ASSERT_TRUE(copy_to_user(frames, caller, address, &input, sizeof(input)));
    buffer_transition_entered = false;
    buffer_transition_continue = false;
    host_buffer_before_prepare = pause_buffer_transition;
    long status = 0;
    std::thread submitting(
        [&] { status = sys_ipc(frames, caller, OS1_IPC_SUBMIT_BUFFERS, pair.client, address); });
    while(!buffer_transition_entered.load())
        std::this_thread::yield();
    // The reserved request remains hidden even before revocation begins.
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK,
              sys_ipc(frames, provider, OS1_IPC_RECEIVE_BUFFERS, provider_fd, address));
    EXPECT_EQ(0, descriptor_close(provider->process->descriptors, provider_fd));
    buffer_transition_continue = true;
    submitting.join();
    host_buffer_before_prepare = nullptr;
    EXPECT_EQ(OS1_ERR_PEER_CLOSED, status);
    uint8_t byte = 0;
    EXPECT_TRUE(copy_from_user(frames, caller, original, &byte, 1));
    os1_buffer_info info{1, sizeof(info)};
    ASSERT_EQ(0, buffer_call(OS1_RESOURCE_BUFFER_INFO, fd, info));
    EXPECT_EQ(1u, info.owned);
}
TEST_F(IpcBuffers, ProviderCloseDuringDeliveryWaitsForPinnedTransition)
{
    long fd = create_buffer();
    ASSERT_GE(fd, 0);
    uint64_t original = 0;
    ASSERT_EQ(0, map_buffer(fd, original));
    auto input = loan(fd);
    long operation = call(OS1_IPC_SUBMIT_BUFFERS, pair.client, input);
    ASSERT_GE(operation, 0);
    buffer_transition_entered = false;
    buffer_transition_continue = false;
    host_ipc_before_receive = pause_buffer_transition;
    long status = 0;
    std::thread receiving(
        [&] { status = sys_ipc(frames, provider, OS1_IPC_RECEIVE_BUFFERS, provider_fd, address); });
    while(!buffer_transition_entered.load())
        std::this_thread::yield();
    EXPECT_EQ(0, descriptor_close(provider->process->descriptors, provider_fd));
    os1_ipc_result_buffers result{};
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, call(OS1_IPC_RESULT_BUFFERS, operation, result));
    uint8_t byte = 0;
    EXPECT_FALSE(copy_from_user(frames, caller, original, &byte, 1));
    buffer_transition_continue = true;
    receiving.join();
    host_ipc_before_receive = nullptr;
    EXPECT_EQ(0, status);
    // The final endpoint pin triggers recovery without another provider syscall.
    EXPECT_TRUE(copy_from_user(frames, caller, original, &byte, 1));
    ASSERT_EQ(0, call(OS1_IPC_RESULT_BUFFERS, operation, result));
    EXPECT_EQ(OS1_ERR_OUTCOME_UNKNOWN, result.result.transport_status);
}
TEST_F(IpcBuffers, SameProcessLoanRestoresOwnerRoleAndRevokesBorrowerRole)
{
    OpenInstance* raw = nullptr;
    ASSERT_EQ(0,
              descriptor_acquire(provider->process->descriptors, provider_fd, OS1_RIGHT_READ, raw));
    InstanceRef pin(raw);
    long local_provider = descriptor_install(
        table(),
        raw,
        OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT | OS1_RIGHT_DELEGATE,
        OS1_DESCRIPTOR_CLOEXEC);
    ASSERT_GE(local_provider, 0);
    ASSERT_EQ(0, descriptor_close(provider->process->descriptors, provider_fd));
    long fd = create_buffer();
    ASSERT_GE(fd, 0);
    uint64_t original = 0;
    ASSERT_EQ(0, map_buffer(fd, original));
    auto input = loan(fd);
    long operation = call(OS1_IPC_SUBMIT_BUFFERS, pair.client, input);
    ASSERT_GE(operation, 0);
    uint8_t byte = 0;
    EXPECT_FALSE(copy_from_user(frames, thread, original, &byte, 1));
    os1_ipc_message_buffers received{};
    ASSERT_EQ(0, call(OS1_IPC_RECEIVE_BUFFERS, local_provider, received));
    ASSERT_EQ(1u, received.buffers.count);
    uint64_t owner_alias_mapping = 0;
    EXPECT_EQ(OS1_ERR_ACCESS, map_buffer(fd, owner_alias_mapping));
    uint64_t borrowed = 0;
    ASSERT_EQ(0, map_buffer(received.buffers.entries[0].fd, borrowed));
    EXPECT_NE(original, borrowed);
    byte = 61;
    ASSERT_TRUE(copy_to_user(frames, thread, borrowed, &byte, 1));
    os1_ipc_result_buffers reply{};
    reply.result = response(received.message);
    reply.buffers.count = 1;
    reply.buffers.entries[0] = {received.buffers.entries[0].fd, OS1_BUFFER_RETURN};
    ASSERT_EQ(0, call(OS1_IPC_REPLY_BUFFERS, local_provider, reply));
    EXPECT_FALSE(copy_from_user(frames, thread, borrowed, &byte, 1));
    ASSERT_TRUE(copy_from_user(frames, thread, original, &byte, 1));
    EXPECT_EQ(61, byte);
    uint64_t denied = 0;
    EXPECT_EQ(OS1_ERR_ACCESS, map_buffer(received.buffers.entries[0].fd, denied));
    os1_ipc_result_buffers result{};
    ASSERT_EQ(0, call(OS1_IPC_RESULT_BUFFERS, operation, result));
    EXPECT_EQ(1u, result.buffers.count);
}
}  // namespace

TEST_F(Ipc, MeasurementIsSeparateFromDeadlineClockAndHasExplicitUnavailability)
{
    const auto saved = g_platform.hpet;
    g_platform.hpet = {};
    const long deadline_before = sys_ipc(frames, thread, OS1_IPC_CLOCK, 0, 0);
    os1_measurement result{};
    EXPECT_EQ(0, call(OS1_IPC_MEASURE, 0, result));
    EXPECT_EQ(1u, result.version);
    EXPECT_EQ(sizeof(result), result.size);
    EXPECT_EQ(OS1_MEASUREMENT_UNAVAILABLE, result.source);
    EXPECT_EQ(0u, result.nanoseconds);
    EXPECT_EQ(0u, result.resolution_ns);
    EXPECT_EQ(0u, result.reserved);
    EXPECT_EQ(deadline_before, sys_ipc(frames, thread, OS1_IPC_CLOCK, 0, 0));
    EXPECT_EQ(OS1_ERR_INVALID, sys_ipc(frames, thread, OS1_IPC_MEASURE, 1, address));
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_MEASURE, 0, 0));
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_MEASURE, 0, address + 4096 - 1));
    g_platform.hpet = saved;
}

void host_console_output_clear();
const std::string& host_console_output();
namespace
{
class KernelConsole : public Ipc
{
protected:
    void SetUp() override
    {
        Ipc::SetUp();
        ASSERT_EQ(0, descriptor_close(table(), pair.client));
        ASSERT_EQ(0, descriptor_close(table(), pair.provider));
        ASSERT_EQ(0, descriptor_bootstrap_console(table()));
        console_input_initialize();
        host_console_output_clear();
    }
    void TearDown() override
    {
        while(ipc_run_kernel_request())
        {
        }
        Ipc::TearDown();
    }
    long bind(int source = 1, uint32_t rights = OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT)
    {
        os1_ipc_kernel_binding request{1, sizeof(request), OS1_CONSOLE_INTERFACE_ID, 1, rights};
        return call(OS1_IPC_BIND_KERNEL, source, request);
    }
    os1_ipc_message console_request(unsigned member = OS1_CONSOLE_WRITE)
    {
        auto m = request();
        m.interface_id = OS1_CONSOLE_INTERFACE_ID;
        m.member = member;
        if(member != OS1_CONSOLE_WRITE)
        {
            m.payload_bytes = 0;
            std::memset(m.payload, 0, sizeof(m.payload));
        }
        return m;
    }
    os1_ipc_result complete(int operation)
    {
        EXPECT_TRUE(ipc_run_kernel_request());
        os1_ipc_result result{};
        EXPECT_EQ(0, call(OS1_IPC_RESULT, operation, result));
        EXPECT_EQ(0, result.transport_status);
        return result;
    }
};
TEST_F(KernelConsole, BindingShapeRightsAndReadyIdentity)
{
    EXPECT_EQ(24u, sizeof(os1_ipc_kernel_binding));
    EXPECT_EQ(16u, sizeof(os1_console_info));
    EXPECT_EQ(4u, sizeof(os1_console_read));
    EXPECT_EQ(4u, sizeof(os1_console_written));
    EXPECT_EQ(OS1_ERR_BAD_DESCRIPTOR, bind(64));
    EXPECT_EQ(OS1_ERR_BAD_DESCRIPTOR, bind(63));
    EXPECT_EQ(OS1_ERR_ACCESS, bind(0, OS1_RIGHT_WRITE));
    EXPECT_EQ(OS1_ERR_INVALID, bind(1, 0));
    EXPECT_EQ(OS1_ERR_INVALID, bind(1, OS1_RIGHT_DELEGATE));
    os1_ipc_kernel_binding b{1, sizeof(b), OS1_CONSOLE_INTERFACE_ID, 1, OS1_RIGHT_WRITE};
    b.version = 2;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_BIND_KERNEL, 1, b));
    b.version = 1;
    b.size--;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_BIND_KERNEL, 1, b));
    b.size++;
    b.interface_version = 2;
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, call(OS1_IPC_BIND_KERNEL, 1, b));
    b.interface_version = 1;
    b.interface_id++;
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, call(OS1_IPC_BIND_KERNEL, 1, b));
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_BIND_KERNEL, 1, 0));
    int client = bind();
    ASSERT_GE(client, 0);
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, bind(client));
    os1_ipc_info info{};
    ASSERT_EQ(0, call(OS1_IPC_INFO, client, info));
    EXPECT_EQ(OS1_CONSOLE_INTERFACE_ID, info.interface_id);
    EXPECT_EQ(1u, info.interface_version);
    EXPECT_EQ(1u, info.ready);
    EXPECT_EQ(0, sys_ipc(frames, thread, OS1_IPC_WAIT, client, 0));
    os1_resource_info resource{};
    ASSERT_EQ(0, descriptor_query(table(), client, resource));
    EXPECT_EQ(OS1_DESCRIPTOR_CLOEXEC, resource.descriptor_flags);
}
TEST_F(KernelConsole, CopiedAsyncRequestRetainedResultAndClosedSource)
{
    int client = bind();
    ASSERT_GE(client, 0);
    auto m = console_request();
    int operation = call(OS1_IPC_SUBMIT, client, m);
    ASSERT_GE(operation, 0);
    EXPECT_TRUE(host_console_output().empty());
    os1_ipc_result result{};
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, call(OS1_IPC_RESULT, operation, result));
    std::memset(m.payload, 'z', 3);
    ASSERT_EQ(0, descriptor_close(table(), 1));
    ASSERT_EQ(0, descriptor_close(table(), client));
    result = complete(operation);
    EXPECT_EQ("ABC", host_console_output());
    EXPECT_EQ(0, result.application_status);
    EXPECT_EQ(4u, result.payload_bytes);
    os1_console_written written{};
    std::memcpy(&written, result.payload, sizeof(written));
    EXPECT_EQ(3u, written.bytes);
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_RESULT, operation, 0));
    os1_ipc_result again{};
    ASSERT_EQ(0, call(OS1_IPC_RESULT, operation, again));
    EXPECT_EQ(0, std::memcmp(&result, &again, sizeof(result)));
    EXPECT_FALSE(ipc_run_kernel_request());
}
TEST_F(KernelConsole, MethodAuthorityDoesNotFollowTransportWrite)
{
    int client = bind(1, OS1_RIGHT_INSPECT);
    ASSERT_GE(client, 0);
    auto m = console_request();
    int operation = call(OS1_IPC_SUBMIT, client, m);
    ASSERT_GE(operation, 0);
    EXPECT_EQ(OS1_ERR_ACCESS, complete(operation).application_status);
    EXPECT_TRUE(host_console_output().empty());
    descriptor_close(table(), operation);
    m = console_request(OS1_CONSOLE_GET_INFO);
    operation = call(OS1_IPC_SUBMIT, client, m);
    ASSERT_GE(operation, 0);
    auto result = complete(operation);
    EXPECT_EQ(0, result.application_status);
    os1_console_info info{};
    std::memcpy(&info, result.payload, sizeof(info));
    EXPECT_EQ(OS1_RIGHT_INSPECT, info.rights);
    EXPECT_EQ(OS1_CONSOLE_MAX_BYTES, info.max_write_bytes);
    EXPECT_EQ(0u, info.max_read_bytes);
    EXPECT_EQ(0u, info.flags);
    descriptor_close(table(), operation);
    int no_inspect = bind(1, OS1_RIGHT_WRITE);
    ASSERT_GE(no_inspect, 0);
    operation = call(OS1_IPC_SUBMIT, no_inspect, m);
    ASSERT_GE(operation, 0);
    EXPECT_EQ(OS1_ERR_ACCESS, complete(operation).application_status);
}
TEST_F(KernelConsole, DelegationCannotBeRecoveredByBinding)
{
    int source = alias(1, OS1_RIGHT_WRITE);
    ASSERT_GE(source, 0);
    int client = bind(source, OS1_RIGHT_WRITE);
    ASSERT_GE(client, 0);
    EXPECT_EQ(OS1_ERR_ACCESS, alias(client, OS1_RIGHT_READ | OS1_RIGHT_WRITE));
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, bind(client, OS1_RIGHT_WRITE));
    int readonly = alias(1, OS1_RIGHT_INSPECT);
    ASSERT_GE(readonly, 0);
    EXPECT_EQ(OS1_ERR_ACCESS, bind(readonly, OS1_RIGHT_WRITE));
}
TEST_F(KernelConsole, EofIsRetainedAcrossResultFaultWithoutConsumingNextInput)
{
    int client = bind(0, OS1_RIGHT_READ);
    ASSERT_GE(client, 0);
    auto m = console_request(OS1_CONSOLE_TRY_READ);
    os1_console_read read{8};
    m.payload_bytes = sizeof(read);
    std::memcpy(m.payload, &read, sizeof(read));
    console_input_on_keyboard_char(4);
    console_input_on_keyboard_char('x');
    console_input_on_keyboard_char('\n');
    int op = call(OS1_IPC_SUBMIT, client, m);
    ASSERT_GE(op, 0);
    ASSERT_TRUE(ipc_run_kernel_request());
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_RESULT, op, 0));
    os1_ipc_result result{};
    ASSERT_EQ(0, call(OS1_IPC_RESULT, op, result));
    EXPECT_EQ(0, result.application_status);
    EXPECT_EQ(0u, result.payload_bytes);
    ASSERT_EQ(0, call(OS1_IPC_RESULT, op, result));
    descriptor_close(table(), op);
    op = call(OS1_IPC_SUBMIT, client, m);
    ASSERT_GE(op, 0);
    result = complete(op);
    EXPECT_EQ(2u, result.payload_bytes);
    EXPECT_EQ('x', result.payload[0]);
}
TEST_F(KernelConsole, OverlongInputRetainsRangeErrorAndPreservesFollowingLine)
{
    int client = bind(0, OS1_RIGHT_READ);
    ASSERT_GE(client, 0);
    auto m = console_request(OS1_CONSOLE_TRY_READ);
    os1_console_read read{8};
    m.payload_bytes = sizeof(read);
    std::memcpy(m.payload, &read, sizeof(read));
    for(unsigned i = 0; i < 140; ++i)
        console_input_on_keyboard_char('x');
    console_input_on_keyboard_char('\n');
    console_input_on_keyboard_char('y');
    console_input_on_keyboard_char('\n');
    int op = call(OS1_IPC_SUBMIT, client, m);
    ASSERT_GE(op, 0);
    ASSERT_TRUE(ipc_run_kernel_request());
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_RESULT, op, 0));
    os1_ipc_result result{};
    ASSERT_EQ(0, call(OS1_IPC_RESULT, op, result));
    EXPECT_EQ(OS1_ERR_RANGE, result.application_status);
    EXPECT_EQ(0u, result.payload_bytes);
    ASSERT_EQ(0, call(OS1_IPC_RESULT, op, result));
    EXPECT_EQ(OS1_ERR_RANGE, result.application_status);
    descriptor_close(table(), op);
    op = call(OS1_IPC_SUBMIT, client, m);
    ASSERT_GE(op, 0);
    result = complete(op);
    EXPECT_EQ(2u, result.payload_bytes);
    EXPECT_EQ('y', result.payload[0]);
}
TEST_F(KernelConsole, TryReadRetainsConsumedInputAcrossResultFaultAndRetry)
{
    int client = bind(0, OS1_RIGHT_READ | OS1_RIGHT_INSPECT);
    ASSERT_GE(client, 0);
    auto query = console_request(OS1_CONSOLE_GET_INFO);
    int query_op = call(OS1_IPC_SUBMIT, client, query);
    ASSERT_GE(query_op, 0);
    auto query_result = complete(query_op);
    os1_console_info info{};
    ASSERT_EQ(sizeof(info), query_result.payload_bytes);
    std::memcpy(&info, query_result.payload, sizeof(info));
    EXPECT_EQ(0u, info.max_write_bytes);
    EXPECT_EQ(128u, info.max_read_bytes);
    EXPECT_EQ(OS1_CONSOLE_CANONICAL_INPUT | OS1_CONSOLE_EOF_INPUT, info.flags);
    descriptor_close(table(), query_op);
    auto m = console_request(OS1_CONSOLE_TRY_READ);
    os1_console_read read{2};
    m.payload_bytes = sizeof(read);
    std::memcpy(m.payload, &read, sizeof(read));
    int op = call(OS1_IPC_SUBMIT, client, m);
    ASSERT_GE(op, 0);
    EXPECT_EQ(OS1_ERR_WOULD_BLOCK, complete(op).application_status);
    descriptor_close(table(), op);
    console_input_on_keyboard_char('a');
    console_input_on_keyboard_char('b');
    console_input_on_keyboard_char('\n');
    op = call(OS1_IPC_SUBMIT, client, m);
    ASSERT_GE(op, 0);
    ASSERT_TRUE(ipc_run_kernel_request());
    EXPECT_EQ(OS1_ERR_FAULT, sys_ipc(frames, thread, OS1_IPC_RESULT, op, address + 4095));
    os1_ipc_result result{};
    ASSERT_EQ(0, call(OS1_IPC_RESULT, op, result));
    EXPECT_EQ(0, result.application_status);
    EXPECT_EQ(2u, result.payload_bytes);
    EXPECT_EQ(0, std::memcmp(result.payload, "ab", 2));
    ASSERT_EQ(0, call(OS1_IPC_RESULT, op, result));
    char remaining[4]{};
    size_t count = 0;
    ASSERT_TRUE(console_input_reserve(remaining, sizeof(remaining), count));
    EXPECT_EQ(1u, count);
    EXPECT_EQ('\n', remaining[0]);
    console_input_finish(count);
    descriptor_close(table(), op);
    read.capacity = 0;
    std::memcpy(m.payload, &read, sizeof(read));
    op = call(OS1_IPC_SUBMIT, client, m);
    ASSERT_GE(op, 0);
    result = complete(op);
    EXPECT_EQ(0, result.application_status);
    EXPECT_EQ(0u, result.payload_bytes);
}
TEST_F(KernelConsole, MalformedRequestsAndUnsupportedModesHaveNoSideEffects)
{
    int client = bind();
    ASSERT_GE(client, 0);
    auto m = console_request();
    m.payload[255] = 1;
    EXPECT_EQ(OS1_ERR_INVALID, call(OS1_IPC_SUBMIT, client, m));
    m = console_request();
    m.interface_version = 2;
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, call(OS1_IPC_SUBMIT, client, m));
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, sys_ipc(frames, thread, OS1_IPC_SUBMIT_CAPS, client, 0));
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, sys_ipc(frames, thread, OS1_IPC_SUBMIT_BUFFERS, client, 0));
    os1_ipc_subscription sub{1, sizeof(sub), 1, 0};
    EXPECT_EQ(OS1_ERR_UNSUPPORTED, call(OS1_IPC_SUBSCRIBE, client, sub));
    for(unsigned member : {0xffffffffu, OS1_CONSOLE_GET_INFO})
    {
        m = console_request();
        m.member = member;
        int op = call(OS1_IPC_SUBMIT, client, m);
        ASSERT_GE(op, 0);
        EXPECT_EQ(member == OS1_CONSOLE_GET_INFO ? OS1_ERR_INVALID : OS1_ERR_UNSUPPORTED,
                  complete(op).application_status);
        descriptor_close(table(), op);
    }
    client = bind(0, OS1_RIGHT_READ);
    ASSERT_GE(client, 0);
    for(unsigned size : {0u, 3u, 5u})
    {
        m = console_request(OS1_CONSOLE_TRY_READ);
        m.payload_bytes = size;
        int op = call(OS1_IPC_SUBMIT, client, m);
        ASSERT_GE(op, 0);
        EXPECT_EQ(OS1_ERR_INVALID, complete(op).application_status);
        descriptor_close(table(), op);
    }
    os1_console_read read{129};
    m = console_request(OS1_CONSOLE_TRY_READ);
    m.payload_bytes = sizeof(read);
    std::memcpy(m.payload, &read, sizeof(read));
    int op = call(OS1_IPC_SUBMIT, client, m);
    ASSERT_GE(op, 0);
    EXPECT_EQ(OS1_ERR_INVALID, complete(op).application_status);
    EXPECT_TRUE(host_console_output().empty());
}
TEST_F(KernelConsole, ZeroAndMaximumWritesAndIndependentFifoClients)
{
    int first = bind(), second = bind();
    ASSERT_GE(first, 0);
    ASSERT_GE(second, 0);
    auto m = console_request();
    m.payload_bytes = 0;
    std::memset(m.payload, 0, sizeof(m.payload));
    int op = call(OS1_IPC_SUBMIT, first, m);
    ASSERT_GE(op, 0);
    EXPECT_EQ(0, complete(op).application_status);
    descriptor_close(table(), op);
    m.payload_bytes = 256;
    std::memset(m.payload, 'x', sizeof(m.payload));
    int a = call(OS1_IPC_SUBMIT, first, m);
    ASSERT_GE(a, 0);
    m.payload_bytes = 1;
    std::memset(m.payload, 0, sizeof(m.payload));
    m.payload[0] = 'y';
    int b = call(OS1_IPC_SUBMIT, second, m);
    ASSERT_GE(b, 0);
    EXPECT_EQ(0, complete(a).application_status);
    EXPECT_EQ(0, complete(b).application_status);
    EXPECT_EQ(std::string(256, 'x') + "y", host_console_output());
}
TEST_F(KernelConsole, RetainedResultsAndAliasesKeepOperationQuota)
{
    int client = bind();
    ASSERT_GE(client, 0);
    auto m = console_request();
    int ops[OS1_IPC_KERNEL_OPERATION_LIMIT];
    for(auto& op : ops)
    {
        op = call(OS1_IPC_SUBMIT, client, m);
        ASSERT_GE(op, 0);
    }
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SUBMIT, client, m));
    for(auto op : ops)
        EXPECT_EQ(0, complete(op).application_status);
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SUBMIT, client, m));
    int copy = alias(ops[0], OS1_RIGHT_READ);
    ASSERT_GE(copy, 0);
    descriptor_close(table(), ops[0]);
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SUBMIT, client, m));
    descriptor_close(table(), copy);
    ops[0] = call(OS1_IPC_SUBMIT, client, m);
    ASSERT_GE(ops[0], 0);
    complete(ops[0]);
}
TEST_F(KernelConsole, BindingQuotaSurvivesClientCloseWhileResultLives)
{
    int clients[OS1_IPC_KERNEL_BINDING_LIMIT];
    for(auto& client : clients)
    {
        client = bind();
        ASSERT_GE(client, 0);
    }
    EXPECT_EQ(OS1_ERR_LIMIT, bind());
    auto m = console_request();
    int op = call(OS1_IPC_SUBMIT, clients[0], m);
    ASSERT_GE(op, 0);
    descriptor_close(table(), clients[0]);
    EXPECT_EQ(OS1_ERR_LIMIT, bind());
    complete(op);
    EXPECT_EQ(OS1_ERR_LIMIT, bind());
    descriptor_close(table(), op);
    clients[0] = bind();
    ASSERT_GE(clients[0], 0);
}
TEST_F(KernelConsole, FullDescriptorTableRollsBackBindingAndRequest)
{
    std::vector<int> copies;
    for(;;)
    {
        int fd = alias(1, OS1_RIGHT_WRITE | OS1_RIGHT_DELEGATE);
        if(fd < 0)
            break;
        copies.push_back(fd);
    }
    EXPECT_EQ(OS1_ERR_LIMIT, bind());
    descriptor_close(table(), copies.back());
    copies.pop_back();
    int client = bind();
    ASSERT_GE(client, 0);
    auto m = console_request();
    EXPECT_EQ(OS1_ERR_LIMIT, call(OS1_IPC_SUBMIT, client, m));
    EXPECT_FALSE(ipc_run_kernel_request());
    descriptor_close(table(), copies.back());
    copies.pop_back();
    int op = call(OS1_IPC_SUBMIT, client, m);
    ASSERT_GE(op, 0);
    complete(op);
}
TEST_F(KernelConsole, ClosedOperationStillExecutesAndReleasesQuota)
{
    int client = bind();
    ASSERT_GE(client, 0);
    auto m = console_request();
    int op = call(OS1_IPC_SUBMIT, client, m);
    ASSERT_GE(op, 0);
    descriptor_close(table(), op);
    descriptor_close(table(), client);
    ASSERT_TRUE(ipc_run_kernel_request());
    EXPECT_EQ("ABC", host_console_output());
    EXPECT_FALSE(ipc_run_kernel_request());
    int clients[OS1_IPC_KERNEL_BINDING_LIMIT];
    for(auto& fd : clients)
    {
        fd = bind();
        ASSERT_GE(fd, 0);
    }
}
TEST_F(KernelConsole, DeadlineDoesNotCancelAcceptedWork)
{
    int client = bind();
    ASSERT_GE(client, 0);
    auto m = console_request();
    int op = call(OS1_IPC_SUBMIT, client, m);
    ASSERT_GE(op, 0);
    os1_ipc_deadline d{1, sizeof(d), 0};
    EXPECT_EQ(OS1_ERR_TIMED_OUT, call(OS1_IPC_WAIT_UNTIL, op, d));
    EXPECT_EQ(0, complete(op).application_status);
    EXPECT_EQ("ABC", host_console_output());
}
TEST_F(KernelConsole, KernelWorkerRegistrationRechecksWorkAndDefersCurrentWake)
{
    int client = bind();
    ASSERT_GE(client, 0);
    thread->user_mode = false;
    ASSERT_TRUE(ipc_wait_kernel_work(thread));
    // IRQ entry can save any live kernel register value while this worker parks.
    constexpr uint64_t saved_rax = 0xfeed000000001234ull;
    thread->frame.rax = saved_rax;
    auto m = console_request();
    int op = call(OS1_IPC_SUBMIT, client, m);
    ASSERT_GE(op, 0);
    EXPECT_EQ(ThreadState::Blocked, thread->state);
    EXPECT_EQ(ThreadWaitReason::Ipc, thread->wait.reason);
    set_current_thread(nullptr);
    ipc_drain_wakeups();
    set_current_thread(thread);
    EXPECT_EQ(ThreadWaitReason::None, thread->wait.reason);
    EXPECT_EQ(saved_rax, thread->frame.rax);
    EXPECT_FALSE(ipc_wait_kernel_work(thread));
    complete(op);
    thread->user_mode = true;
}
}  // namespace

TEST_F(KernelConsole, EveryBindingAllocationFailureReleasesSourceAndBudget)
{
    for(int budget : {0, 1, 2})
    {
        const auto source_refs = table().entries[1].instance->references;
        host_ipc_kernel_allocation_budget = budget;
        EXPECT_EQ(OS1_ERR_NO_MEMORY, bind());
        host_ipc_kernel_allocation_budget = -1;
        EXPECT_EQ(source_refs, table().entries[1].instance->references);
        int clients[OS1_IPC_KERNEL_BINDING_LIMIT];
        for(auto& fd : clients)
        {
            fd = bind();
            ASSERT_GE(fd, 0);
        }
        for(auto fd : clients)
            EXPECT_EQ(0, descriptor_close(table(), fd));
    }
}
TEST_F(KernelConsole, WaitManyPinsCompletionAcrossDescriptorClose)
{
    int client = bind();
    ASSERT_GE(client, 0);
    auto m = console_request();
    int op = call(OS1_IPC_SUBMIT, client, m);
    ASSERT_GE(op, 0);
    os1_ipc_wait_many selection{1, sizeof(selection), 1, 0, UINT64_MAX, {}};
    selection.fds[0] = op;
    EXPECT_EQ(kIpcBlocked, call(OS1_IPC_WAIT_MANY, 0, selection));
    EXPECT_EQ(0, descriptor_close(table(), op));
    EXPECT_EQ(0, descriptor_close(table(), client));
    ASSERT_TRUE(ipc_run_kernel_request());
    EXPECT_EQ(ThreadState::Blocked, thread->state);
    set_current_thread(nullptr);
    ipc_drain_wakeups();
    EXPECT_EQ(ThreadState::Ready, thread->state);
    EXPECT_EQ(ThreadWaitReason::None, thread->wait.reason);
    EXPECT_EQ(0u, thread->io_pins);
}
