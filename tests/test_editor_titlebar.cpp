#include "ui/Toolbar.h"
#include "core/Canvas.h"
#include "utils/WindowUtils.h"
#include <gtest/gtest.h>
#include <array>
#include <cmath>

using namespace shotlet;

namespace {
class EditorTitlebarTest : public testing::Test {
protected:
    void SetUp() override {
        previous = ImGui::GetCurrentContext();
        context = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
        io.Fonts->SetTexID(1);
        canvas.ReplaceScreenshotAndSelectAll(1, ImVec2(320, 240), ImVec2(320, 240), nullptr, 1280);
        toolbar.SetCanvas(&canvas);
        toolbar.SetEditorMode(true);
        toolbar.SetWindowActionCallback([this](Toolbar::WindowAction action) { actions.push_back(action); });
        toolbar.SetConfirmCallback([this] { confirmed = true; });
    }
    void TearDown() override {
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
    }
    void Frame(float scale = 1, bool disableAnnotations = false) {
        constexpr float logicalWidth = 760, logicalHeight = 500;
        ImGui::GetIO().DisplaySize = ImVec2(logicalWidth * scale, logicalHeight * scale);
        toolbar.SetUIScale(scale);
        ImGui::NewFrame();
        ImGui::BeginDisabled(disableAnnotations);
        toolbar.Render();
        ImGui::EndDisabled();
        toolbar.RenderWindowControls();
        ImGui::Render();
    }
    void Click(ImVec2 position, bool disableAnnotations = false) {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y); Frame(1, disableAnnotations);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); Frame(1, disableAnnotations);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); Frame(1, disableAnnotations);
    }
    ImVec2 EditorActionCenter(size_t index) {
        const auto* tools = ImGui::FindWindowByName("Toolbar");
        constexpr size_t ViewActionCount = 5;
        constexpr float SeparatorWidth = 1;
        const float size = toolbar.GetSelectedIconSize();
        const float left = tools->Pos.x + tools->WindowPadding.x;
        // 第一个标注工具的位置来自实际渲染结果，反推视图操作组的间距后定位点击。
        const float gap = (toolbar.GetSelectedToolPos().x - left - static_cast<float>(ViewActionCount) * size - SeparatorWidth) /
            static_cast<float>(ViewActionCount + 1);
        return ImVec2(left + static_cast<float>(index) * (size + gap) + size / 2, tools->Pos.y + tools->Size.y / 2);
    }
    Canvas canvas;
    Toolbar toolbar;
    std::vector<Toolbar::WindowAction> actions;
    bool confirmed = false;
    ImGuiContext* context = nullptr;
    ImGuiContext* previous = nullptr;
};
}

TEST_F(EditorTitlebarTest, WindowsControlsFitBesideToolbarAtEveryDpi) {
    for (Theme theme : {Theme::Dark, Theme::Light}) {
        toolbar.SetTheme(theme);
        for (float scale : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 3.0f}) {
            SCOPED_TRACE(scale);
            Frame(scale); Frame(scale);
            const auto* tools = ImGui::FindWindowByName("Toolbar");
            const auto* controls = ImGui::FindWindowByName("EditorWindowControls");
            ASSERT_NE(tools, nullptr); ASSERT_NE(controls, nullptr);
            EXPECT_FLOAT_EQ(tools->Size.y, std::floor(Toolbar::EditorHeight * scale));
            EXPECT_FLOAT_EQ(controls->Size.y, tools->Size.y);
            EXPECT_FLOAT_EQ(controls->Pos.x, tools->Pos.x + tools->Size.x);
            EXPECT_FLOAT_EQ(controls->Pos.x + controls->Size.x, ImGui::GetIO().DisplaySize.x);
            EXPECT_LE(tools->DC.CursorMaxPos.x, controls->Pos.x);
            EXPECT_LE(controls->DC.CursorMaxPos.x, ImGui::GetIO().DisplaySize.x);
            EXPECT_LE(tools->ScrollMax.y, 1);
            EXPECT_LE(controls->ScrollMax.y, 1);
            EXPECT_FLOAT_EQ(toolbar.GetSelectedIconSize(), Toolbar::IconSize * scale);
            EXPECT_FALSE(toolbar.IsEditorDragArea(ImVec2(controls->Pos.x + controls->Size.x / 2,
                                                         controls->Pos.y + controls->Size.y / 2)));
        }
    }
}

TEST_F(EditorTitlebarTest, WindowButtonsRemainClickableWhileAnnotationToolsAreDisabled) {
    Frame(); Frame();
    const auto* controls = ImGui::FindWindowByName("EditorWindowControls");
    ASSERT_NE(controls, nullptr);
    constexpr std::array expected{Toolbar::WindowAction::Minimize, Toolbar::WindowAction::ToggleMaximize, Toolbar::WindowAction::Close};
    const float buttonWidth = controls->Size.x / expected.size();
    for (size_t index = 0; index < expected.size(); ++index) {
        Click(ImVec2(controls->Pos.x + (index + .5f) * buttonWidth, controls->Size.y / 2), true);
        ASSERT_EQ(actions.size(), index + 1);
        EXPECT_EQ(actions.back(), expected[index]);
        EXPECT_FALSE(confirmed);
    }
    toolbar.SetWindowMaximized(true);
    Click(ImVec2(controls->Pos.x + controls->Size.x / 2, controls->Size.y / 2), true);
    ASSERT_EQ(actions.size(), expected.size() + 1);
    EXPECT_EQ(actions.back(), Toolbar::WindowAction::ToggleMaximize);
}

