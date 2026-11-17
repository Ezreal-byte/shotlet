#include <gtest/gtest.h>

#include "core/Canvas.h"

using namespace shotlet;

namespace {
void SetTestScreenshot(Canvas& canvas) {
    constexpr int width = 100;
    constexpr int height = 100;
    constexpr int rowPitch = width * 4;
    auto imageData = std::make_unique<unsigned char[]>(rowPitch * height);

    canvas.SetScreenshotOwned(
        static_cast<ImTextureID>(1),
        ImVec2(static_cast<float>(width), static_cast<float>(height)),
        ImVec2(static_cast<float>(width), static_cast<float>(height)),
        std::move(imageData),
        rowPitch);
}

void SelectTestRegion(Canvas& canvas) {
    canvas.OnMouseDown(ImVec2(10.0f, 10.0f));
    canvas.OnMouseMove(ImVec2(80.0f, 80.0f));
    canvas.OnMouseUp(ImVec2(80.0f, 80.0f));
}

// 构建坐标可辨识的测试图：像素值编码 (x,y)，便于断言"某像素是否被改写"
constexpr int kTestImageSize = 100;
std::unique_ptr<unsigned char[]> MakeEncodedTestImage(int width, int height) {
    auto data = std::make_unique<unsigned char[]>(width * height * 4);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int idx = (y * width + x) * 4;
            data[idx] = static_cast<unsigned char>(x);
            data[idx + 1] = static_cast<unsigned char>(y);
            data[idx + 2] = 128;
            data[idx + 3] = 255;
        }
    }
    return data;
}

// 在 (10,10)-(60,60) 选区内涂抹马赛克（选区足够大以避开 ±10px 的调整手柄命中区）
void SetupMosaicCanvas(Canvas& canvas) {
    constexpr int width = kTestImageSize;
    constexpr int height = kTestImageSize;
    canvas.SetScreenshotOwned(
        static_cast<ImTextureID>(1),
        ImVec2(static_cast<float>(width), static_cast<float>(height)),
        ImVec2(static_cast<float>(width), static_cast<float>(height)),
        MakeEncodedTestImage(width, height),
        width * 4);

    // 选区 (10,10)-(60,60)
    canvas.OnMouseDown(ImVec2(10.0f, 10.0f));
    canvas.OnMouseMove(ImVec2(60.0f, 60.0f));
    canvas.OnMouseUp(ImVec2(60.0f, 60.0f));

    canvas.SetTool(ToolType::Mosaic, /*clearSelection=*/false);
    canvas.OnMouseDown(ImVec2(30.0f, 30.0f));
    canvas.OnMouseMove(ImVec2(45.0f, 45.0f));
    canvas.OnMouseUp(ImVec2(45.0f, 45.0f));
}
} // namespace

// 编辑缓冲区域化的核心契约：像素级修改只发生在选区（外扩一个笔刷半径）内，
// 选区外的像素必须是原始截图数据——放大镜借用 m_imageData 展示、
// 导出裁剪都依赖这一不变式
TEST(CanvasMosaicRegionTest, MosaicEditsStayInsideSelection) {
    Canvas canvas;
    SetupMosaicCanvas(canvas);

    ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 1u);
    const auto* image = canvas.GetCurrentImageData();
    ASSERT_NE(image, nullptr);

    const int rowPitch = canvas.GetCurrentRowPitch();
    auto pixelAt = [&](int x, int y) {
        return (static_cast<int>(image[y * rowPitch + x * 4]) << 8) |
               static_cast<int>(image[y * rowPitch + x * 4 + 1]);
    };

    // 选区外的像素保持原始编码值 (x<<8 | y)
    EXPECT_EQ(pixelAt(1, 1), (1 << 8) | 1);
    EXPECT_EQ(pixelAt(80, 80), (80 << 8) | 80);
    EXPECT_EQ(pixelAt(99, 99), (99 << 8) | 99);
    // 选区内、笔刷渐变带之外的角落也不应被改动（最大笔刷半径 16）
    EXPECT_EQ(pixelAt(12, 12), (12 << 8) | 12);

    // 涂抹中心被马赛克化：值变为块平均，必然不同于原始编码值
    EXPECT_NE(pixelAt(37, 37), (37 << 8) | 37);
}

