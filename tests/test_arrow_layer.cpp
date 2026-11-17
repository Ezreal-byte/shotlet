#include <gtest/gtest.h>
#include "core/Layer.h"
#include <array>
#include <cmath>
#include <vector>

using namespace shotlet;

namespace {
constexpr int kImageSize = 480;
constexpr int kChannels = 4;
constexpr ImU32 kArrowColor = IM_COL32(150, 220, 0, 255);

class ArrowLayerTest : public testing::Test {
protected:
    void SetUp() override {
        context = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().Fonts->AddFontDefault();
        ImGui::GetIO().Fonts->Build();
    }

    void TearDown() override { ImGui::DestroyContext(context); }

    // 查询真实 ImGui 三角网格，直接检查预览轮廓，不复制箭头几何公式。
    bool PreviewContains(const ImDrawList& draw, ImVec2 point) const {
        for (int i = 0; i < draw.IdxBuffer.Size; i += 3) {
            if (ImTriangleContainsPoint(draw.VtxBuffer[draw.IdxBuffer[i]].pos,
                                        draw.VtxBuffer[draw.IdxBuffer[i + 1]].pos,
                                        draw.VtxBuffer[draw.IdxBuffer[i + 2]].pos, point)) return true;
        }
        return false;
    }

    ImGuiContext* context = nullptr;
};
} // namespace

TEST_F(ArrowLayerTest, ElongatedHeadHasSweptWingsInPreviewExportAndHitTesting) {
    const ImVec2 tip(240, 240);
    constexpr float kDiagonal = 0.70710678f;
    const std::array<ImVec2, 8> directions = {{{0, -1}, {1, 0}, {0, 1}, {-1, 0},
        {kDiagonal, kDiagonal}, {-kDiagonal, kDiagonal}, {kDiagonal, -kDiagonal}, {-kDiagonal, -kDiagonal}}};
    for (const auto direction : directions) {
        SCOPED_TRACE(testing::Message() << "direction=" << direction.x << "," << direction.y);
        const ImVec2 normal(-direction.y, direction.x);
        ArrowLayer arrow(ImVec2(tip.x - direction.x * 180, tip.y - direction.y * 180), tip, kArrowColor, 12);
        arrow.SetShowHandles(false);
        ImDrawList draw(ImGui::GetDrawListSharedData());
        draw._ResetForNewFrame();
        draw.Flags = ImDrawListFlags_None;
        arrow.Render(&draw, ImVec2(0, 0));
        std::vector<unsigned char> pixels(kImageSize * kImageSize * kChannels);
        arrow.RenderToImage(pixels.data(), kImageSize, kImageSize, kImageSize * kChannels, ImVec2(0, 0));

        // 头部后段两侧应有翼角；翼角后方保留内收空白，中间箭杆连续。
        struct Sample { float behindTip; float across; bool filled; };
        const Sample samples[] = {{96, 34, true}, {96, -34, true}, {104, 24, false},
                                  {104, -24, false}, {120, 0, true}, {40, 24, false}};
        for (const auto sample : samples) {
            const int x = static_cast<int>(std::round(tip.x - direction.x * sample.behindTip + normal.x * sample.across));
            const int y = static_cast<int>(std::round(tip.y - direction.y * sample.behindTip + normal.y * sample.across));
            const ImVec2 point(x, y);
            EXPECT_EQ(PreviewContains(draw, point), sample.filled);
            EXPECT_EQ(pixels[(y * kImageSize + x) * kChannels + 3] != 0, sample.filled);
            EXPECT_EQ(arrow.IsPointNearBorder(point, 0), sample.filled);
        }
    }
}

TEST_F(ArrowLayerTest, ExportScaleAndCropKeepThePreviewContour) {
    constexpr float kScale = 2;
    const ImVec2 cropOffset(40, 60);
    ArrowLayer original(ImVec2(100, 200), ImVec2(100, 50), kArrowColor, 6);
    std::vector<unsigned char> pixels(kImageSize * kImageSize * kChannels);
    original.RenderToImage(pixels.data(), kImageSize, kImageSize, kImageSize * kChannels, cropOffset, kScale);

    ArrowLayer preview(ImVec2(200, 400), ImVec2(200, 100), kArrowColor, 6);
    preview.SetShowHandles(false);
    ImDrawList draw(ImGui::GetDrawListSharedData());
    draw._ResetForNewFrame();
    draw.Flags = ImDrawListFlags_None;
    preview.Render(&draw, ImVec2(-cropOffset.x, -cropOffset.y), kScale);
    for (const ImVec2 point : {ImVec2(194, 136), ImVec2(126, 136), ImVec2(184, 144), ImVec2(160, 200)}) {
        const int index = (static_cast<int>(point.y) * kImageSize + static_cast<int>(point.x)) * kChannels;
        EXPECT_EQ(PreviewContains(draw, point), pixels[index + 3] != 0);
    }
}

TEST_F(ArrowLayerTest, ShortAndZeroLengthArrowsKeepFiniteGeometryWithinEndpoints) {
    for (const float length : {0.0f, 0.5f, 4.0f, 20.0f}) {
        for (const float thickness : {2.0f, 4.0f, 6.0f}) {
            ArrowLayer arrow(ImVec2(100, 100), ImVec2(100 + length, 100), kArrowColor, thickness);
            arrow.SetShowHandles(false);
            ImDrawList draw(ImGui::GetDrawListSharedData());
            draw._ResetForNewFrame();
            draw.Flags = ImDrawListFlags_None;
            arrow.Render(&draw, ImVec2(0, 0));
            for (const auto& vertex : draw.VtxBuffer) {
                EXPECT_TRUE(std::isfinite(vertex.pos.x));
                EXPECT_TRUE(std::isfinite(vertex.pos.y));
                EXPECT_GE(vertex.pos.x, 100);
                EXPECT_LE(vertex.pos.x, 100 + length);
            }
            EXPECT_EQ(draw.VtxBuffer.empty(), length == 0);
            EXPECT_TRUE(arrow.IsPointNearBorder(ImVec2(100, 100), 1));
        }
    }
}
