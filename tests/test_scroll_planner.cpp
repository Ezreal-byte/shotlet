// ScrollPlanner 单元测试
// 对应设计文档 10.3：分步驱动 / 增益标定 / 串行化 / 意图合并 / 积压上限。
// 每个测试顶部的"为什么"注释说明该行为存在的业务理由（CLAUDE.md Rule 9）。

#include <gtest/gtest.h>

#include <cstdint>

#include "scrollcapture/ScrollPlanner.h"

namespace shotlet {
namespace {

constexpr int kVH = 500;  // 视口高（选区高）

TEST(ScrollPlannerTest, ProbeStepsAreSmallThenNormal) {
    // 为什么：增益未标定时小步探测，避免一步滚过头导致重叠不足（设计 10.3）
    ScrollPlanner planner;
    planner.SetViewportHeight(kVH);
    planner.AddIntent(2000);

    // 前 3 步：探测步长 0.3 × 500 = 150
    for (int i = 0; i < 3; ++i) {
        const auto step = planner.TakeStep();
        EXPECT_EQ(step.injectPixels, 150) << "探测步 " << i;
        EXPECT_EQ(step.expectedShift, 150);
        planner.CompleteStep(150, step.injectPixels, true);
    }
    // 第 4 步起：正常步长 0.6 × 500 = 300
    const auto step = planner.TakeStep();
    EXPECT_EQ(step.injectPixels, 300);
}

TEST(ScrollPlannerTest, SplitsLargeIntentIntoSteps) {
    // 为什么：用户猛滚一次可能超过一屏，拆步注入让选区画面连续更新；
    // 这是 Shotlet 接管滚轮的核心价值（设计 10.3）
    ScrollPlanner planner;
    planner.SetViewportHeight(kVH);
    planner.AddIntent(1000);

    int64_t totalInjected = 0;
    int steps = 0;
    while (planner.HasStep()) {
        const auto step = planner.TakeStep();
        EXPECT_LE(step.injectPixels, 300);  // 单步注入受步长上限约束
        totalInjected += step.injectPixels;
        planner.CompleteStep(step.injectPixels, step.injectPixels, true);
        ++steps;
    }
    EXPECT_EQ(totalInjected, 1000);  // 无遗漏、无重复
    EXPECT_GE(steps, 4);
}

TEST(ScrollPlannerTest, SerializesSingleInFlight) {
    // 为什么：任一时刻只允许一个"注入→采集→匹配"步骤在执行（设计 10.1），
    // 并行会导致帧序列错乱、拼接基准失效
    ScrollPlanner planner;
    planner.SetViewportHeight(kVH);
    planner.AddIntent(1000);

    ASSERT_TRUE(planner.HasStep());
    (void)planner.TakeStep();
    // in-flight 期间：pending 仍有余额也不可再取步
    EXPECT_FALSE(planner.HasStep());
    EXPECT_TRUE(planner.InFlight());
    planner.AddIntent(100);  // 期间到达的意图只累积
    EXPECT_FALSE(planner.HasStep());

    planner.CompleteStep(150, 150, true);
    EXPECT_TRUE(planner.HasStep());  // 完成后恢复可取
    EXPECT_EQ(planner.Pending(), 950);
}

TEST(ScrollPlannerTest, GainCalibrationShrinksInjection) {
    // 为什么：注入量 = 目标位移 / 增益；增益标定偏低（页面滚得快）时
    // 反推注入量变小，避免实际位移超出步长目标导致重叠不足
    ScrollPlanner planner;
    planner.SetViewportHeight(kVH);
    planner.AddIntent(2000);

    auto step = planner.TakeStep();  // 探测步 150（gain=1）
    EXPECT_EQ(step.injectPixels, 150);
    planner.CompleteStep(300, step.injectPixels, true);  // 实际 dy=2×注入 → k=2

    // EMA: gain = 1 + 0.3×(2-1) = 1.3 → 下一步注入 = 150/1.3 ≈ 115
    step = planner.TakeStep();
    EXPECT_NEAR(step.injectPixels, 115, 2);
    EXPECT_NEAR(step.expectedShift, 150, 2);  // 预期位移仍落在步长目标
}

TEST(ScrollPlannerTest, ReverseIntentNetMerge) {
    // 为什么：反向意图按净量合并，不产生无谓往返（往返 = 长图重复/错位风险）
    ScrollPlanner planner;
    planner.SetViewportHeight(kVH);
    planner.AddIntent(1000);
    planner.AddIntent(-400);
    EXPECT_EQ(planner.Pending(), 600);

    const auto step = planner.TakeStep();  // 150
    EXPECT_EQ(step.injectPixels, 150);  // 正向净量
    EXPECT_EQ(planner.Pending(), 450);
}

TEST(ScrollPlannerTest, PendingCapTruncates) {
    // 为什么：触控板 momentum 惯性无上限累积会让采集失控，
    // pending 上限（3 个视口高）保证用户停止后采集会收敛结束
    ScrollPlanner planner;
    planner.SetViewportHeight(kVH);
    planner.AddIntent(2000);  // 超出 cap=1500
    EXPECT_EQ(planner.Pending(), 1500);

    // 反向同样受 cap 约束
    planner.AddIntent(-5000);
    EXPECT_EQ(planner.Pending(), -1500);
}

TEST(ScrollPlannerTest, FailureShrinksNextStep) {
    // 为什么：匹配失败（位移明显大于预期/重叠不足）后下一步必须更保守，
    // 否则连续失败会让拼接器持续进恢复流程（设计 10.3 / 11.6）
    ScrollPlanner planner;
    planner.SetViewportHeight(kVH);
    planner.AddIntent(2000);

    auto step = planner.TakeStep();  // 探测步 150
    EXPECT_EQ(step.injectPixels, 150);
    planner.CompleteStep(0, step.injectPixels, false);  // 失败

    // 步长收缩 0.7 → 下一步目标 150×0.7 = 105
    step = planner.TakeStep();
    EXPECT_EQ(step.injectPixels, 105);
    EXPECT_EQ(step.expectedShift, 105);

    // 成功后逐步恢复（0.7×1.1 = 0.77 → 150×0.77 ≈ 116）
    planner.CompleteStep(105, step.injectPixels, true);
    step = planner.TakeStep();
    EXPECT_NEAR(step.injectPixels, 116, 2);
}

TEST(ScrollPlannerTest, SmallPendingTakenInOneStep) {
    // 为什么：意图量小于步长上限时一步取完（预期位移 < 目标即可，
    // 不必凑满步长；否则会引入不存在的滚动）
    ScrollPlanner planner;
    planner.SetViewportHeight(kVH);
    planner.AddIntent(80);

    const auto step = planner.TakeStep();
    EXPECT_EQ(step.injectPixels, 80);
    EXPECT_EQ(step.expectedShift, 80);
    EXPECT_EQ(planner.Pending(), 0);
    planner.CompleteStep(80, 80, true);
    EXPECT_FALSE(planner.HasStep());
}

TEST(ScrollPlannerTest, DelayedResponseDoesNotPoisonGain) {
    PlannerConfig config;
    config.initialGain = 2.0;
    ScrollPlanner planner(config);
    planner.SetViewportHeight(1200);
    planner.AddIntent(-2);
    auto step = planner.TakeStep();
    planner.CompleteStep(-220, step.injectPixels, true);
    EXPECT_DOUBLE_EQ(planner.Gain(), 2.0);
    planner.AddIntent(-100);
    step = planner.TakeStep();
    planner.CompleteStep(-300, step.injectPixels, true, false);
    EXPECT_DOUBLE_EQ(planner.Gain(), 2.0);  // 重试/兜底结果不参与标定。
}

TEST(ScrollPlannerTest, BoundaryDoesNotReduceGainOrConsumeProbeSteps) {
    ScrollPlanner planner;
    planner.SetViewportHeight(kVH);
    for (int i = 0; i < 8; ++i) {
        planner.AddIntent(150);
        const auto step = planner.TakeStep();
        planner.CompleteStep(0, step.injectPixels, true);
    }
    EXPECT_DOUBLE_EQ(planner.Gain(), 1.0);
    planner.AddIntent(-1500);
    const auto reverse = planner.TakeStep();
    EXPECT_EQ(reverse.injectPixels, -150);
    EXPECT_EQ(reverse.expectedShift, -150);
}

TEST(ScrollPlannerTest, UnconfiguredOrIdleTakeStepIsNoOp) {
    // 为什么：TakeStep 在无可执行步骤时必须是无副作用的空操作，
    // 否则调用方的防御性调用会污染 in-flight 状态导致死锁（永无 HasStep）
    ScrollPlanner planner;
    EXPECT_FALSE(planner.HasStep());  // 未配置视口
    EXPECT_EQ(planner.TakeStep().injectPixels, 0);
    EXPECT_FALSE(planner.InFlight());

    planner.SetViewportHeight(kVH);
    EXPECT_FALSE(planner.HasStep());  // 无意图
    EXPECT_EQ(planner.TakeStep().injectPixels, 0);
    EXPECT_FALSE(planner.InFlight());
}

}  // namespace
}  // namespace shotlet