// 撤销马赛克后，选区内像素必须完整恢复原始值——
// 这条路径覆盖 RestoreRegionFromOriginal 的区域化坐标换算
TEST(CanvasMosaicRegionTest, UndoRestoresOriginalPixelsInsideSelection) {
    Canvas canvas;
    SetupMosaicCanvas(canvas);

    ASSERT_TRUE(canvas.CanUndo());
    canvas.Undo();
    ASSERT_TRUE(canvas.GetLayerManager().IsEmpty());

    const auto* image = canvas.GetCurrentImageData();
    ASSERT_NE(image, nullptr);
    const int rowPitch = canvas.GetCurrentRowPitch();

    // 撤销后整图恢复：选区内和选区外的像素都等于原始编码值
    for (const auto& [x, y] : {std::pair{30, 30}, {37, 37}, {45, 45}, {80, 80}}) {
        EXPECT_EQ(image[y * rowPitch + x * 4], static_cast<unsigned char>(x))
            << "pixel (" << x << "," << y << ") R channel not restored";
        EXPECT_EQ(image[y * rowPitch + x * 4 + 1], static_cast<unsigned char>(y))
            << "pixel (" << x << "," << y << ") G channel not restored";
    }
}

TEST(CanvasTextureUpdateTest, DeletingSelectedLayerUploadsTextureOnce) {
    Canvas canvas;

    SetTestScreenshot(canvas);

    int updateCount = 0;
    canvas.SetTextureUpdateCallback([&updateCount](const Canvas::TextureUpdateRegion*) {
        updateCount++;
    });

    SelectTestRegion(canvas);

    canvas.SetTool(ToolType::Rectangle);
    canvas.OnMouseDown(ImVec2(20.0f, 20.0f));
    canvas.OnMouseMove(ImVec2(60.0f, 60.0f));
    canvas.OnMouseUp(ImVec2(60.0f, 60.0f));

    canvas.SetTool(ToolType::None);
    canvas.OnMouseDown(ImVec2(20.0f, 20.0f));
    ASSERT_TRUE(canvas.HasSelectedLayer());

    updateCount = 0;
    canvas.OnKeyDown(SDLK_DELETE, 0, false);

    EXPECT_EQ(updateCount, 1);
}

class CanvasShapeCreationThresholdTest : public ::testing::TestWithParam<ToolType> {};

TEST_P(CanvasShapeCreationThresholdTest, IgnoresDragsThatDoNotExceedFourPixels) {
    Canvas canvas;
    SetTestScreenshot(canvas);
    SelectTestRegion(canvas);

    canvas.SetTool(GetParam());
    canvas.OnMouseDown(ImVec2(20.0f, 20.0f));
    canvas.OnMouseMove(ImVec2(24.0f, 24.0f));
    canvas.OnMouseUp(ImVec2(24.0f, 24.0f));

    EXPECT_TRUE(canvas.GetLayerManager().IsEmpty());
}

TEST_P(CanvasShapeCreationThresholdTest, DoesNotCreatePendingPreviewBeforeThreshold) {
    Canvas canvas;
    SetTestScreenshot(canvas);
    SelectTestRegion(canvas);

    canvas.SetTool(GetParam());
    canvas.OnMouseDown(ImVec2(20.0f, 20.0f));
    canvas.OnMouseMove(ImVec2(24.0f, 24.0f));
    canvas.FlushPendingInput();

    EXPECT_TRUE(canvas.GetLayerManager().IsEmpty());
}

TEST_P(CanvasShapeCreationThresholdTest, CreatesLayerWhenDragExceedsFourPixels) {
    Canvas canvas;
    SetTestScreenshot(canvas);
    SelectTestRegion(canvas);

    canvas.SetTool(GetParam());
    canvas.OnMouseDown(ImVec2(20.0f, 20.0f));
    canvas.OnMouseMove(ImVec2(25.0f, 25.0f));
    canvas.OnMouseUp(ImVec2(25.0f, 25.0f));

    ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 1u);
}

INSTANTIATE_TEST_SUITE_P(
    ShapeTools,
    CanvasShapeCreationThresholdTest,
    ::testing::Values(ToolType::Rectangle, ToolType::Ellipse, ToolType::Arrow));