TEST_F(EditorTitlebarTest, RemovingWindowControlsRestoresFullToolbarWidth) {
    Frame(); Frame();
    toolbar.SetWindowActionCallback({});
    Frame(); Frame();
    EXPECT_FLOAT_EQ(toolbar.GetWindowRect().GetWidth(), ImGui::GetIO().DisplaySize.x);
    EXPECT_TRUE(toolbar.IsEditorDragArea(ImVec2(ImGui::GetIO().DisplaySize.x - 1, 0)));
    const auto* controls = ImGui::FindWindowByName("EditorWindowControls");
    ASSERT_NE(controls, nullptr);
    EXPECT_FALSE(controls->Active);
}

TEST_F(EditorTitlebarTest, NativeWindowButtonsDoNotCreateAnOverlappingControlsGroup) {
    // macOS 编辑器没有窗口动作回调，应保留完整工具栏而不生成无效的自绘按钮。
    toolbar.SetWindowActionCallback({});
    Frame(); Frame();
    EXPECT_EQ(ImGui::FindWindowByName("EditorWindowControls"), nullptr);
    EXPECT_FLOAT_EQ(toolbar.GetWindowRect().GetWidth(), ImGui::GetIO().DisplaySize.x);
}

TEST_F(EditorTitlebarTest, CropFollowsRotateAndCropModeKeepsOnlyCropAndDoneInteractive) {
    canvas.SetTool(ToolType::Rectangle);
    std::vector<Toolbar::EditorAction> editorActions;
    toolbar.SetEditorActionCallback([&](Toolbar::EditorAction action) { editorActions.push_back(action); });
    Frame(); Frame();
    constexpr size_t RotateIndex = 3, CropIndex = RotateIndex + 1;
    const auto rotate = EditorActionCenter(RotateIndex), crop = EditorActionCenter(CropIndex);
    EXPECT_GT(crop.x, rotate.x);
    EXPECT_FALSE(toolbar.IsEditorDragArea(crop));
    Click(rotate);
    Click(crop);
    ASSERT_EQ(editorActions.size(), 2u);
    EXPECT_EQ(editorActions[0], Toolbar::EditorAction::Rotate);
    EXPECT_EQ(editorActions[1], Toolbar::EditorAction::Crop);

    const auto annotation = toolbar.GetSelectedToolPos();
    const float iconSize = toolbar.GetSelectedIconSize();
    toolbar.SetEditorCropping(true);
    Click(rotate);
    EXPECT_EQ(editorActions.size(), 2u);
    EXPECT_FALSE(toolbar.HasSelectedToolPos());
    Click(ImVec2(annotation.x + iconSize / 2, annotation.y + iconSize / 2));
    EXPECT_EQ(canvas.GetTool(), ToolType::Rectangle);
    Click(crop);
    ASSERT_EQ(editorActions.size(), 3u);
    EXPECT_EQ(editorActions.back(), Toolbar::EditorAction::Crop);

    const auto* tools = ImGui::FindWindowByName("Toolbar");
    constexpr float ConfirmRightInset = 16, ConfirmButtonWidth = 50;
    Click(ImVec2(tools->Pos.x + tools->Size.x - ConfirmRightInset - ConfirmButtonWidth / 2, tools->Size.y / 2));
    EXPECT_TRUE(confirmed);
}

TEST(WindowResizeBorderTest, AllEightEdgesScaleAndInteriorClicksStayInClientArea) {
    constexpr int logicalWidth = 760, logicalHeight = 500;
    constexpr float logicalBorder = 5;
    struct Hit { float x, y; SDL_HitTestResult expected; };
    constexpr std::array cases{
        Hit{0, 0, SDL_HITTEST_RESIZE_TOPLEFT}, Hit{380, 0, SDL_HITTEST_RESIZE_TOP},
        Hit{759, 0, SDL_HITTEST_RESIZE_TOPRIGHT}, Hit{759, 250, SDL_HITTEST_RESIZE_RIGHT},
        Hit{759, 499, SDL_HITTEST_RESIZE_BOTTOMRIGHT}, Hit{380, 499, SDL_HITTEST_RESIZE_BOTTOM},
        Hit{0, 499, SDL_HITTEST_RESIZE_BOTTOMLEFT}, Hit{0, 250, SDL_HITTEST_RESIZE_LEFT},
        Hit{380, 21, SDL_HITTEST_NORMAL}, Hit{380, 250, SDL_HITTEST_NORMAL}, Hit{-1, 250, SDL_HITTEST_NORMAL}
    };
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f, 3.0f})
        for (const auto& item : cases)
            EXPECT_EQ(WindowUtils::HitTestResizeBorder({int(item.x * scale), int(item.y * scale)},
                int(logicalWidth * scale), int(logicalHeight * scale), logicalBorder * scale), item.expected);
}
