#include <gtest/gtest.h>
#include "ui/OverlayManager.h"
#include "ui/ScreenOverlay.h"
#include "ui/ImageEditor.h"

using namespace shotlet;

// 测试桩提供的观测接口（定义见 image_editor_test_impl.cpp / screen_overlay_test_impl.cpp）
namespace shotlet {
int ImageEditorRenderFrameCallCountForTest(const ImageEditor* editor);
int ScreenOverlayRenderFrameCallCountForTest();
void SetImageEditorSelectionScreenOriginForTest(const ImageEditor* editor, ImVec2 origin);
}

// Test OverlayManager initial state
TEST(OverlayManagerTest, InitialState) {
    OverlayManager manager;
    EXPECT_FALSE(manager.IsCreated());
    EXPECT_EQ(manager.GetActiveDisplayId(), -1);
    EXPECT_EQ(manager.GetActiveOverlay(), nullptr);
    EXPECT_TRUE(manager.GetOverlays().empty());
    EXPECT_FALSE(manager.IsFinished());
    EXPECT_FALSE(manager.IsCancelled());
}

// Test GetOverlay returns nullptr when no overlays exist
TEST(OverlayManagerTest, GetOverlayEmpty) {
    OverlayManager manager;
    EXPECT_EQ(manager.GetOverlay(0), nullptr);
    EXPECT_EQ(manager.GetOverlay(1), nullptr);
    EXPECT_EQ(manager.GetOverlay(-1), nullptr);
}

// Test DestroyAll on empty manager is safe
TEST(OverlayManagerTest, DestroyAllEmpty) {
    OverlayManager manager;
    manager.DestroyAll();
    EXPECT_FALSE(manager.IsCreated());
}

// Test CreateAll with null capture returns false
TEST(OverlayManagerTest, CreateAllNullCapture) {
    OverlayManager manager;
    EXPECT_FALSE(manager.CreateAll(nullptr));
    EXPECT_FALSE(manager.IsCreated());
}

// Test that ProcessFrame on empty manager is safe
TEST(OverlayManagerTest, ProcessFrameEmptyManagerIsSafe) {
    OverlayManager manager;
    // Should not crash
    manager.ProcessFrame();
    EXPECT_TRUE(true);
}

TEST(OverlayManagerTest, EscapeHidesCaptureWindowsWithoutRenderingAnotherFrame) {
    ASSERT_TRUE(SDL_InitSubSystem(SDL_INIT_EVENTS));
    struct CleanupEvents { ~CleanupEvents() { SDL_QuitSubSystem(SDL_INIT_EVENTS); } } cleanup;
    OverlayManager manager;
    for (int i = 0; i < 2; ++i) {
        auto overlay = std::make_unique<ScreenOverlay>();
        overlay->SetActive(i == 0);
        manager.AddTestOverlay(std::move(overlay));
    }
    manager.SetMagnifierVisible(true);
    const int framesBefore = ScreenOverlayRenderFrameCallCountForTest();
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = SDLK_ESCAPE;
    ASSERT_TRUE(SDL_PushEvent(&event));

    manager.ProcessFrame();

    EXPECT_TRUE(manager.IsCancelled());
    for (const auto& overlay : manager.GetOverlays()) EXPECT_TRUE(overlay->ShouldClose());
    EXPECT_FALSE(manager.IsMagnifierVisible());
    EXPECT_EQ(ScreenOverlayRenderFrameCallCountForTest(), framesBefore);
}

// Test that ProcessFrame with overlay but no active canvas doesn't crash
TEST(OverlayManagerTest, ProcessFrameWithOverlayNoActiveCanvasIsSafe) {
    OverlayManager manager;

    // Create test overlay and add to manager
    auto overlay = std::make_unique<ScreenOverlay>();
    ScreenOverlay::Config config;
    config.displayId = 0;
    config.windowWidth = 1920;
    config.windowHeight = 1080;
    config.scaleFactor = 1.0f;
    overlay->SetActive(true);

    manager.AddTestOverlay(std::move(overlay));
    manager.SetActiveDisplayId(0);

    // Magnifier starts visible
    manager.SetMagnifierVisible(true);
    EXPECT_TRUE(manager.IsMagnifierVisible());

    // ProcessFrame should not crash even when GetCanvas() returns nullptr
    // (because GetCanvas()->IsSelecting() won't be called when GetCanvas() is nullptr)
    manager.ProcessFrame();

    // Test passes if no crash occurred
    EXPECT_TRUE(manager.IsMagnifierVisible());
}

