// ScrollUnits 单元测试
// 对应设计文档 10.2：滚轮 delta 的单位/符号换算集中一处（唯一翻转点）。
// 每个测试顶部的"为什么"注释说明该行为存在的业务理由（CLAUDE.md Rule 9）。

#include <gtest/gtest.h>

#include <cstdint>

#include "scrollcapture/ScrollUnits.h"
#include "scrollcapture/WinScrollUnits.h"

namespace shotlet::wheel {
namespace {

TEST(ScrollWheelUnitsTest, PointDeltaToPixelIntentFlipsSignAndScales) {
    // 为什么：CGEvent point delta 正值 = 向上滚，而 Planner 约定向下为正；
    // 符号若不翻转，用户向下滚页面却向上拼，整条闭环方向反转
    EXPECT_EQ(PointDeltaToPixelIntent(-10, 2.0), 20);  // 向下滚 10pt（Retina 2x）→ +20px
    EXPECT_EQ(PointDeltaToPixelIntent(10, 2.0), -20);  // 向上滚 → 负意图
}

TEST(ScrollWheelUnitsTest, PointDeltaOneXScale) {
    // 为什么：非 Retina 显示器 scaleFactor=1，意图必须与 delta 数值一致（仅符号翻转）
    EXPECT_EQ(PointDeltaToPixelIntent(-3, 1.0), 3);
    EXPECT_EQ(PointDeltaToPixelIntent(0, 1.0), 0);
}

TEST(ScrollWheelUnitsTest, InvalidScaleFallsBackToOne) {
    // 为什么：缩放系数来自显示配置查询，查询失败返回 0/负值时不能崩或产生反向意图
    EXPECT_EQ(PointDeltaToPixelIntent(-10, 0.0), 10);
    EXPECT_EQ(PointDeltaToPixelIntent(-10, -2.0), 10);
}

TEST(ScrollWheelUnitsTest, FractionalScaleRounds) {
    // 为什么：非整数缩放（如 1.5）下点转像素需取整，保证意图是整数像素供 Planner 拆步
    EXPECT_EQ(PointDeltaToPixelIntent(-3, 1.5), 5);  // 4.5 → 5
}

TEST(ScrollWheelUnitsTest, InjectAndExtractAreInverse) {
    // 为什么：注入（意图→wheel delta）与提取（point delta→意图）共用一套符号约定，
    // 互逆性保证"注入 N 像素"与"收到 N 像素意图"符号一致，demo 自吞回路检测依赖它
    for (const int64_t px : {int64_t{-300}, int64_t{-1}, int64_t{0}, int64_t{1}, int64_t{250}}) {
        const auto wheelDelta = PixelIntentToWheelDelta(px);
        EXPECT_EQ(PointDeltaToPixelIntent(wheelDelta, 1.0), px);
    }
}

TEST(ScrollWheelUnitsTest, FixedPhysicalStepConvertsToDisplayPoints) {
    for (const double scale : {1.0, 2.0}) {
        for (const int64_t pixels : {-200, 200}) {
            EXPECT_EQ(PointDeltaToPixelIntent(PixelIntentToWheelDelta(pixels, scale), scale), pixels);
        }
    }
    EXPECT_EQ(PixelIntentToWheelDelta(250, 2.0), -125);
    EXPECT_EQ(PixelIntentToWheelDelta(-250, 2.0), 125);
    EXPECT_EQ(PixelIntentToWheelDelta(250, 0.0), -250);
}

TEST(ScrollWheelUnitsTest, LargeIntentClamps) {
    // 为什么：int64 意图注入 int32 wheel 字段必须钳制且保持方向——
    // 向下滚超大值 → wheel delta 负边界（wheel delta = -意图），溢出取反会导致方向反转
    EXPECT_EQ(PixelIntentToWheelDelta(1LL << 40), -0x7fffffff);
    EXPECT_EQ(PixelIntentToWheelDelta(-(1LL << 40)), 0x7fffffff);
}

TEST(ScrollWheelUnitsTest, LineDeltaFallback) {
    // 为什么：离散滚轮鼠标产生 line 单位事件（point delta 为 0），
    // 若不退路换算会把用户滚动丢成 intent=0 → 事件被吞但页面不滚（闭环假死）
    EXPECT_EQ(LineDeltaToPixelIntent(-3, 2.0), 60);   // 3 行 × 10pt × 2x，向下
    EXPECT_EQ(LineDeltaToPixelIntent(2, 1.0), -20);   // 向上翻 2 行
    EXPECT_EQ(LineDeltaToPixelIntent(-1, 0.0), 10);   // 无效缩放退 1x
}

TEST(ScrollWheelUnitsTest, LineDeltaZeroStaysZero) {
    // 为什么：point/line 都为 0（静止滚轮噪声）时 intent 必须为 0，
    // 否则 planner 会拆出幽灵步注入
    EXPECT_EQ(LineDeltaToPixelIntent(0, 2.0), 0);
    EXPECT_EQ(PointDeltaToPixelIntent(0, 2.0), 0);
}

}  // namespace
}  // namespace shotlet::wheel