TEST(CanvasFlushPendingInputTest, CommitsCurrentShapeBeforeExport) {
    Canvas canvas;
    SetTestScreenshot(canvas);
    SelectTestRegion(canvas);

    canvas.SetTool(ToolType::Rectangle);
    canvas.OnMouseDown(ImVec2(20.0f, 20.0f));
    canvas.OnMouseMove(ImVec2(60.0f, 60.0f));

    ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 0u);

    canvas.FlushPendingInput();

    ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 1u);
    EXPECT_EQ(canvas.GetLayerManager().GetLayers()[0]->GetTypeName(), std::string("Rectangle"));
}

TEST(CanvasSmartRegionTest, IgnoresSmartRegionAfterSelectionIsConfirmed) {
    Canvas canvas;
    SetTestScreenshot(canvas);

    SmartRegion smartRegion;
    smartRegion.bounds = ImRect(ImVec2(20.0f, 20.0f), ImVec2(40.0f, 40.0f));

    canvas.SetSmartRegion(&smartRegion);
    ASSERT_EQ(canvas.GetSmartRegion(), &smartRegion);

    SelectTestRegion(canvas);
    ASSERT_TRUE(canvas.HasSelection());
    const ImRect manualSelection = canvas.GetSelection();

    canvas.SetSmartRegion(&smartRegion);
    EXPECT_EQ(canvas.GetSmartRegion(), nullptr);

    canvas.OnMouseDoubleClick(ImVec2(30.0f, 30.0f));
    const ImRect selectionAfterDoubleClick = canvas.GetSelection();

    EXPECT_FLOAT_EQ(selectionAfterDoubleClick.Min.x, manualSelection.Min.x);
    EXPECT_FLOAT_EQ(selectionAfterDoubleClick.Min.y, manualSelection.Min.y);
    EXPECT_FLOAT_EQ(selectionAfterDoubleClick.Max.x, manualSelection.Max.x);
    EXPECT_FLOAT_EQ(selectionAfterDoubleClick.Max.y, manualSelection.Max.y);
}

TEST(CursorManagerTest, ScalesMosaicBrushVisualRadiusByUIScale) {
    EXPECT_FLOAT_EQ(CursorManager::GetMosaicBrushVisualRadius(10.0f, 1.0f), 10.0f);
    EXPECT_FLOAT_EQ(CursorManager::GetMosaicBrushVisualRadius(10.0f, 1.5f), 15.0f);
    EXPECT_FLOAT_EQ(CursorManager::GetMosaicBrushVisualRadius(16.0f, 2.0f), 32.0f);
}

TEST(CursorManagerTest, MosaicBrushOutlineContrastsAgainstLightBackgrounds) {
    const ImU32 fillColor = CursorManager::GetMosaicBrushFillColor();
    const ImU32 outlineColor = CursorManager::GetMosaicBrushOutlineColor();

    const int fillR = fillColor & 0xFF;
    const int outlineR = outlineColor & 0xFF;
    const int fillAlpha = (fillColor >> IM_COL32_A_SHIFT) & 0xFF;
    const int outlineAlpha = (outlineColor >> IM_COL32_A_SHIFT) & 0xFF;

    EXPECT_LT(outlineR, fillR);
    EXPECT_GT(outlineAlpha, fillAlpha);
}

TEST(CanvasLongImageTest, ReplaceSelectsEntireImageAndKeepsOriginalPixels) {
    Canvas canvas;
    SetupMosaicCanvas(canvas);
    constexpr int width = 40, height = 16384;
    auto data = MakeEncodedTestImage(width, height);
    const auto* owned = data.get();
    canvas.ReplaceScreenshotAndSelectAll(2, ImVec2(width, height), ImVec2(2, 819.2f),
                                        std::move(data), width * 4);
    EXPECT_TRUE(canvas.HasSelection());
    EXPECT_EQ(canvas.GetSelection().Min.x, 0);
    EXPECT_EQ(canvas.GetSelection().Max.y, 819.2f);
    EXPECT_EQ(canvas.GetScreenshotSize().y, height);
    EXPECT_EQ(canvas.GetCurrentImageData(), owned);
    EXPECT_TRUE(canvas.GetLayerManager().IsEmpty());
    EXPECT_EQ(canvas.GetTool(), ToolType::None);
}
