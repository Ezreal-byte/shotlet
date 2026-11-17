#include <gtest/gtest.h>
#include "ui/ImageEditorLayout.h"
#include "core/Canvas.h"
#include <cstring>
#include <array>

using namespace shotlet;

TEST(ImageEditorLayoutTest, InitialWindowSharesPhysicalPixelBudgetAcrossPlatforms) {
    constexpr ImVec2 imagePixels(952, 500);
    constexpr ImVec2 workAreaPixels(3840, 2160);
    struct Case { const char* name; float pixelDensity, uiScale; };
    constexpr std::array cases{
        Case{"Windows 100%", 1, 1}, Case{"Windows 125%", 1, 1.25f},
        Case{"Windows 150%", 1, 1.5f}, Case{"Windows 175%", 1, 1.75f},
        Case{"Windows 200%", 1, 2}, Case{"macOS Retina", 2, 1},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.name);
        const ImVec2 workArea(workAreaPixels.x / item.pixelDensity, workAreaPixels.y / item.pixelDensity);
        const auto size = ImageEditorInitialWindowSize(imagePixels, workArea, item.pixelDensity, item.uiScale);
        EXPECT_FLOAT_EQ(size.x * item.pixelDensity, ImageEditorLayout::TargetWindowWidthPixels);
        EXPECT_FLOAT_EQ(size.y * item.pixelDensity, ImageEditorLayout::MinimumInitialWindowHeightPixels);
    }
}

TEST(ImageEditorLayoutTest, LongImageUsesAvailableHeightAndSmallImageStaysCompact) {
    constexpr ImVec2 workArea(3840, 2160);
    constexpr float uiScale = 2;
    const auto small = ImageEditorInitialWindowSize(ImVec2(952, 500), workArea, 1, uiScale);
    const auto longImage = ImageEditorInitialWindowSize(ImVec2(1600, 12000), workArea, 1, uiScale);
    EXPECT_FLOAT_EQ(longImage.x, small.x);
    EXPECT_FLOAT_EQ(longImage.y, workArea.y - 2 * ImageEditorLayout::ScreenMargin * uiScale);
    EXPECT_LT(small.y, longImage.y);
}

TEST(ImageEditorLayoutTest, HighDpiPreservesToolbarMinimumAndSmallDisplaysKeepMargins) {
    constexpr ImVec2 imagePixels(952, 500);
    constexpr ImVec2 largeWorkArea(3840, 2160);
    constexpr float highDpiScale = 3;
    const auto large = ImageEditorInitialWindowSize(imagePixels, largeWorkArea, 1, highDpiScale);
    EXPECT_FLOAT_EQ(large.x, ImageEditorLayout::MinimumWindowWidth * highDpiScale);
    EXPECT_FLOAT_EQ(large.y, ImageEditorLayout::MinimumWindowHeight * highDpiScale);

    constexpr ImVec2 smallWorkArea(1000, 600);
    constexpr float smallDisplayScale = 2;
    const auto small = ImageEditorInitialWindowSize(imagePixels, smallWorkArea, 1, smallDisplayScale);
    const float inset = 2 * ImageEditorLayout::ScreenMargin * smallDisplayScale;
    EXPECT_FLOAT_EQ(small.x, smallWorkArea.x - inset);
    EXPECT_FLOAT_EQ(small.y, smallWorkArea.y - inset);
}

TEST(ImageEditorLayoutTest, ScrollAndZoomPreserveImageCoordinates) {
    ImageEditorTransform transform{ImVec2(48, -940), 0.5f};
    const ImVec2 logical(300, 2200);
    const ImVec2 screen = transform.ToScreen(logical);
    EXPECT_FLOAT_EQ(screen.x, 198);
    EXPECT_FLOAT_EQ(screen.y, 160);
    const auto restored = transform.ToImage(screen);
    EXPECT_FLOAT_EQ(restored.x, logical.x);
    EXPECT_FLOAT_EQ(restored.y, logical.y);
    auto clip = transform.ToImage(ImRect(0, 54, 800, 566));
    EXPECT_FLOAT_EQ(clip.Min.y, 1988);
    EXPECT_FLOAT_EQ(clip.Max.y, 3012);
}