namespace shotlet::winwheel {
namespace {

TEST(WinScrollWheelUnitsTest, NativeDeltaUsesControllerDirection) {
    // 为什么：Windows 正 wheel delta 表示向上，而 controller 统一约定向下为正。
    EXPECT_EQ(WheelDeltaToIntent(120), -120);
    EXPECT_EQ(WheelDeltaToIntent(-120), 120);
    EXPECT_EQ(WheelDeltaToIntent(0), 0);
}

TEST(WinScrollWheelUnitsTest, OnlyOwnMarkedEventsAreTreatedAsInjected) {
    // 为什么：Windows 触控板事件可能带 LLMHF_INJECTED，但仍然属于用户输入。
    constexpr uintptr_t marker = 0x1234;
    EXPECT_FALSE(IsInjectedEvent(1, 0, marker));
    EXPECT_TRUE(IsInjectedEvent(0, marker, marker));
    EXPECT_TRUE(IsInjectedEvent(1, marker, marker));
    EXPECT_FALSE(IsInjectedEvent(0, 0, marker));
}

TEST(WinScrollWheelUnitsTest, SystemInjectedUserInputUsesPassThroughMode) {
    // 为什么：远程桌面转发的触控板事件必须由目标窗口直接消费，不能拦截后重新注入。
    constexpr uintptr_t marker = 0x1234;
    EXPECT_TRUE(IsPassThroughEvent(1, 0, marker));
    EXPECT_FALSE(IsPassThroughEvent(0, 0, marker));
    EXPECT_FALSE(IsPassThroughEvent(1, marker, marker));
}

TEST(WinScrollWheelUnitsTest, OwnMarkerSurvives32BitInputForwarding) {
    // 自身补发滚动经 32 位链路转发后仍须识别，不能将采集模式切换成透传。
    const uintptr_t forwarded = static_cast<uint32_t>(kSyntheticScrollMarker);
    EXPECT_TRUE(IsInjectedEvent(1, forwarded, kSyntheticScrollMarker));
    EXPECT_FALSE(IsPassThroughEvent(1, forwarded, kSyntheticScrollMarker));
}

TEST(WinScrollWheelUnitsTest, InjectedWheelDataFlipsSignAndClamps) {
    // 为什么：注入方向必须与用户意图一致，大步长也不能因整数溢出而反向。
    EXPECT_EQ(PixelIntentToWheelData(250), -250);
    EXPECT_EQ(PixelIntentToWheelData(-250), 250);
    EXPECT_EQ(PixelIntentToWheelData(int64_t{1} << 40), INT32_MIN);
    EXPECT_EQ(PixelIntentToWheelData(-(int64_t{1} << 40)), INT32_MAX);
}

}  // namespace
}  // namespace shotlet::winwheel
