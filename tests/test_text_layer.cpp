#include <gtest/gtest.h>

#include "core/Layer.h"
#include "utils/Colors.h"

using namespace shotlet;

namespace {
class ImGuiContextTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        ImGui::GetIO().Fonts->AddFontDefault();
        ImGui::GetIO().Fonts->Build();
    }

    void TearDown() override {
        ImGui::SetCurrentContext(context);
        ImGui::DestroyContext(context);
        context = nullptr;
    }

private:
    ImGuiContext* context = nullptr;
};
} // namespace

TEST_F(ImGuiContextTest, TextLayerSelectionFrameMatchesInputFrameGeometry) {
    TextLayer layer(ImVec2(20.0f, 30.0f), "hello", Colors::PresetRed, 17.0f, 80.0f);

    const ImRect textBounds = layer.GetBounds();
    const ImRect frame = layer.GetFrameBounds();

    EXPECT_FLOAT_EQ(frame.Min.x, textBounds.Min.x - TextLayer::FramePadding * 2.0f);
    EXPECT_FLOAT_EQ(frame.Min.y, textBounds.Min.y - TextLayer::FramePadding);
    EXPECT_FLOAT_EQ(frame.Max.x, textBounds.Max.x + TextLayer::FramePadding * 2.0f);
    EXPECT_FLOAT_EQ(frame.Max.y, textBounds.Max.y + TextLayer::FramePadding);
}

TEST_F(ImGuiContextTest, TextLayerSelectionFrameCanScalePadding) {
    TextLayer layer(ImVec2(20.0f, 30.0f), "hello", Colors::PresetRed, 17.0f, 80.0f);

    const ImRect textBounds = layer.GetBounds();
    const ImRect frame = layer.GetFrameBounds(2.0f);

    EXPECT_FLOAT_EQ(frame.Min.x, textBounds.Min.x - TextLayer::FramePadding * 4.0f);
    EXPECT_FLOAT_EQ(frame.Min.y, textBounds.Min.y - TextLayer::FramePadding * 2.0f);
    EXPECT_FLOAT_EQ(frame.Max.x, textBounds.Max.x + TextLayer::FramePadding * 4.0f);
    EXPECT_FLOAT_EQ(frame.Max.y, textBounds.Max.y + TextLayer::FramePadding * 2.0f);
}
