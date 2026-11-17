#include "app/OverlayReclaimTimer.h"
#include <gtest/gtest.h>

using shotlet::OverlayReclaimTimer;
using namespace std::chrono_literals;

TEST(OverlayReclaimTimerTest, IdleWithoutCaptureDoesNotScheduleReclaim) {
    OverlayReclaimTimer timer;
    EXPECT_FALSE(timer.IsPending());
    EXPECT_FALSE(timer.IsDue(OverlayReclaimTimer::Clock::time_point{} + 1h));
}

TEST(OverlayReclaimTimerTest, ReclaimStartsAtThirtySecondBoundary) {
    OverlayReclaimTimer timer;
    const auto finished = OverlayReclaimTimer::Clock::time_point{};
    timer.Schedule(finished);
    EXPECT_TRUE(timer.IsPending());
    EXPECT_FALSE(timer.IsDue(finished + 30s - 1ns));
    EXPECT_TRUE(timer.IsDue(finished + 30s));
}

TEST(OverlayReclaimTimerTest, NewCaptureCancelsPreviousDeadline) {
    OverlayReclaimTimer timer;
    const auto finished = OverlayReclaimTimer::Clock::time_point{};
    timer.Schedule(finished);
    timer.Cancel();
    EXPECT_FALSE(timer.IsPending());
    EXPECT_FALSE(timer.IsDue(finished + 1h));
    timer.Schedule(finished + 25s);
    EXPECT_FALSE(timer.IsDue(finished + 30s));
    EXPECT_TRUE(timer.IsDue(finished + 55s));
}

TEST(OverlayReclaimTimerTest, RepeatedCleanupRestartsIdlePeriod) {
    OverlayReclaimTimer timer;
    const auto finished = OverlayReclaimTimer::Clock::time_point{};
    timer.Schedule(finished);
    timer.Schedule(finished + 20s);
    EXPECT_FALSE(timer.IsDue(finished + 30s));
    EXPECT_TRUE(timer.IsDue(finished + 50s));
    timer.Cancel();
    timer.Cancel();
    EXPECT_FALSE(timer.IsDue(finished + 1h));
}