// Test that ProcessFrame hides magnifier when active overlay exists
// Note: This test verifies the magnifier can be hidden, but cannot control
// the IsSelecting state of Canvas in this test implementation
TEST(OverlayManagerTest, MagnifierVisibilityCanBeControlled) {
    OverlayManager manager;

    // Create and add overlay
    auto overlay = std::make_unique<ScreenOverlay>();
    ScreenOverlay::Config config;
    config.displayId = 0;
    config.windowWidth = 1920;
    config.windowHeight = 1080;
    config.scaleFactor = 1.0f;
    overlay->SetActive(true);
    manager.AddTestOverlay(std::move(overlay));
    manager.SetActiveDisplayId(0);

    // Magnifier can be shown
    manager.SetMagnifierVisible(true);
    EXPECT_TRUE(manager.IsMagnifierVisible());

    // Magnifier can be hidden
    manager.SetMagnifierVisible(false);
    EXPECT_FALSE(manager.IsMagnifierVisible());

    // Magnifier can be shown again
    manager.SetMagnifierVisible(true);
    EXPECT_TRUE(manager.IsMagnifierVisible());
}

// —— 图片编辑器路由（真 OverlayManager.cpp 链入测试后，验证 8 处路由分支）——
// 生产中编辑器由平台滚动截图结果创建，测试经 SetImageEditorForTest 注入。

namespace {
std::unique_ptr<ImageEditor> MakeEditor(int displayId = 7) {
    ScreenOverlay::Config config;
    config.displayId = displayId;
    return std::make_unique<ImageEditor>(config, nullptr, nullptr);
}
} // namespace

TEST(OverlayManagerEditorRoutingTest, QueriesFollowEditorState) {
    OverlayManager manager;
    auto overlay = std::make_unique<ScreenOverlay>();
    overlay->SetActive(true);
    manager.AddTestOverlay(std::move(overlay));

    auto editor = MakeEditor();
    ImageEditor* raw = editor.get();
    manager.SetImageEditorForTest(std::move(editor));

    // 编辑器存在时所有 Is* 查询完全跟随编辑器状态（overlay 状态无关）
    raw->SetRoutingStateForTest(false, false, false, false, false);
    EXPECT_FALSE(manager.IsFinished());
    EXPECT_FALSE(manager.IsCancelled());
    EXPECT_FALSE(manager.IsPinRequested());
    EXPECT_FALSE(manager.IsSaveRequested());
    EXPECT_FALSE(manager.IsOcrRequested());

    raw->SetRoutingStateForTest(true, true, true, true, true);
    EXPECT_TRUE(manager.IsFinished());
    EXPECT_TRUE(manager.IsCancelled());
    EXPECT_TRUE(manager.IsPinRequested());
    EXPECT_TRUE(manager.IsSaveRequested());
    EXPECT_TRUE(manager.IsOcrRequested());
}

TEST(OverlayManagerEditorRoutingTest, ResultContextRoutesToEditor) {
    OverlayManager manager;
    auto overlay = std::make_unique<ScreenOverlay>();
    overlay->SetActive(true);
    manager.AddTestOverlay(std::move(overlay));

    // 无编辑器：结果导出回落活跃 overlay（桩无 Canvas/窗口/上下文 → 全空）
    EXPECT_EQ(manager.GetResultCanvas(), nullptr);
    EXPECT_EQ(manager.GetResultSDLWindow(), nullptr);
    EXPECT_EQ(manager.GetResultImGuiContext(), nullptr);
    ASSERT_NE(manager.GetResultConfig(), nullptr);
    EXPECT_EQ(manager.GetResultConfig()->displayId, -1);

    auto editor = MakeEditor(7);
    ImageEditor* raw = editor.get();
    manager.SetImageEditorForTest(std::move(editor));

    // 编辑器存在：全部路由到编辑器（桩构造了非空 Canvas 与哑窗口/上下文标识）
    EXPECT_EQ(manager.GetResultCanvas(), raw->GetCanvas());
    EXPECT_NE(manager.GetResultCanvas(), nullptr);
    EXPECT_EQ(manager.GetResultSDLWindow(), raw->GetSDLWindow());
    EXPECT_NE(manager.GetResultSDLWindow(), nullptr);
    EXPECT_EQ(manager.GetResultImGuiContext(), raw->GetImGuiContext());
    EXPECT_NE(manager.GetResultImGuiContext(), nullptr);
    ASSERT_NE(manager.GetResultConfig(), nullptr);
    EXPECT_EQ(manager.GetResultConfig()->displayId, 7);
}

