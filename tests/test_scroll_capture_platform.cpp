#include <gtest/gtest.h>

#include "scrollcapture/MacScrollEvent.h"
#include "scrollcapture/MacFrameSource.h"
#include "scrollcapture/ScrollCaptureStitcher.h"
#include "scrollcapture/ScrollPlanner.h"
#include "scroll_capture_test_data.h"
#include "mac_frame_timeline.h"

namespace shotlet {
namespace {

using Milliseconds = TestUtils::MacFrameTimeline::Milliseconds;
const Milliseconds kQuietPeriod{MacFrameSource::Config{}.quietPeriodMs};
const Milliseconds kSettleTimeout{MacFrameSource::Config{}.settleTimeoutMs};

TEST(ScrollCapturePlatformTest, VerticalWheelProducesIntentAndHorizontalWheelDoesNot) {
    // 直接构造系统滚轮事件，防止只测数值换算而漏掉轴读取错误。
    CGEventRef vertical = CGEventCreateScrollWheelEvent2(
        nullptr, kCGScrollEventUnitPixel, 1, -40, 0, 0);
    ASSERT_NE(vertical, nullptr);
    EXPECT_EQ(wheel::VerticalIntent(vertical, 2.0), 80);
    CFRelease(vertical);

    CGEventRef horizontal = CGEventCreateScrollWheelEvent2(
        nullptr, kCGScrollEventUnitPixel, 2, 0, -40, 0);
    ASSERT_NE(horizontal, nullptr);
    EXPECT_EQ(wheel::VerticalIntent(horizontal, 2.0), 0);
    CFRelease(horizontal);
}

TEST(ScrollCapturePlatformTest, DiscreteVerticalWheelFallsBackToLineDelta) {
    CGEventRef event = CGEventCreateScrollWheelEvent2(
        nullptr, kCGScrollEventUnitLine, 1, 3, 0, 0);
    ASSERT_NE(event, nullptr);
    CGEventSetIntegerValueField(event, kCGScrollWheelEventPointDeltaAxis1, 0);
    EXPECT_EQ(wheel::VerticalIntent(event, 2.0), -60);
    CFRelease(event);
}

TEST(ScrollCapturePlatformTest, FrameArrivingBeforeWaitIsNotDiscarded) {
    MacFrameSource source;
    const uint8_t first[] = {10, 20, 30, 255};
    const uint8_t next[] = {40, 50, 60, 255};
    source.HandleFrame(first, 1, 1, 4, false);
    const auto baseline = source.TakeBaseline();
    source.HandleFrame(next, 1, 1, 4, false, baseline.notBefore + 1);
    CapturedFrame frame;
    ASSERT_TRUE(source.WaitForStableFrame(baseline, frame));
    EXPECT_EQ(frame.pixels, (std::vector<uint8_t>{40, 50, 60, 255}));
    EXPECT_TRUE(frame.stable);  // 响应后持续静止才完成，不要求静止页面重复生成完整帧。
}

TEST(ScrollCapturePlatformTest, SharedFramesKeepPixelsWithoutCopyingOrHoldingSourceLifetime) {
    CapturedFrame first, next;
    {
        MacFrameSource source;
        const uint8_t bgra[] = {30, 20, 10, 255, 99, 99, 99, 99,
                                60, 50, 40, 255, 99, 99, 99, 99};
        source.HandleFrame(bgra, 1, 2, 8, true);
        ASSERT_TRUE(source.WaitForFrameReference(0, first, 0));
        EXPECT_TRUE(first.pixels.empty());
        EXPECT_EQ(*first.sharedPixels, (std::vector<uint8_t>{10, 20, 30, 255, 40, 50, 60, 255}));
        EXPECT_EQ(source.TakeBaseline().frame.sharedPixels, first.sharedPixels);
        source.HandleIdleFrame();
        ASSERT_TRUE(source.WaitForFrameReference(first.frameId, next, 0));
        EXPECT_EQ(next.sharedPixels, first.sharedPixels);
        const uint8_t changed[] = {70, 80, 90, 255};
        source.HandleFrame(changed, 1, 1, 4, false);
        ASSERT_TRUE(source.WaitForFrameReference(next.frameId, next, 0));
        EXPECT_NE(next.sharedPixels, first.sharedPixels);
        source.HandleStreamError("模拟流失败");
        EXPECT_FALSE(source.WaitForFrameReference(0, next, 0));
    }
    EXPECT_EQ(first.View().data[0], 10);
    EXPECT_EQ(first.View().data[4], 40);
    EXPECT_EQ(next.View().data[0], 70);
}

TEST(ScrollCapturePlatformTest, RespondedAndQuietFrameDoesNotPayFixedGuardDelay) {
    // 应用已经响应且持续静止时，应在静止窗口结束，不再叠加固定等待。
    MacFrameSource source;
    const uint8_t first[] = {10, 20, 30, 255};
    const uint8_t next[] = {90, 100, 110, 255};
    source.HandleFrame(first, 1, 1, 4, false);
    const auto baseline = source.TakeBaseline();
    source.HandleFrame(next, 1, 1, 4, false, baseline.notBefore + 1);
    source.HandleIdleFrame(baseline.notBefore + 2);
    TestUtils::MacFrameTimeline timeline(source, baseline);
    CapturedFrame frame;
    ASSERT_TRUE(timeline.WaitForStableFrame(frame));
    EXPECT_TRUE(frame.stable);
    EXPECT_DOUBLE_EQ(timeline.Elapsed().count(), kQuietPeriod.count());
    EXPECT_EQ(frame.pixels, (std::vector<uint8_t>{90, 100, 110, 255}));
}

TEST(ScrollCapturePlatformTest, IdleAfterInjectionConfirmsUnchangedFrame) {
    MacFrameSource source;
    const uint8_t pixels[] = {10, 20, 30, 255};
    source.HandleFrame(pixels, 1, 1, 4, false);
    const auto baseline = source.TakeBaseline();
    source.HandleIdleFrame(baseline.notBefore + 2);
    TestUtils::MacFrameTimeline timeline(source, baseline);
    CapturedFrame frame;
    ASSERT_TRUE(timeline.WaitForStableFrame(frame));
    EXPECT_DOUBLE_EQ(timeline.Elapsed().count(), kSettleTimeout.count());
    EXPECT_TRUE(frame.stable);
    EXPECT_EQ(frame.pixels, (std::vector<uint8_t>{10, 20, 30, 255}));
}

TEST(ScrollCapturePlatformTest, EarlyIdleMustWaitForDelayedScrollResponse) {
    // 注入后的 Idle 先到，真正滚动效果经过延迟才出现，不能被提前判为无变化。
    MacFrameSource source;
    const uint8_t first[] = {10, 20, 30, 255};
    const uint8_t moved[] = {90, 100, 110, 255};
    source.HandleFrame(first, 1, 1, 4, false);
    const auto baseline = source.TakeBaseline();
    source.HandleIdleFrame(baseline.notBefore + 2);
    TestUtils::MacFrameTimeline timeline(source, baseline);
    const auto responseDelay = kQuietPeriod;
    timeline.ScheduleFrame(responseDelay, {moved, 1, 1, 4});
    CapturedFrame frame;
    ASSERT_TRUE(timeline.WaitForStableFrame(frame));
    EXPECT_EQ(frame.pixels, (std::vector<uint8_t>{90, 100, 110, 255}));
}

TEST(ScrollCapturePlatformTest, BriefAnimationPauseMustNotFinishStep) {
    MacFrameSource source;
    const uint8_t first[] = {10, 20, 30, 255};
    const uint8_t middle[] = {50, 60, 70, 255};
    const uint8_t final[] = {90, 100, 110, 255};
    source.HandleFrame(first, 1, 1, 4, false);
    const auto baseline = source.TakeBaseline();
    source.HandleFrame(middle, 1, 1, 4, false, baseline.notBefore + 1);
    source.HandleIdleFrame(baseline.notBefore + 2);
    TestUtils::MacFrameTimeline timeline(source, baseline);
    // 暂停时间短于静止窗口，最终帧到达后必须重新计算静止时间。
    constexpr double kAnimationPauseFraction = 0.75;
    const auto animationPause = kQuietPeriod * kAnimationPauseFraction;
    timeline.ScheduleFrame(animationPause, {final, 1, 1, 4});
    CapturedFrame frame;
    ASSERT_TRUE(timeline.WaitForStableFrame(frame));
    EXPECT_TRUE(frame.stable);
    EXPECT_DOUBLE_EQ(timeline.Elapsed().count(), (animationPause + kQuietPeriod).count());
    EXPECT_EQ(frame.pixels, (std::vector<uint8_t>{90, 100, 110, 255}));
}

TEST(ScrollCapturePlatformTest, QueuedPreInjectionFrameIsNotFreshEvenIfCallbackIsLate) {
    MacFrameSource source;
    const uint8_t first[] = {10, 20, 30, 255};
    const uint8_t queued[] = {90, 100, 110, 255};
    source.HandleFrame(first, 1, 1, 4, false);
    const auto baseline = source.TakeBaseline();
    // 回调发生在注入之后，但帧生成于注入之前，不能算作当前步骤的新画面。
    source.HandleFrame(queued, 1, 1, 4, false, baseline.notBefore - 1);
    TestUtils::MacFrameTimeline timeline(source, baseline);
    CapturedFrame frame;
    EXPECT_FALSE(timeline.WaitForStableFrame(frame));
    EXPECT_DOUBLE_EQ(timeline.Elapsed().count(), kSettleTimeout.count());
}

TEST(ScrollCapturePlatformTest, ContinuingAnimationAtDeadlineIsNotStable) {
    MacFrameSource source;
    const uint8_t first[] = {10, 20, 30, 255};
    source.HandleFrame(first, 1, 1, 4, false);
    const auto baseline = source.TakeBaseline();
    TestUtils::MacFrameTimeline timeline(source, baseline);
    const auto frameInterval = kQuietPeriod / 2;
    const uint8_t dark[] = {100, 100, 100, 255};
    const uint8_t light[] = {200, 200, 200, 255};
    bool useDark = true;
    for (auto offset = frameInterval; offset <= kSettleTimeout; offset += frameInterval) {
        timeline.ScheduleFrame(offset, {useDark ? dark : light, 1, 1, 4});
        useDark = !useDark;
    }
    CapturedFrame frame;
    ASSERT_TRUE(timeline.WaitForStableFrame(frame));
    EXPECT_FALSE(frame.stable);
    EXPECT_DOUBLE_EQ(timeline.Elapsed().count(), kSettleTimeout.count());
}

TEST(ScrollCapturePlatformTest, DelayedScrollRemainsAssociatedWithItsOwnInjection) {
    // 覆盖日志末尾两步：-144 的响应必须归本步，下一步 -2 只能测得 -4。
    constexpr int width = 200, height = 1200, pageHeight = 2500;
    const auto page = scrolltest::MakeTexturedLongImage(width, pageHeight, 7);
    scrolltest::FrameSpec spec;
    spec.viewportHeight = height;
    spec.contentY = 700;
    auto first = scrolltest::CutFrame(page, width, pageHeight, spec);
    MacFrameSource source;
    source.HandleFrame(first.data(), width, height, width * 4, false);
    ScrollCaptureStitcher stitcher;
    ASSERT_TRUE(stitcher.StartSession({first.data(), width, height, width * 4}));
    PlannerConfig config;
    config.initialGain = 2.0;
    ScrollPlanner planner(config);
    planner.SetViewportHeight(height);
    for (const int intent : {-144, -2}) {
        planner.AddIntent(intent);
        const auto step = planner.TakeStep();
        const auto baseline = source.TakeBaseline();
        source.HandleIdleFrame(baseline.notBefore + 2);
        spec.contentY += intent * 2;
        auto moved = scrolltest::CutFrame(page, width, pageHeight, spec);
        TestUtils::MacFrameTimeline timeline(source, baseline);
        timeline.ScheduleFrame(kQuietPeriod, {moved.data(), width, height, width * 4});
        CapturedFrame frame;
        ASSERT_TRUE(timeline.WaitForStableFrame(frame));
        ASSERT_TRUE(frame.stable);
        const auto result = stitcher.ProcessFrame({
            {frame.pixels.data(), width, height, width * 4},
            static_cast<int>(step.expectedShift), true, frame.stable});
        ASSERT_EQ(result.status, StitchStatus::Ok);
        EXPECT_EQ(result.dy, intent * 2);
        EXPECT_FALSE(result.usedFallbackSearch);
        planner.CompleteStep(result.dy, step.injectPixels, true);
        EXPECT_DOUBLE_EQ(planner.Gain(), 2.0);
    }
    std::vector<uint8_t> output;
    int w = 0, h = 0;
    ASSERT_TRUE(stitcher.ExportFinalImage(output, w, h));
    EXPECT_EQ(h, height + 292);
    EXPECT_TRUE(std::equal(output.begin(), output.end(), page.begin() + spec.contentY * width * 4));
}

TEST(ScrollCapturePlatformTest, StreamFailureDoesNotReturnCachedPixelsAsSuccess) {
    MacFrameSource source;
    const uint8_t pixels[] = {10, 20, 30, 255};
    source.HandleFrame(pixels, 1, 1, 4, false);
    source.HandleStreamError("测试采集失败");
    CapturedFrame frame;
    EXPECT_FALSE(source.WaitForStableFrame(CaptureBaseline{}, frame));
}

TEST(ScrollCapturePlatformTest, WheelIntentDrivesMultipleStepsAndExportsLongImage) {
    // 模拟 Retina 页面按注入量的两倍移动；验证事件→拆步→拼接→导出全链路。
    constexpr int width = 200;
    constexpr int height = 1200;
    constexpr int pageHeight = 5000;
    const auto page = scrolltest::MakeTexturedLongImage(width, pageHeight, 7);
    scrolltest::FrameSpec spec;
    spec.viewportHeight = height;
    auto first = scrolltest::CutFrame(page, width, pageHeight, spec);
    ScrollCaptureStitcher stitcher;
    ASSERT_TRUE(stitcher.StartSession({first.data(), width, height, width * 4}));
    PlannerConfig config;
    config.initialGain = 2.0;
    ScrollPlanner planner(config);
    planner.SetViewportHeight(height);
    CGEventRef event = CGEventCreateScrollWheelEvent2(
        nullptr, kCGScrollEventUnitPixel, 1, -400, 0, 0);
    ASSERT_NE(event, nullptr);
    planner.AddIntent(wheel::VerticalIntent(event, 2.0));
    CFRelease(event);
    int steps = 0;
    while (planner.HasStep() && steps < 20) {
        const auto step = planner.TakeStep();
        spec.contentY += static_cast<int>(step.injectPixels * 2);
        auto frame = scrolltest::CutFrame(page, width, pageHeight, spec);
        const auto result = stitcher.ProcessFrame(
            {{frame.data(), width, height, width * 4},
             static_cast<int>(step.expectedShift), true, true});
        ASSERT_EQ(result.status, StitchStatus::Ok);
        planner.CompleteStep(result.dy, step.injectPixels, true);
        ++steps;
    }
    EXPECT_GT(steps, 1);
    std::vector<uint8_t> output;
    int outWidth = 0, outHeight = 0;
    ASSERT_TRUE(stitcher.ExportFinalImage(output, outWidth, outHeight));
    EXPECT_EQ(outHeight, height + 1600);
    EXPECT_TRUE(std::equal(output.begin(), output.end(), page.begin()));
}

}  // namespace
}  // namespace shotlet
