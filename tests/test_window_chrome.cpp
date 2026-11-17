#include "ui/Toolbar.h"
#include "ui/WindowChrome.h"
#include "utils/WindowChrome.h"
#include <gtest/gtest.h>
#include <array>
#include <cmath>

using namespace shotlet;

namespace {
constexpr std::array DpiScales{1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 3.0f};
constexpr ImU32 ControlsBackground = IM_COL32(40, 40, 40, 255);
}

TEST(WindowChromeConsistency, EditorUsesSharedTitleBarHeight) {
    // 编辑器标题栏引用共享高度，防止单独调整后与其他窗口产生视觉差异。
    EXPECT_FLOAT_EQ(Toolbar::EditorHeight, WindowChrome::TitleBarHeight);
}

TEST(WindowChromeConsistency, WindowControlsWidthMatchesButtonCount) {
    // 控制组宽度由按钮数量决定：固定尺寸窗口省略最大化按钮后总宽随之收缩。
    EXPECT_FLOAT_EQ(WindowChrome::WindowControlsWidth(1.0f, /*includeMaximize=*/true),
                    3 * WindowChrome::WindowControlButtonWidth);
    EXPECT_FLOAT_EQ(WindowChrome::WindowControlsWidth(1.0f, /*includeMaximize=*/false),
                    2 * WindowChrome::WindowControlButtonWidth);
    for (float scale : DpiScales) {
        SCOPED_TRACE(scale);
        EXPECT_FLOAT_EQ(WindowChrome::WindowControlsWidth(scale, /*includeMaximize=*/true),
                        std::ceil(3 * WindowChrome::WindowControlButtonWidth * scale));
    }
}

namespace {
class WindowChromeControlsTest : public testing::Test {
protected:
    void SetUp() override {
        previous = ImGui::GetCurrentContext();
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
        io.Fonts->SetTexID(1);
        io.DisplaySize = ImVec2(kWidth, kHeight);
    }
    void TearDown() override {
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
    }
    void Frame(bool includeMaximize, float scale = 1.0f) {
        ImGui::GetIO().DisplaySize = ImVec2(kWidth * scale, kHeight * scale);
        const float height = WindowChrome::TitleBarHeight * scale;
        ImGui::NewFrame();
        WindowChrome::RenderWindowControls("ChromeControls", height, Theme::Dark,
                                           Language::ChineseSimplified, scale, /*maximized=*/false, includeMaximize,
                                           ControlsBackground, nullptr,
                                           [this](WindowChrome::WindowAction action) { actions.push_back(action); });
        ImGui::Render();
    }
    void Click(ImVec2 position, bool includeMaximize, float scale = 1.0f) {
        auto &io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y); Frame(includeMaximize, scale);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); Frame(includeMaximize, scale);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); Frame(includeMaximize, scale);
    }
    static constexpr float kWidth = 800, kHeight = 600;
    ImGuiContext *context = nullptr;
    ImGuiContext *previous = nullptr;
    std::vector<WindowChrome::WindowAction> actions;
};
} // namespace

TEST_F(WindowChromeControlsTest, ControlsGroupAnchorsToTopRightAtUnifiedHeight) {
    for (bool includeMaximize : {true, false}) {
        for (float scale : DpiScales) {
            SCOPED_TRACE(scale);
            auto &io = ImGui::GetIO();
            actions.clear();
            // 与各窗口一致：高度随缩放取整，控制组贴合窗口右上角。
            Frame(includeMaximize, scale); Frame(includeMaximize, scale);
            const auto *controls = ImGui::FindWindowByName("ChromeControls");
            ASSERT_NE(controls, nullptr);
            EXPECT_FLOAT_EQ(controls->Size.y, std::floor(WindowChrome::TitleBarHeight * scale));
            EXPECT_FLOAT_EQ(controls->Pos.x + controls->Size.x, io.DisplaySize.x);
            EXPECT_FLOAT_EQ(controls->Size.x,
                            WindowChrome::WindowControlsWidth(scale, includeMaximize));
            EXPECT_LE(controls->ScrollMax.y, 1);
        }
    }
}