TEST(OverlayManagerEditorRoutingTest, DestroyAllClearsEditorAndOverlays) {
    OverlayManager manager;
    auto overlay = std::make_unique<ScreenOverlay>();
    overlay->SetActive(true);
    manager.AddTestOverlay(std::move(overlay));

    auto editor = MakeEditor();
    auto* raw = editor.get();
    manager.SetImageEditorForTest(std::move(editor));
    raw->SetRoutingStateForTest(true, true, false, false, false);
    EXPECT_TRUE(manager.IsFinished());

    // 真实现 DestroyAll：编辑器先于 overlay 销毁，之后路由回落（无 overlay 无编辑器 → 全空）
    manager.DestroyAll();
    EXPECT_FALSE(manager.IsFinished());
    EXPECT_FALSE(manager.IsCancelled());
    EXPECT_EQ(manager.GetResultCanvas(), nullptr);
    EXPECT_EQ(manager.GetResultSDLWindow(), nullptr);
    EXPECT_EQ(manager.GetResultImGuiContext(), nullptr);
    EXPECT_EQ(manager.GetResultConfig(), nullptr);
    EXPECT_TRUE(manager.GetOverlays().empty());
    EXPECT_EQ(manager.GetActiveDisplayId(), -1);
}

TEST(OverlayManagerEditorRoutingTest, ProcessFrameEditorBranchSkipsOverlayRendering) {
    OverlayManager manager;
    auto overlay = std::make_unique<ScreenOverlay>();
    overlay->SetActive(true);
    manager.AddTestOverlay(std::move(overlay));

    // 无编辑器：ProcessFrame 渲染 overlay（真实现的常规路径）
    manager.ProcessFrame();
    const int overlayFramesBefore = ScreenOverlayRenderFrameCallCountForTest();
    EXPECT_GT(overlayFramesBefore, 0);

    // 编辑器存在：进入编辑器独占分支（渲染编辑器、旁路所有 overlay）
    auto editor = MakeEditor();
    auto* raw = editor.get();
    manager.SetImageEditorForTest(std::move(editor));
    manager.ProcessFrame();
    EXPECT_EQ(ImageEditorRenderFrameCallCountForTest(raw), 1);
    EXPECT_EQ(ScreenOverlayRenderFrameCallCountForTest(), overlayFramesBefore);
}

TEST(OverlayManagerEditorRoutingTest, ClosedEditorDoesNotRenderAnotherFrame) {
    OverlayManager manager;
    auto editor = MakeEditor();
    auto* raw = editor.get();
    raw->SetRoutingStateForTest(false, true, false, false, false);
    manager.SetImageEditorForTest(std::move(editor));

    manager.ProcessFrame();

    EXPECT_TRUE(manager.IsCancelled());
    EXPECT_EQ(ImageEditorRenderFrameCallCountForTest(raw), 0);
}

TEST(OverlayManagerEditorRoutingTest, SelectionScreenOriginRoutesToEditor) {
    // Pin/OCR 结果窗口锚点：编辑器路径问编辑器（选区为图片坐标需经视图变换），
    // 不能用来源 overlay 的 windowX + 图片坐标（长图 y 可达数千，必然弹到屏幕外）
    OverlayManager manager;
    auto overlay = std::make_unique<ScreenOverlay>();
    overlay->SetActive(true);
    manager.AddTestOverlay(std::move(overlay));

    // overlay 路径：桩 overlay 无 Canvas（未 SetCaptureData），无选区来源 → 无效锚点
    EXPECT_FLOAT_EQ(manager.GetResultSelectionScreenOrigin().x, -1.0f);
    EXPECT_FLOAT_EQ(manager.GetResultSelectionScreenOrigin().y, -1.0f);

    auto editor = MakeEditor();
    auto* raw = editor.get();
    manager.SetImageEditorForTest(std::move(editor));
    SetImageEditorSelectionScreenOriginForTest(raw, ImVec2(1538, 260));
    EXPECT_FLOAT_EQ(manager.GetResultSelectionScreenOrigin().x, 1538.0f);
    EXPECT_FLOAT_EQ(manager.GetResultSelectionScreenOrigin().y, 260.0f);
}

TEST(OverlayManagerTest, ThemeChangesReachAllOverlaysAndExistingEditor) {
    OverlayManager manager;
    for (int i = 0; i < 2; ++i) manager.AddTestOverlay(std::make_unique<ScreenOverlay>());
    ScreenOverlay::Config config;
    auto editor = std::make_unique<ImageEditor>(config, nullptr, nullptr);
    auto* existingEditor = editor.get();
    manager.SetImageEditorForTest(std::move(editor));
    for (const auto theme : {Theme::Light, Theme::Dark, Theme::Light}) {
        manager.SetTheme(theme);
        for (const auto& overlay : manager.GetOverlays()) EXPECT_EQ(overlay->GetConfig().theme, theme);
        EXPECT_EQ(existingEditor->GetConfig().theme, theme);
        EXPECT_EQ(manager.GetResultCanvas(), existingEditor->GetCanvas());
        EXPECT_FALSE(manager.IsFinished());
    }
}
