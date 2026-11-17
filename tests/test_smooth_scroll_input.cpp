#include <gtest/gtest.h>
#include "scrollcapture/SmoothScrollInput.h"
#include <limits>

using namespace shotlet;

TEST(SmoothScrollInputTest, SmallIntentKeepsMagnitudeAndLargeIntentIsSplit) {
    SmoothScrollInput input(1200, 200);
    input.Add(2, 1000, 1000);
    EXPECT_EQ(input.Next(1000), 2);
    input.Feedback(2, true, 1010, false);
    input.Add(10000, 1016, 1016);
    const auto step = input.Next(1016);
    EXPECT_GT(step, 0);
    EXPECT_LE(step, 1200 / 64);
    EXPECT_LE(input.Pending(), input.Budget());
}

TEST(SmoothScrollInputTest, MissingResponseCannotConsumeUnlimitedInput) {
    SmoothScrollInput input(1200, 200);
    int64_t total = 0;
    for (int time = 1000; time <= 1900; time += 16) {
        input.Add(10000, time, time);
        input.Feedback(0, true, time, false); // 旧零位移样本不能刷新预算。
        total += input.Next(time);
    }
    EXPECT_EQ(total, input.Budget());
    EXPECT_EQ(input.Debt(), input.Budget());
    input.Feedback(0, true, 2000, true);
    EXPECT_EQ(input.Pending(), 0);
    input.Add(10, 2016, 2016);
    EXPECT_EQ(input.Next(2016), 10);
}

TEST(SmoothScrollInputTest, SlowConsumerAndReleasedWheelDiscardBacklog) {
    SmoothScrollInput input(1200, 200);
    input.Add(1000, 1000, 1000);
    EXPECT_GT(input.Next(1000), 0);
    input.Add(1000, 1160, 1160);
    EXPECT_EQ(input.Next(1160), 0); // 没有匹配反馈，暂停并清空积压。
    EXPECT_EQ(input.Pending(), 0);
    input.Feedback(18, true, 1200, false);
    input.Add(100, 1200, 1200);
    EXPECT_EQ(input.Next(1321), 0); // 松手后的旧意图不回放。
    input.Add(100, 1200, 1400);
    EXPECT_EQ(input.Pending(), 0);
}

TEST(SmoothScrollInputTest, MatchFailureRequiresRecoveryAndKeepsDirection) {
    SmoothScrollInput input(1200, 200);
    input.Add(-100, 1000, 1000);
    const auto first = input.Next(1000);
    EXPECT_LT(first, 0);
    input.Feedback(0, false, 1010, false);
    input.Add(-100, 1016, 1016);
    EXPECT_EQ(input.Next(1016), 0);
    input.Feedback(int(first), true, 1050, false);
    input.Add(100, 1050, 1050);
    EXPECT_EQ(input.Next(1050), 0);
    input.Add(-2, 1066, 1066);
    EXPECT_EQ(input.Next(1066), -2);
}

TEST(SmoothScrollInputTest, ExtremeInputCannotOverflowBudget) {
    SmoothScrollInput input(1200, 200);
    input.Add(std::numeric_limits<int64_t>::min(), 1000, 1000);
    EXPECT_EQ(input.Pending(), input.Budget());
    EXPECT_LT(input.Next(1000), 0);
}

TEST(SmoothScrollInputTest, FixedRegionsShrinkBudgetWithoutForgivingOutstandingMotion) {
    SmoothScrollInput input(1200, 200);
    input.Add(300, 1000, 1000);
    EXPECT_EQ(input.Next(1000), 18);
    input.SetContentHeight(220, 200);
    EXPECT_EQ(input.Budget(), 10);
    EXPECT_EQ(input.Debt(), 18);
    EXPECT_EQ(input.Next(1016), 0);
    input.Feedback(18, true, 1032, false);
    input.Add(5, 1032, 1032);
    EXPECT_GT(input.Next(1032), 0);
}