TEST_F(WindowChromeControlsTest, ButtonsFireActionsInOrder) {
    // 三按钮布局依次触发最小化、最大化切换、关闭；任何按钮不响应都会破坏窗口控制。
    constexpr std::array expected{WindowChrome::WindowAction::Minimize,
                                   WindowChrome::WindowAction::ToggleMaximize,
                                   WindowChrome::WindowAction::Close};
    for (float scale : DpiScales) {
        SCOPED_TRACE(scale);
        actions.clear();
        Frame(true, scale); Frame(true, scale);
        const auto *controls = ImGui::FindWindowByName("ChromeControls");
        ASSERT_NE(controls, nullptr);
        const float buttonWidth = controls->Size.x / expected.size();
        for (size_t index = 0; index < expected.size(); ++index) {
            Click(ImVec2(controls->Pos.x + (index + .5f) * buttonWidth, controls->Size.y / 2), true, scale);
            ASSERT_EQ(actions.size(), index + 1);
            EXPECT_EQ(actions.back(), expected[index]);
        }
    }
}

TEST_F(WindowChromeControlsTest, NativeHitUsesEventCoordinatesBeforeMouseFrame) {
    for (bool includeMaximize : {true, false}) {
        for (float scale : DpiScales) {
            SCOPED_TRACE(scale);
            auto &io = ImGui::GetIO();
            const float width = kWidth * scale;
            const float height = WindowChrome::TitleBarHeight * scale;
            const ImVec2 blankPoint(width / 2, height / 2);
            io.AddMousePosEvent(blankPoint.x, blankPoint.y);
            Frame(includeMaximize, scale); Frame(includeMaximize, scale);
            ASSERT_FALSE(ImGui::IsAnyItemHovered());
            const auto *controls = ImGui::FindWindowByName("ChromeControls");
            ASSERT_NE(controls, nullptr);
            const ImVec2 buttonPoint(controls->Pos.x + controls->Size.x / 2, height / 2);
            // 尚未把新坐标交给 ImGui 时，原生命中仍必须保护按钮。
            EXPECT_TRUE(WindowChrome::IsWindowControlsPoint(buttonPoint, width, height, scale, includeMaximize));
            EXPECT_FALSE(WindowChrome::IsWindowControlsPoint(blankPoint, width, height, scale, includeMaximize));
            const ImVec2 belowControls(buttonPoint.x, controls->Size.y);
            EXPECT_FALSE(WindowChrome::IsWindowControlsPoint(belowControls, width, height, scale, includeMaximize));

            io.AddMousePosEvent(buttonPoint.x, buttonPoint.y);
            Frame(includeMaximize, scale);
            ASSERT_TRUE(ImGui::IsAnyItemHovered());
            // 上一帧悬停按钮也不能阻止空白标题栏开始拖动。
            EXPECT_FALSE(WindowChrome::IsWindowControlsPoint(blankPoint, width, height, scale, includeMaximize));
        }
    }
}

TEST_F(WindowChromeControlsTest, FixedSizeWindowsSkipMaximizeButton) {
    // 固定尺寸窗口（如设置）只有最小化与关闭：中间按钮不存在且宽度收缩。
    Frame(false); Frame(false);
    const auto *controls = ImGui::FindWindowByName("ChromeControls");
    ASSERT_NE(controls, nullptr);
    EXPECT_FLOAT_EQ(controls->Size.x, WindowChrome::WindowControlsWidth(1.0f, false));
    const float buttonWidth = controls->Size.x / 2;
    Click(ImVec2(controls->Pos.x + .5f * buttonWidth, controls->Size.y / 2), false);
    ASSERT_EQ(actions.size(), 1u);
    EXPECT_EQ(actions.back(), WindowChrome::WindowAction::Minimize);
    Click(ImVec2(controls->Pos.x + 1.5f * buttonWidth, controls->Size.y / 2), false);
    ASSERT_EQ(actions.size(), 2u);
    EXPECT_EQ(actions.back(), WindowChrome::WindowAction::Close);
}
