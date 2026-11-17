// FrameStability 单元测试
// 对应设计文档 10.4：稳定帧判定（降采样差分 + 容差）。
// 每个测试顶部的"为什么"注释说明该行为存在的业务理由（CLAUDE.md Rule 9）。

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "scrollcapture/FrameStability.h"

namespace shotlet {
namespace {

constexpr int kW = 160;   // 帧宽（8 的倍数，与默认 gridStep=8 网格对齐）
constexpr int kH = 120;   // 帧高
constexpr int kRowBytes = kW * 4;

PixelView ViewOf(const std::vector<uint8_t>& v) {
    return PixelView{v.data(), kW, kH, kRowBytes};
}

// 灰度帧：每行一个亮度（行号 × 1.5），列方向叠加固定条纹增强采样差异
std::vector<uint8_t> MakeShadeFrame(int rowStep) {
    std::vector<uint8_t> v(static_cast<size_t>(kRowBytes) * kH, 0);
    for (int y = 0; y < kH; ++y) {
        const uint8_t shade = static_cast<uint8_t>((y * rowStep) % 200 + 30);
        for (int x = 0; x < kW; ++x) {
            const size_t i = (static_cast<size_t>(y) * kRowBytes) + x * 4;
            v[i] = shade;
            v[i + 1] = static_cast<uint8_t>(shade / 2 + 10);
            v[i + 2] = static_cast<uint8_t>(shade / 3 + 20);
            v[i + 3] = 0xFF;
        }
    }
    return v;
}

// 在 (px,py) 起的 w×h 矩形内写入高对比内容（模拟视频/hover 局部动态区域）
void PatchRegion(std::vector<uint8_t>& v, int px, int py, int w, int h) {
    for (int y = py; y < py + h && y < kH; ++y) {
        for (int x = px; x < px + w && x < kW; ++x) {
            const size_t i = (static_cast<size_t>(y) * kRowBytes) + x * 4;
            v[i] = 0xF0;
            v[i + 1] = 0x80;
            v[i + 2] = 0x00;
        }
    }
}

TEST(FrameStabilityTest, IdenticalFramesAreStable) {
    // 为什么：静止判定是稳定帧的定义基准；注入后页面停止，必须立即判稳
    const auto a = MakeShadeFrame(1);
    const auto b = a;
    EXPECT_FLOAT_EQ(ComputeFrameDiffRatio(ViewOf(a), ViewOf(b)), 0.0f);
    EXPECT_TRUE(IsFrameStable(ViewOf(a), ViewOf(b)));
}

TEST(FrameStabilityTest, OnePixelShiftIsUnstable) {
    // 为什么：滚动动画中间帧不得误判为稳定，否则拼接器采样到错帧、dy 估计错
    // rowStep=8：相邻行 R 通道差 8 > 阈值 6，整帧下移 1 行后所有采样行都判差异
    const auto a = MakeShadeFrame(8);
    auto b = MakeShadeFrame(8);
    for (int y = kH - 1; y > 0; --y) {
        std::memcpy(b.data() + static_cast<size_t>(y) * kRowBytes,
                    b.data() + static_cast<size_t>(y - 1) * kRowBytes, kRowBytes);
    }
    EXPECT_GT(ComputeFrameDiffRatio(ViewOf(a), ViewOf(b)), 0.5f);
    EXPECT_FALSE(IsFrameStable(ViewOf(a), ViewOf(b)));
}

TEST(FrameStabilityTest, SmallRegionChangeIsStable) {
    // 为什么：设计 10.4 第 4 条——动态内容（视频、光标闪烁、hover）只占局部，
    // 必须容忍局部变化，否则稳定帧永远等不到，settle 每步都超时
    const auto a = MakeShadeFrame(1);
    auto b = MakeShadeFrame(1);
    // 16×8 像素块落在采样网格上只覆盖 1 个采样点：1/300 ≈ 0.3% < 1.5% 阈值
    PatchRegion(b, 16, 16, 16, 8);
    EXPECT_TRUE(IsFrameStable(ViewOf(a), ViewOf(b)));
}

TEST(FrameStabilityTest, LargeRegionChangeIsUnstable) {
    // 为什么：大面积变化 = 滚动中间帧或页面跳变，判定稳定会把错帧喂给拼接器
    const auto a = MakeShadeFrame(1);
    auto b = MakeShadeFrame(1);
    PatchRegion(b, 0, 0, kW, kH / 2);  // 上半屏全变
    EXPECT_GT(ComputeFrameDiffRatio(ViewOf(a), ViewOf(b)), 0.4f);
    EXPECT_FALSE(IsFrameStable(ViewOf(a), ViewOf(b)));
}

TEST(FrameStabilityTest, SizeMismatchReturnsMaxDiff) {
    // 为什么：帧尺寸漂移是采集层 bug，按最不稳定处理让调用方 fail loud，不能静默判稳
    const auto a = MakeShadeFrame(1);
    std::vector<uint8_t> b(static_cast<size_t>(kRowBytes) * (kH - 1), 0);
    const PixelView viewB{b.data(), kW, kH - 1, kRowBytes};
    EXPECT_FLOAT_EQ(ComputeFrameDiffRatio(ViewOf(a), viewB), 1.0f);
    EXPECT_FALSE(IsFrameStable(ViewOf(a), viewB));
}

TEST(FrameStabilityTest, HandlesPaddedPitch) {
    // 为什么：PixelView 契约允许 pitch > width*4（行对齐填充），
    // 采样必须按 pitch 寻址而不是假设紧凑排列
    const auto a = MakeShadeFrame(1);
    const auto b = MakeShadeFrame(1);
    // 构造 pitch 多 16 字节的副本
    const int paddedPitch = kRowBytes + 16;
    std::vector<uint8_t> pa(static_cast<size_t>(paddedPitch) * kH, 0xAA);
    std::vector<uint8_t> pb(static_cast<size_t>(paddedPitch) * kH, 0xAA);
    for (int y = 0; y < kH; ++y) {
        std::memcpy(pa.data() + static_cast<size_t>(y) * paddedPitch,
                    a.data() + static_cast<size_t>(y) * kRowBytes, kRowBytes);
        std::memcpy(pb.data() + static_cast<size_t>(y) * paddedPitch,
                    b.data() + static_cast<size_t>(y) * kRowBytes, kRowBytes);
    }
    const PixelView va{pa.data(), kW, kH, paddedPitch};
    const PixelView vb{pb.data(), kW, kH, paddedPitch};
    EXPECT_FLOAT_EQ(ComputeFrameDiffRatio(va, vb), 0.0f);
}

TEST(FrameStabilityTest, GridSamplingExactCount) {
    // 为什么：差异率 = 差异采样数/总采样数，网格必须精确可预测，
    // 否则阈值 stableDiffRatio 的语义随帧尺寸漂移
    const auto a = MakeShadeFrame(1);
    auto b = MakeShadeFrame(1);
    // 只改 1 个采样点位置 (0,0)：其 8×8 邻域内全部高对比
    PatchRegion(b, 0, 0, 8, 8);
    // kW=160, kH=120, gridStep=8 → 20×15 = 300 个采样点，其中 (0,0) 落在网格上 = 1 个差异
    const float ratio = ComputeFrameDiffRatio(ViewOf(a), ViewOf(b));
    EXPECT_NEAR(ratio, 1.0f / 300.0f, 1e-6f);
}

}  // namespace
}  // namespace shotlet
