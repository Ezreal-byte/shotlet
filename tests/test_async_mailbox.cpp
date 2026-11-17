#include "utils/AsyncMailbox.h"
#include "utils/ScopeExit.h"
#include <gtest/gtest.h>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

namespace shotlet {
namespace {
constexpr auto CancellationDeadline = std::chrono::seconds(2);
}

TEST(AsyncMailboxTest, CancelWakesConsumerEvenWhenNativeCallbackNeverArrives) {
    AsyncMailbox<int> mailbox;
    std::promise<void> entered;
    auto worker = std::async(std::launch::async, [&] {
        entered.set_value();
        int sample = 0;
        return mailbox.Wait(sample);
    });
    const ScopeExit cancel([&] { mailbox.Cancel(); });
    entered.get_future().wait();
    mailbox.Cancel();
    ASSERT_EQ(worker.wait_for(CancellationDeadline), std::future_status::ready);
    EXPECT_FALSE(worker.get());
}

TEST(AsyncMailboxTest, SamplesCanArriveBeforeOrAfterConsumerStartsWaiting) {
    AsyncMailbox<std::unique_ptr<int>> mailbox;
    ASSERT_TRUE(mailbox.Publish(std::make_unique<int>(1)));
    std::unique_ptr<int> first;
    ASSERT_TRUE(mailbox.Wait(first));
    EXPECT_EQ(*first, 1);
    auto worker = std::async(std::launch::async, [&] {
        std::unique_ptr<int> next;
        return mailbox.Wait(next) ? *next : 0;
    });
    const ScopeExit cancel([&] { mailbox.Cancel(); });
    ASSERT_TRUE(mailbox.Publish(std::make_unique<int>(2)));
    ASSERT_EQ(worker.wait_for(CancellationDeadline), std::future_status::ready);
    EXPECT_EQ(worker.get(), 2);
}

TEST(AsyncMailboxTest, CancellingDoesNotReleaseQueuedNativeValueOnUiThread) {
    std::thread::id releasedOn;
    auto mailbox = std::make_unique<AsyncMailbox<std::shared_ptr<int>>>();
    auto sample = std::shared_ptr<int>(new int(1), [&](int* value) {
        releasedOn = std::this_thread::get_id();
        delete value;
    });
    const std::weak_ptr<int> alive = sample;
    ASSERT_TRUE(mailbox->Publish(std::move(sample)));
    mailbox->Cancel();
    EXPECT_FALSE(alive.expired());
    std::shared_ptr<int> ignored;
    EXPECT_FALSE(mailbox->Wait(ignored));
    auto discard = std::async(std::launch::async, [&] { mailbox->DiscardPending(); });
    ASSERT_EQ(discard.wait_for(CancellationDeadline), std::future_status::ready);
    discard.get();
    EXPECT_TRUE(alive.expired());
    EXPECT_NE(releasedOn, std::this_thread::get_id());
}

TEST(AsyncMailboxTest, LateCallbackOwnsStateAfterUiClosesAndCannotRestartCapture) {
    auto owner = std::make_shared<AsyncMailbox<int>>();
    const std::weak_ptr<AsyncMailbox<int>> alive = owner;
    std::promise<void> deliver;
    auto worker = std::async(std::launch::async, [callbackState = owner, ready = deliver.get_future()]() mutable {
        ready.wait();
        return callbackState->Publish(1);
    });
    owner->Cancel();
    owner.reset();
    EXPECT_FALSE(alive.expired());
    deliver.set_value();
    ASSERT_EQ(worker.wait_for(CancellationDeadline), std::future_status::ready);
    EXPECT_FALSE(worker.get());
    EXPECT_TRUE(alive.expired());
}
} // namespace shotlet
