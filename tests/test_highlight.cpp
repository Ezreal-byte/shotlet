#include <gtest/gtest.h>
#include "core/Canvas.h"
#include "utils/MaskGeometry.h"
#include <cstring>
#include <cmath>

using namespace shotlet;
namespace {
void Prepare(Canvas& canvas) {
    constexpr int width = 320, height = 240, channels = 4;
    auto pixels = std::make_unique<unsigned char[]>(width * height * channels);
    std::memset(pixels.get(), 255, width * height * channels);
    canvas.ReplaceScreenshotAndSelectAll(1, ImVec2(width, height), ImVec2(width, height), std::move(pixels), width * channels);
    canvas.SetImageEditing(true);
    canvas.SetTool(ToolType::Highlight);
}
void Drag(Canvas& canvas, ImVec2 start, ImVec2 end) {
    canvas.OnMouseDown(start); canvas.OnMouseMove(end); canvas.OnMouseUp(end);
}
float Area(const MaskGeometry::Polygon& polygon) {
    float twiceArea = 0;
    for (size_t i = 0; i < polygon.size(); ++i) {
        const auto a = polygon[i], b = polygon[(i + 1) % polygon.size()];
        twiceArea += a.x * b.y - a.y * b.x;
    }
    return std::abs(twiceArea) / 2;
}
float Area(const std::vector<MaskGeometry::Polygon>& polygons) {
    float total = 0;
    for (const auto& polygon : polygons) total += Area(polygon);
    return total;
}
}

TEST(HighlightTest, DragThresholdShapesSelectionAndAnnotationsReuseExistingInteraction) {
    Canvas canvas;
    Prepare(canvas);
    Drag(canvas, ImVec2(30, 30), ImVec2(33, 33));
    EXPECT_TRUE(canvas.GetLayerManager().IsEmpty());
    Drag(canvas, ImVec2(30, 30), ImVec2(160, 140));
    ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 1u);
    auto* highlight = dynamic_cast<HighlightLayer*>(canvas.GetLayerManager().GetLayers()[0].get());
    ASSERT_NE(highlight, nullptr);
    EXPECT_FALSE(highlight->IsEllipse());
    EXPECT_TRUE(highlight->IsPointNearBorder(ImVec2(50, 50), 0));
    ASSERT_TRUE(canvas.SelectLayerById(highlight->GetId()));
    EXPECT_EQ(canvas.GetPropertyTool(), ToolType::Highlight);
    canvas.SetHighlightEllipse(true);
    EXPECT_TRUE(highlight->IsEllipse());
    EXPECT_FALSE(highlight->IsPointNearBorder(ImVec2(31, 31), 0));
    Drag(canvas, highlight->GetBounds().GetCenter(), ImVec2(-50, -50));
    EXPECT_FLOAT_EQ(highlight->GetBounds().Min.x, 0);
    EXPECT_FLOAT_EQ(highlight->GetBounds().Min.y, 0);
    const auto end = highlight->GetBounds().Max;
    Drag(canvas, end, ImVec2(200, 180));
    EXPECT_FLOAT_EQ(highlight->GetBounds().Max.x, 200);
    auto clone = highlight->Clone();
    EXPECT_TRUE(dynamic_cast<HighlightLayer*>(clone.get())->IsEllipse());
    // 高亮区域内仍然能继续标注，不会把箭头手势当成移动高亮。
    canvas.SetTool(ToolType::Arrow);
    Drag(canvas, ImVec2(70, 70), ImVec2(130, 100));
    ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 2u);
    EXPECT_NE(dynamic_cast<ArrowLayer*>(canvas.GetLayerManager().GetLayers()[1].get()), nullptr);
    canvas.Undo(); canvas.Undo();
    EXPECT_TRUE(canvas.GetLayerManager().IsEmpty());
}

TEST(HighlightTest, GeometrySubtractsUnionWithoutRepeatedDimming) {
    const auto outer = MaskGeometry::RoundedRectangle(ImRect(0, 0, 200, 200));
    const auto first = MaskGeometry::RoundedRectangle(ImRect(20, 20, 80, 80));
    const auto second = MaskGeometry::RoundedRectangle(ImRect(60, 60, 100, 100));
    EXPECT_NEAR(Area(MaskGeometry::SubtractHoles(outer, {first, second})), 40000 - 3600 - 1600 + 400, 0.01f);
    const auto ellipse = MaskGeometry::Ellipse(ImRect(40, 40, 160, 160));
    const auto once = MaskGeometry::SubtractHoles(outer, {ellipse});
    const auto twice = MaskGeometry::SubtractHoles(outer, {ellipse, ellipse});
    EXPECT_NEAR(Area(once), Area(twice), 0.05f);
    EXPECT_NEAR(Area(once), 40000 - std::acos(-1.0f) * 60 * 60, 10);
    EXPECT_TRUE(MaskGeometry::SubtractHoles(outer, {outer}).empty());
}

TEST(HighlightTest, TransparencyAndBothShapesSurviveDocumentRestoreAndCrop) {
    Canvas canvas;
    Prepare(canvas);
    canvas.SetHighlightTransparency(65);
    Drag(canvas, ImVec2(30, 30), ImVec2(80, 100));
    canvas.SetHighlightEllipse(true);
    Drag(canvas, ImVec2(220, 200), ImVec2(150, 130));
    std::string error;
    auto document = canvas.ExportDocument(error);
    ASSERT_TRUE(document) << error;
    ASSERT_EQ(document->layers.size(), 2u);
    EXPECT_FALSE(std::get<DocumentHighlight>(document->layers[0].data).ellipse);
    EXPECT_TRUE(std::get<DocumentHighlight>(document->layers[1].data).ellipse);
    EXPECT_EQ(document->highlightTransparency, 65);
    Canvas restored;
    ASSERT_TRUE(restored.RestoreDocument(*document, 1, error)) << error;
    EXPECT_EQ(restored.GetHighlightTransparency(), 65);
    EXPECT_NE(dynamic_cast<HighlightLayer*>(restored.GetLayerManager().GetLayers()[0].get()), nullptr);
    auto cropped = restored.ExportCroppedDocument(ImRect(10, 10, 250, 230), error);
    ASSERT_TRUE(cropped) << error;
    EXPECT_FLOAT_EQ(std::get<DocumentHighlight>(cropped->layers[0].data).start.x, 20);
    canvas.SetHighlightTransparency(200);
    EXPECT_EQ(canvas.GetHighlightTransparency(), HighlightSettings::MaximumTransparency);
    document->highlightTransparency = -1;
    EXPECT_FALSE(ValidateCaptureDocument(*document, error));
}