TEST(ImageEditorLayoutTest, LongImageFitsWidthByDefaultAndCanFitEntireImage) {
    const ImVec2 image(800, 8192), viewport(900, 700);
    EXPECT_FLOAT_EQ(ImageEditorFitZoom(image, viewport, false), 1.0f);
    EXPECT_NEAR(ImageEditorFitZoom(image, viewport, true), 668.0f / 8192, 0.00001f);
    EXPECT_FLOAT_EQ(ImageEditorFitZoom(ImVec2(400, 100), viewport, false), 1.0f);
}

TEST(ImageEditorLayoutTest, PixelBufferSizeMustMatchDimensions) {
    // Create() 按 width*4 行距做全量纹理上传（读取 w*h*4 字节）：拼接/裁剪异常路径
    // 产出不一致数据时必须拒绝创建，而非越界读堆内存。
    EXPECT_TRUE(IsValidPixelBufferSize(618, 914, 618u * 914u * 4u));
    EXPECT_FALSE(IsValidPixelBufferSize(618, 914, 618u * 914u * 4u - 1));
    EXPECT_FALSE(IsValidPixelBufferSize(618, 914, 618u * 914u * 4u + 4));
    // 零尺寸/负值拒绝（旧逻辑 width<=0 的语义保留）
    EXPECT_FALSE(IsValidPixelBufferSize(0, 914, 0));
    EXPECT_FALSE(IsValidPixelBufferSize(-1, 914, 4));
    EXPECT_FALSE(IsValidPixelBufferSize(618, 0, 0));
    // 大尺寸必须用 64 位乘法：int 域 w*h*4 会溢出回绕成小值，误放行
    EXPECT_FALSE(IsValidPixelBufferSize(65536, 65536, 0));
}

TEST(ImageEditorLayoutTest, EditorSelectionScreenAnchorMapsImageToGlobalCoordinates) {
    // Pin/OCR 结果窗口定位：编辑器选区是图片坐标，必须经视图变换映射回屏幕；
    // 直接用来源 overlay 的 windowX 加图片坐标（长图 y 可达数千）会弹到屏幕外。
    const ImageEditorTransform transform{ImVec2(48, -940), 0.5f};
    const ImRect selection(100, 2000, 300, 2400);
    const ImVec2 editorWindowOrigin(1440, 200);
    const ImVec2 anchor = EditorSelectionScreenAnchor(editorWindowOrigin, transform, selection);
    // selection.Min(100,2000) -> 窗口局部 (48+50, -940+1000) = (98,60) -> 全局 (1538,260)
    EXPECT_FLOAT_EQ(anchor.x, 1538.0f);
    EXPECT_FLOAT_EQ(anchor.y, 260.0f);
    // 恒等变换时锚点=窗口原点+选区（与 overlay 路径语义对齐）
    const ImageEditorTransform identity{ImVec2(0, 0), 1.0f};
    const ImVec2 identityAnchor = EditorSelectionScreenAnchor(ImVec2(10, 20), identity, selection);
    EXPECT_FLOAT_EQ(identityAnchor.x, 110.0f);
    EXPECT_FLOAT_EQ(identityAnchor.y, 2020.0f);
}

namespace {
void SetupEditor(Canvas& canvas) {
    auto pixels = std::make_unique<unsigned char[]>(200 * 3000 * 4);
    std::memset(pixels.get(), 255, 200 * 3000 * 4);
    canvas.ReplaceScreenshotAndSelectAll(7, ImVec2(200, 3000), ImVec2(100, 1500), std::move(pixels), 800);
    canvas.SetImageEditing(true);
}
}

TEST(ImageEditorCanvasTest, BlankDragAndEdgesDoNotMoveOrCropWholeImageSelection) {
    Canvas canvas;
    SetupEditor(canvas);
    canvas.OnMouseDown(ImVec2(0, 0));
    canvas.OnMouseMove(ImVec2(50, 100));
    canvas.OnMouseUp(ImVec2(50, 100));
    canvas.OnMouseDown(ImVec2(50, 500));
    canvas.OnMouseMove(ImVec2(80, 550));
    canvas.OnMouseUp(ImVec2(80, 550));
    EXPECT_FLOAT_EQ(canvas.GetSelection().Min.x, 0);
    EXPECT_FLOAT_EQ(canvas.GetSelection().Min.y, 0);
    EXPECT_FLOAT_EQ(canvas.GetSelection().Max.x, 100);
    EXPECT_FLOAT_EQ(canvas.GetSelection().Max.y, 1500);
}

TEST(ImageEditorCanvasTest, ScrolledAnnotationUsesOriginalImageCoordinates) {
    Canvas canvas;
    SetupEditor(canvas);
    const ImageEditorTransform transform{ImVec2(40, -700), 0.75f};
    canvas.SetTool(ToolType::Rectangle);
    canvas.OnMouseDown(transform.ToImage(transform.ToScreen(ImVec2(20, 1100))));
    canvas.OnMouseMove(transform.ToImage(transform.ToScreen(ImVec2(80, 1200))));
    canvas.OnMouseUp(transform.ToImage(transform.ToScreen(ImVec2(80, 1200))));
    ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 1u);
    const auto bounds = canvas.GetLayerManager().GetLayers()[0]->GetBounds();
    EXPECT_FLOAT_EQ(bounds.Min.y, 1100);
    EXPECT_FLOAT_EQ(bounds.Max.y, 1200);
    EXPECT_FLOAT_EQ(canvas.GetScreenshotSize().y, 3000);
    canvas.Reset();
    EXPECT_FALSE(canvas.IsImageEditing());
}

TEST(ImageEditorCanvasTest, RenderTransformsOnlyImageDrawCommandsAndRestoresMouse) {
    auto* context = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    io.Fonts->AddFontDefault();
    io.Fonts->Build();
    // 测试不启动渲染后端，手动标记字体纹理已上传，避免 ImGui 在 Render 时断言。
    io.Fonts->SetTexID(1);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("editor-test", nullptr, ImGuiWindowFlags_NoDecoration);
    Canvas canvas;
    SetupEditor(canvas);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(1, 1), ImVec2(4, 4), IM_COL32_WHITE);
    const int before = draw->VtxBuffer.Size;
    const auto previous = draw->VtxBuffer[before - 1].pos;
    io.MousePos = ImVec2(100, 160);
    const ImageEditorTransform transform{ImVec2(80, -600), 0.5f};
    const ImRect viewport(0, 54, 800, 566);
    canvas.RenderInEditor(draw, transform, viewport);
    EXPECT_FLOAT_EQ(io.MousePos.x, 100);
    EXPECT_FLOAT_EQ(io.MousePos.y, 160);
    EXPECT_FLOAT_EQ(draw->VtxBuffer[before - 1].pos.y, previous.y);
    EXPECT_FLOAT_EQ(draw->VtxBuffer[before].pos.x, 80);
    EXPECT_FLOAT_EQ(draw->VtxBuffer[before].pos.y, -600);
    int images = 0;
    for (const auto& command : draw->CmdBuffer) {
        if (command.GetTexID() != 7 || command.ElemCount == 0) continue;
        ++images;
        EXPECT_GE(command.ClipRect.y, viewport.Min.y);
        EXPECT_LE(command.ClipRect.w, viewport.Max.y);
    }
    EXPECT_EQ(images, 1);
    draw->AddRectFilled(ImVec2(5, 5), ImVec2(9, 9), IM_COL32_WHITE);
    EXPECT_FLOAT_EQ(draw->VtxBuffer.back().pos.y, 9);
    ImGui::End();
    ImGui::Render();
    ImGui::DestroyContext(context);
}

TEST(ImageEditorLayoutTest, SmallImageEnlargesProportionallyWithinEditorViewport) {
    const ImVec2 viewport(1560, 880);
    const ImVec2 landscape(400, 200);
    const float zoom = ImageEditorFitZoom(landscape, viewport, true);
    EXPECT_GT(zoom, 1);
    EXPECT_FLOAT_EQ(landscape.x * zoom, viewport.x - 32);
    EXPECT_LE(landscape.y * zoom, viewport.y - 32);
    const ImVec2 portrait(200, 400);
    const float portraitZoom = ImageEditorFitZoom(portrait, viewport, true);
    EXPECT_GT(portraitZoom, 1);
    EXPECT_FLOAT_EQ(portrait.y * portraitZoom, viewport.y - 32);
    EXPECT_LE(portrait.x * portraitZoom, viewport.x - 32);
}

TEST(ImageEditorLayoutTest, QuarterTurnsPreserveInputAndNormalizeClipRectangles) {
    const ImVec2 image(100, 1500);
    const ImRect viewport(20, 54, 700, 566);
    for (int turns = 0; turns < 4; ++turns) {
        const ImageEditorTransform transform{ImVec2(40, -100), 0.5f, turns, image};
        const ImVec2 point(20, 1100);
        const ImVec2 restored = transform.ToImage(transform.ToScreen(point));
        EXPECT_FLOAT_EQ(restored.x, point.x);
        EXPECT_FLOAT_EQ(restored.y, point.y);
        const ImRect clip = transform.ToImage(viewport);
        EXPECT_GT(clip.GetWidth(), 0);
        EXPECT_GT(clip.GetHeight(), 0);
        const ImRect restoredClip = transform.ToScreen(clip);
        EXPECT_FLOAT_EQ(restoredClip.Min.x, viewport.Min.x);
        EXPECT_FLOAT_EQ(restoredClip.Min.y, viewport.Min.y);
        EXPECT_FLOAT_EQ(restoredClip.Max.x, viewport.Max.x);
        EXPECT_FLOAT_EQ(restoredClip.Max.y, viewport.Max.y);
        const ImVec2 anchor = EditorSelectionScreenAnchor(ImVec2(10, 20), transform, ImRect(ImVec2(0, 0), image));
        EXPECT_FLOAT_EQ(anchor.x, 50);
        EXPECT_FLOAT_EQ(anchor.y, -80);
    }
    const ImageEditorTransform rotated{ImVec2(0, 0), 1, 1, image};
    EXPECT_FLOAT_EQ(rotated.ToScreen(ImVec2(20, 1100)).x, 1100);
    EXPECT_FLOAT_EQ(rotated.ToScreen(ImVec2(20, 1100)).y, 80);
}

TEST(ImageEditorCanvasTest, RotationPreservesEditableAnnotationsAndCyclesBackToOriginal) {
    Canvas canvas;
    SetupEditor(canvas);
    canvas.SetTool(ToolType::Rectangle);
    canvas.OnMouseDown(ImVec2(10, 100));
    canvas.OnMouseMove(ImVec2(80, 200));
    canvas.OnMouseUp(ImVec2(80, 200));
    ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 1u);
    const auto* layer = canvas.GetLayerManager().GetLayers()[0].get();
    for (int turns = 1; turns <= 4; ++turns) {
        canvas.RotateImageCounterClockwise();
        EXPECT_EQ(canvas.GetImageQuarterTurns(), turns % 4);
        ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 1u);
        EXPECT_EQ(canvas.GetLayerManager().GetLayers()[0].get(), layer);
        EXPECT_FLOAT_EQ(layer->GetBounds().Min.y, 100);
        EXPECT_FLOAT_EQ(canvas.GetScreenshotSize().y, 3000);
    }
    canvas.RotateImageCounterClockwise();
    const ImageEditorTransform transform{ImVec2(0, 0), 1, 1, canvas.GetDisplaySize()};
    canvas.OnMouseDown(transform.ToImage(ImVec2(400, 80)));
    canvas.OnMouseMove(transform.ToImage(ImVec2(500, 20)));
    canvas.OnMouseUp(transform.ToImage(ImVec2(500, 20)));
    ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 2u);
    EXPECT_FLOAT_EQ(canvas.GetLayerManager().GetLayers()[1]->GetBounds().Min.y, 400);
    canvas.Undo();
    EXPECT_EQ(canvas.GetImageQuarterTurns(), 1);
    canvas.Reset();
    EXPECT_EQ(canvas.GetImageQuarterTurns(), 0);
}
