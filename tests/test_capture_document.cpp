#include <gtest/gtest.h>
#include "core/Canvas.h"
#include <algorithm>
#include <cstring>
#include <limits>

using namespace shotlet;

namespace {
class CaptureDocumentTest : public testing::Test {
protected:
    void SetUp() override {
        context = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(800, 600);
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
        ImGui::NewFrame();
    }
    void TearDown() override {
        ImGui::EndFrame();
        ImGui::DestroyContext(context);
    }
    void Prepare(Canvas& canvas, float pixelScale = 1, ImVec2 start = ImVec2(10, 10), ImVec2 end = ImVec2(90, 90)) {
        constexpr int size = 200;
        auto pixels = std::make_unique<unsigned char[]>(size * size * 4);
        for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
            const int index = (y * size + x) * 4;
            pixels[index] = x; pixels[index + 1] = y; pixels[index + 2] = (x + y) % 256; pixels[index + 3] = 255;
        }
        canvas.SetScreenshotOwned(1, ImVec2(size, size), ImVec2(size / pixelScale, size / pixelScale), std::move(pixels), size * 4);
        canvas.OnMouseDown(start); canvas.OnMouseMove(end); canvas.OnMouseUp(end);
        canvas.SetImageEditing(true);
    }
    void Mosaic(Canvas& canvas, ImVec2 start, ImVec2 end) {
        canvas.SetTool(ToolType::Mosaic);
        canvas.OnMouseDown(start); canvas.OnMouseMove(end); canvas.OnMouseUp(end);
        canvas.FlushPendingInput();
    }
    std::vector<uint8_t> Crop(const Canvas& canvas, int x = 10, int y = 10, int size = 80) {
        std::vector<uint8_t> pixels(size * size * 4);
        for (int row = 0; row < size; ++row)
            std::memcpy(pixels.data() + row * size * 4,
                canvas.GetCurrentImageData() + (row + y) * canvas.GetCurrentRowPitch() + x * 4, size * 4);
        return pixels;
    }
    std::string error;
    ImGuiContext* context = nullptr;
};
}

TEST_F(CaptureDocumentTest, SevenLayerTypesKeepParametersOrderIdsAndLocalCoordinates) {
    Canvas canvas;
    Prepare(canvas, 2, ImVec2(10.5f, 11.5f), ImVec2(90.5f, 91.5f));
    canvas.SetUIScale(1.5f);
    auto rectangle = std::make_unique<RectangleLayer>(ImVec2(20, 30), ImVec2(40, 50), IM_COL32(1, 2, 3, 4), 4);
    rectangle->SetFilled(true);
    canvas.GetLayerManager().AddLayer(std::move(rectangle));
    canvas.GetLayerManager().AddLayer(std::make_unique<EllipseLayer>(ImVec2(22, 32), ImVec2(42, 52), IM_COL32(5, 6, 7, 8), 6));
    canvas.GetLayerManager().AddLayer(std::make_unique<ArrowLayer>(ImVec2(24, 34), ImVec2(44, 54), IM_COL32(9, 10, 11, 12), 2));
    canvas.GetLayerManager().AddLayer(std::make_unique<BrushLayer>(std::vector<ImVec2>{{26, 36}, {46, 56}}, IM_COL32(13, 14, 15, 16), 3));
    Mosaic(canvas, ImVec2(75, 20), ImVec2(80, 30));
    canvas.GetLayerManager().AddLayer(std::make_unique<TextLayer>(ImVec2(28, 38), "中文\n第二行", IM_COL32(17, 18, 19, 20), 23, 65));
    auto number = std::make_unique<NumberLayer>(ImVec2(60, 70), 1, IM_COL32(21, 22, 23, 24), 28);
    number->SetLabel(ImVec2(30, 40), "数字说明\n第二行", 20, 40, 1.5f);
    canvas.GetLayerManager().AddLayer(std::move(number));
    canvas.SetImageEditing(true); canvas.RotateImageCounterClockwise();
    for (const auto& layer : canvas.GetLayerManager().GetLayers()) {
        ASSERT_GT(layer->GetId(), 0u) << layer->GetTypeName();
        ASSERT_GE(layer->GetThickness(), 0) << layer->GetTypeName();
    }
    auto document = canvas.ExportDocument(error);
    ASSERT_TRUE(document) << error;
    EXPECT_EQ(document->width, 160);
    EXPECT_EQ(document->height, 160);
    EXPECT_FLOAT_EQ(document->logicalSize.x, 80);
    EXPECT_FLOAT_EQ(document->documentRenderScale, 1.5f);
    EXPECT_EQ(document->quarterTurns, 1);
    ASSERT_EQ(document->layers.size(), 7u);
    const auto& r = std::get<DocumentRectangle>(document->layers[0].data);
    EXPECT_FLOAT_EQ(r.start.x, 9.5f); EXPECT_FLOAT_EQ(r.start.y, 18.5f); EXPECT_TRUE(r.filled);
    EXPECT_EQ(document->layers[0].color.a, 4);
    const auto& t = std::get<DocumentText>(document->layers[5].data);
    EXPECT_EQ(t.text, "中文\n第二行"); EXPECT_FLOAT_EQ(t.fontSize, 23); EXPECT_FLOAT_EQ(t.wrapWidth, 65);

    Canvas restored;
    restored.SetUIScale(2);
    ASSERT_TRUE(restored.RestoreDocument(*document, 2, error)) << error;
    EXPECT_TRUE(restored.IsImageEditing());
    EXPECT_EQ(restored.GetImageQuarterTurns(), 1);
    auto second = restored.ExportDocument(error);
    ASSERT_TRUE(second) << error;
    EXPECT_EQ(second->originalRgba, document->originalRgba);
    EXPECT_FLOAT_EQ(second->cropOrigin.x, document->cropOrigin.x);
    EXPECT_FLOAT_EQ(second->cropOrigin.y, document->cropOrigin.y);
    ASSERT_EQ(second->layers.size(), document->layers.size());
    for (size_t i = 0; i < second->layers.size(); ++i) {
        EXPECT_EQ(second->layers[i].id, document->layers[i].id);
        EXPECT_EQ(second->layers[i].data.index(), document->layers[i].data.index());
        EXPECT_EQ(second->layers[i].color.r, document->layers[i].color.r);
        EXPECT_EQ(second->layers[i].color.a, document->layers[i].color.a);
        EXPECT_EQ(second->layers[i].thickness, document->layers[i].thickness);
    }
    const auto& brush = std::get<DocumentBrush>(second->layers[3].data);
    ASSERT_EQ(brush.points.size(), 2u); EXPECT_FLOAT_EQ(brush.points[1].y, 44.5f);
    EXPECT_FLOAT_EQ(std::get<DocumentNumber>(second->layers[6].data).diameter, 28);
    const auto& numberLabel = std::get<DocumentNumber>(second->layers[6].data);
    ASSERT_TRUE(numberLabel.label);
    EXPECT_EQ(numberLabel.label->text, "数字说明\n第二行");
    EXPECT_FLOAT_EQ(numberLabel.label->position.x, 19.5f);
    EXPECT_FLOAT_EQ(numberLabel.label->position.y, 28.5f);
    EXPECT_FLOAT_EQ(numberLabel.label->wrapWidth, 40);
    EXPECT_FLOAT_EQ(numberLabel.labelFrameScale, 1.5f);
}

TEST_F(CaptureDocumentTest, SnapshotReconstructsOnlySelectedOriginalWithoutChangingActiveCanvas) {
    Canvas canvas;
    Prepare(canvas);
    Mosaic(canvas, ImVec2(11, 12), ImVec2(50, 50));
    const auto before = Crop(canvas);
    auto estimate = canvas.EstimateDocumentSnapshotBytes(error);
    ASSERT_TRUE(estimate) << error;
    auto document = canvas.ExportDocument(error);
    ASSERT_TRUE(document) << error;
    EXPECT_EQ(Crop(canvas), before);
    ASSERT_EQ(document->originalRgba.size(), 80u * 80 * 4);
    for (int y = 0; y < 80; ++y) for (int x = 0; x < 80; ++x) {
        EXPECT_EQ(document->originalRgba[(y * 80 + x) * 4], x + 10);
        EXPECT_EQ(document->originalRgba[(y * 80 + x) * 4 + 1], y + 10);
    }
    ASSERT_TRUE(document->mosaicCache);
    EXPECT_LE(document->mosaicCache->rgba.size(), document->originalRgba.size());
    EXPECT_GE(*estimate, document->originalRgba.size() + document->mosaicCache->rgba.size());
    Canvas restored;
    int uploads = 0;
    restored.SetTextureUpdateCallback([&](const Canvas::TextureUpdateRegion*) { ++uploads; });
    ASSERT_TRUE(restored.RestoreDocument(*document, 2, error)) << error;
    EXPECT_EQ(uploads, 1);
    EXPECT_EQ(Crop(restored, 0, 0), before);
    restored.Undo();
    EXPECT_EQ(Crop(restored, 0, 0), document->originalRgba);
}

TEST_F(CaptureDocumentTest, MosaicStrokeEditsRetainCachedColorsAndRebuildOverlappingMask) {
    Canvas canvas;
    Prepare(canvas);
    Mosaic(canvas, ImVec2(12, 12), ImVec2(50, 50));
    Mosaic(canvas, ImVec2(35, 35), ImVec2(70, 70));
    auto document = canvas.ExportDocument(error);
    ASSERT_TRUE(document) << error;
    Canvas restored;
    ASSERT_TRUE(restored.RestoreDocument(*document, 2, error));
    const auto firstId = document->layers[0].id;
    const auto secondId = document->layers[1].id;
    const auto cache = restored.ExportDocument(error)->mosaicCache->rgba;
    ASSERT_TRUE(restored.MoveMosaicLayer(firstId, ImVec2(4, 0)));
    ASSERT_TRUE(restored.SetMosaicLayerBrushRadius(firstId, 14));
    EXPECT_EQ(restored.ExportDocument(error)->mosaicCache->rgba, cache);
    ASSERT_TRUE(restored.DeleteLayerById(firstId));
    const auto after = restored.ExportDocument(error);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->mosaicCache->rgba, cache);
    ASSERT_EQ(after->layers.size(), 1u);
    EXPECT_EQ(after->layers[0].id, secondId);
    Canvas reference;
    auto onlySecond = *document;
    onlySecond.layers.erase(onlySecond.layers.begin());
    ASSERT_TRUE(reference.RestoreDocument(onlySecond, 3, error));
    EXPECT_EQ(Crop(restored, 0, 0), Crop(reference, 0, 0));
    ASSERT_TRUE(restored.DeleteLayerById(secondId));
    EXPECT_EQ(Crop(restored, 0, 0), document->originalRgba);
}

TEST_F(CaptureDocumentTest, CanvasClickSelectsTopMosaicAndDeleteKeysRestoreUnderlyingPixels) {
    Canvas source;
    Prepare(source);
    Mosaic(source, ImVec2(30, 30), ImVec2(60, 30));
    Mosaic(source, ImVec2(40, 30), ImVec2(60, 30));
    auto document = source.ExportDocument(error);
    ASSERT_TRUE(document) << error;
    ASSERT_EQ(document->layers.size(), 2u);
    for (int key : {SDLK_DELETE, SDLK_BACKSPACE}) {
        Canvas restored;
        ASSERT_TRUE(restored.RestoreDocument(*document, 2, error)) << error;
        restored.OnMouseDown(ImVec2(35, 20)); restored.OnMouseUp(ImVec2(35, 20));
        EXPECT_EQ(restored.GetSelectedLayerId(), document->layers[1].id);
        EXPECT_EQ(restored.GetPropertyTool(), ToolType::None);
        restored.OnKeyDown(key, 0, false);
        ASSERT_EQ(restored.GetLayerManager().GetLayers().size(), 1u);
        auto onlyFirst = *document;
        onlyFirst.layers.pop_back();
        Canvas reference;
        ASSERT_TRUE(reference.RestoreDocument(onlyFirst, 3, error)) << error;
        EXPECT_EQ(Crop(restored, 0, 0), Crop(reference, 0, 0));
        restored.OnMouseDown(ImVec2(35, 20)); restored.OnMouseUp(ImVec2(35, 20));
        EXPECT_EQ(restored.GetSelectedLayerId(), document->layers[0].id);
        restored.OnKeyDown(key, 0, false);
        EXPECT_FALSE(restored.HasSelectedLayer());
        EXPECT_TRUE(restored.GetLayerManager().IsEmpty());
        EXPECT_EQ(Crop(restored, 0, 0), document->originalRgba);
    }
}

TEST_F(CaptureDocumentTest, SelectedMosaicDoesNotMoveOrChangeWhenDragged) {
    Canvas canvas;
    Prepare(canvas);
    Mosaic(canvas, ImVec2(30, 30), ImVec2(60, 30));
    const auto id = canvas.GetLayerManager().GetLayers()[0]->GetId();
    // 辅助列表选中同样退出涂抹，且不提供属性编辑。
    ASSERT_TRUE(canvas.SelectLayerById(id));
    EXPECT_EQ(canvas.GetTool(), ToolType::None);
    EXPECT_EQ(canvas.GetPropertyTool(), ToolType::None);
    const auto before = Crop(canvas);
    const auto bounds = canvas.GetLayerManager().GetLayers()[0]->GetBounds();
    canvas.OnMouseDown(ImVec2(45, 30)); canvas.OnMouseMove(ImVec2(55, 50)); canvas.OnMouseUp(ImVec2(55, 50));
    EXPECT_EQ(canvas.GetSelectedLayerId(), id);
    EXPECT_EQ(Crop(canvas), before);
    EXPECT_FLOAT_EQ(canvas.GetLayerManager().GetLayers()[0]->GetBounds().Min.x, bounds.Min.x);
    EXPECT_FLOAT_EQ(canvas.GetLayerManager().GetLayers()[0]->GetBounds().Min.y, bounds.Min.y);
    CursorManager cursor;
    CursorContext ctx{}; ctx.currentTool = ToolType::None; ctx.hasSelection = true;
    ctx.selectionRect = canvas.GetSelection(); ctx.mousePos = ImVec2(45, 30);
    ctx.selectedLayer = canvas.GetLayerManager().GetLayers()[0].get();
    EXPECT_EQ(cursor.UpdateAndSetImGui(ctx), CursorType::Hand);
}

TEST_F(CaptureDocumentTest, MosaicToolKeepsDrawingAcrossExistingStrokesAndBlankBoundsDoNotSelect) {
    Canvas canvas;
    Prepare(canvas);
    Mosaic(canvas, ImVec2(25, 25), ImVec2(65, 65));
    Mosaic(canvas, ImVec2(30, 30), ImVec2(60, 60));
    ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 2u);
    EXPECT_FALSE(canvas.HasSelectedLayer());
    canvas.SetTool(ToolType::None);
    canvas.OnMouseDown(ImVec2(25, 65)); canvas.OnMouseUp(ImVec2(25, 65));
    EXPECT_FALSE(canvas.HasSelectedLayer());
    canvas.OnMouseDown(ImVec2(45, 45)); canvas.OnMouseUp(ImVec2(45, 45));
    EXPECT_EQ(canvas.GetSelectedLayerId(), canvas.GetLayerManager().GetLayers().back()->GetId());
    // 单击形成的笔迹和重复采样点也能正确命中。
    MosaicLayer dot({{30, 30}, {30, 30}}, 15, 10);
    EXPECT_TRUE(dot.IsPointNearBorder(ImVec2(35, 30), 0));
    EXPECT_FALSE(dot.IsPointNearBorder(ImVec2(45, 30), 0));
    MosaicLayer empty({}, 15, 10);
    EXPECT_FALSE(empty.IsPointNearBorder(ImVec2(0, 0), 5));
}

TEST_F(CaptureDocumentTest, SelectedAnnotationsChooseTheirOwnPropertyTool) {
    Canvas canvas;
    Prepare(canvas);
    auto& layers = canvas.GetLayerManager();
    layers.AddLayer(std::make_unique<RectangleLayer>(ImVec2(20, 20), ImVec2(40, 40), IM_COL32_WHITE, 2));
    layers.AddLayer(std::make_unique<EllipseLayer>(ImVec2(20, 20), ImVec2(40, 40), IM_COL32_WHITE, 2));
    layers.AddLayer(std::make_unique<ArrowLayer>(ImVec2(20, 20), ImVec2(40, 40), IM_COL32_WHITE, 2));
    layers.AddLayer(std::make_unique<BrushLayer>(std::vector<ImVec2>{{20, 20}, {40, 40}}, IM_COL32_WHITE, 2));
    layers.AddLayer(std::make_unique<TextLayer>(ImVec2(20, 20), "text", IM_COL32_WHITE, 13));
    layers.AddLayer(std::make_unique<NumberLayer>(ImVec2(30, 30), 1, IM_COL32_WHITE, 24));
    const ToolType tools[] = {ToolType::Rectangle, ToolType::Ellipse, ToolType::Arrow,
                              ToolType::Brush, ToolType::Text, ToolType::Number};
    for (size_t i = 0; i < layers.GetLayers().size(); ++i) {
        ASSERT_TRUE(canvas.SelectLayerById(layers.GetLayers()[i]->GetId()));
        EXPECT_EQ(canvas.GetTool(), ToolType::None);
        EXPECT_EQ(canvas.GetPropertyTool(), tools[i]);
    }
}

TEST_F(CaptureDocumentTest, DrawingToolsCanAnnotateExistingMosaicWithoutSelectingIt) {
    for(auto tool:{ToolType::Rectangle,ToolType::Ellipse,ToolType::Arrow,ToolType::Brush,ToolType::Text,ToolType::Number}){
        Canvas canvas;Prepare(canvas);Mosaic(canvas,ImVec2(25,25),ImVec2(65,65));
        canvas.SetTool(tool);
        CursorManager cursor;CursorContext ctx{};ctx.currentTool=tool;ctx.hasSelection=true;
        ctx.selectionRect=canvas.GetSelection();ctx.mousePos=ImVec2(45,45);ctx.layers=&canvas.GetLayerManager().GetLayers();
        EXPECT_EQ(cursor.UpdateAndSetImGui(ctx),CursorType::Crosshair);
        canvas.OnMouseDown(ImVec2(35,35));canvas.OnMouseMove(ImVec2(55,55));canvas.OnMouseUp(ImVec2(55,55));
        EXPECT_EQ(canvas.GetTool(),tool);EXPECT_FALSE(canvas.HasSelectedLayer());
        if(tool==ToolType::Text){EXPECT_TRUE(canvas.IsTextInputActive());canvas.CancelTextInput();}
        else EXPECT_EQ(canvas.GetLayerManager().GetLayers().size(),2u);
    }
}

TEST_F(CaptureDocumentTest, DocumentBlockSizeChangeUpdatesAllStrokesAndRoundTrips) {
    Canvas canvas;
    Prepare(canvas);
    Mosaic(canvas, ImVec2(13, 13), ImVec2(50, 50));
    Mosaic(canvas, ImVec2(35, 35), ImVec2(70, 70));
    auto document = canvas.ExportDocument(error);
    ASSERT_TRUE(document);
    Canvas restored;
    ASSERT_TRUE(restored.RestoreDocument(*document, 2, error));
    auto before = Crop(restored, 0, 0);
    restored.SetMosaicBlockSize(7);
    EXPECT_EQ(restored.GetMosaicBlockSize(), 7);
    EXPECT_NE(Crop(restored, 0, 0), before);
    auto changed = restored.ExportDocument(error);
    ASSERT_TRUE(changed);
    EXPECT_EQ(changed->mosaicBlockSize, 7);
    Canvas reopened;
    ASSERT_TRUE(reopened.RestoreDocument(*changed, 3, error));
    EXPECT_EQ(Crop(reopened, 0, 0), Crop(restored, 0, 0));
}

TEST_F(CaptureDocumentTest, WindowDpiDoesNotRewriteSelectedTextOrDocumentGeometry) {
    Canvas canvas;
    Prepare(canvas);
    canvas.SetUIScale(1.5f);
    canvas.GetLayerManager().AddLayer(std::make_unique<TextLayer>(ImVec2(20, 30), "hello", IM_COL32_WHITE, 23, 65));
    canvas.GetLayerManager().AddLayer(std::make_unique<ArrowLayer>(ImVec2(30, 40), ImVec2(70, 70), IM_COL32_WHITE, 4));
    auto document = canvas.ExportDocument(error);
    ASSERT_TRUE(document);
    Canvas restored;
    ASSERT_TRUE(restored.RestoreDocument(*document, 2, error));
    ASSERT_TRUE(restored.SelectLayerById(document->layers[0].id));
    ImDrawList before(ImGui::GetDrawListSharedData());
    before._ResetForNewFrame(); before.PushTextureID(ImGui::GetIO().Fonts->TexID); before.PushClipRectFullScreen();
    restored.RenderClean(&before);
    for (float scale : {1.0f, 1.5f, 2.0f, 3.0f}) {
        restored.SetUIScale(scale);
        EXPECT_FLOAT_EQ(restored.GetSelectedTextLayerFontSize(), 23);
        EXPECT_FLOAT_EQ(restored.GetDocumentRenderScale(), 1.5f);
        EXPECT_FLOAT_EQ(restored.GetNumberDiameter(), 48);
        ImDrawList after(ImGui::GetDrawListSharedData());
        after._ResetForNewFrame(); after.PushTextureID(ImGui::GetIO().Fonts->TexID); after.PushClipRectFullScreen();
        restored.RenderClean(&after);
        ASSERT_EQ(after.VtxBuffer.Size, before.VtxBuffer.Size);
        for (int i = 0; i < after.VtxBuffer.Size; ++i) {
            EXPECT_EQ(after.VtxBuffer[i].pos.x, before.VtxBuffer[i].pos.x);
            EXPECT_EQ(after.VtxBuffer[i].pos.y, before.VtxBuffer[i].pos.y);
            EXPECT_EQ(after.VtxBuffer[i].col, before.VtxBuffer[i].col);
        }
    }
    // 恢复后的文字仍能进入真实文字编辑状态，取消后恢复原内容。
    restored.OnMouseDoubleClick(ImVec2(15, 25));
    EXPECT_TRUE(restored.IsTextInputActive());
    restored.CancelTextInput();
    EXPECT_FALSE(restored.IsTextInputActive());
}

TEST_F(CaptureDocumentTest, InvalidDocumentDoesNotDestroyCurrentCanvas) {
    Canvas canvas;
    Prepare(canvas);
    auto document = canvas.ExportDocument(error);
    ASSERT_TRUE(document);
    const auto before = Crop(canvas);
    for (int kind = 0; kind < 6; ++kind) {
        auto bad = *document;
        switch (kind) {
            case 0: bad.formatVersion++; break;
            case 1: bad.renderVersion++; break;
            case 2: bad.originalRgba.pop_back(); break;
            case 3: bad.logicalSize.x = std::numeric_limits<float>::quiet_NaN(); break;
            case 4: bad.quarterTurns = 4; break;
            case 5: bad.mosaicGridOffsetX = bad.mosaicBlockSize; break;
        }
        EXPECT_FALSE(canvas.RestoreDocument(bad, 2, error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(Crop(canvas), before);
        EXPECT_EQ(canvas.GetScreenshotSize().x, 200);
    }
}

TEST_F(CaptureDocumentTest, RejectsDuplicateIdsAndInvalidCacheAndExcessivePointCounts) {
    Canvas canvas;
    Prepare(canvas);
    Mosaic(canvas, ImVec2(20, 20), ImVec2(40, 40));
    auto document = canvas.ExportDocument(error);
    ASSERT_TRUE(document);
    auto duplicate = *document;
    duplicate.layers.push_back(duplicate.layers[0]);
    EXPECT_FALSE(ValidateCaptureDocument(duplicate, error));
    auto cache = *document;
    cache.mosaicCache->x = cache.width;
    EXPECT_FALSE(ValidateCaptureDocument(cache, error));
    auto points = *document;
    std::get<DocumentMosaic>(points.layers[0].data).points.resize(CaptureDocument::MaxPoints + 1);
    EXPECT_FALSE(ValidateCaptureDocument(points, error));
}

TEST_F(CaptureDocumentTest, FractionalPixelCropRetainsOnlyInteriorSamplesAndOriginalScale) {
    Canvas canvas;
    Prepare(canvas, 1, ImVec2(10.75f, 10.25f), ImVec2(90.25f, 90.75f));
    auto document = canvas.ExportDocument(error);
    ASSERT_TRUE(document) << error;
    EXPECT_EQ(document->width, 79);
    EXPECT_EQ(document->height, 81);
    EXPECT_EQ(document->originalRgba[0], 11);
    EXPECT_EQ(document->originalRgba[1], 10);
    EXPECT_FLOAT_EQ(document->sampleOffset.x, .25f);
    EXPECT_FLOAT_EQ(document->pixelScale.x, 1);
    Canvas restored;
    ASSERT_TRUE(restored.RestoreDocument(*document, 2, error)) << error;
    auto second = restored.ExportDocument(error);
    ASSERT_TRUE(second) << error;
    EXPECT_EQ(second->originalRgba, document->originalRgba);
    EXPECT_FLOAT_EQ(second->logicalSize.x, 79.5f);
    EXPECT_FLOAT_EQ(second->sampleOffset.x, .25f);
}

TEST_F(CaptureDocumentTest, PendingInputMustBeCommittedBeforeSnapshot) {
    Canvas canvas;
    Prepare(canvas);
    canvas.SetTool(ToolType::Rectangle);
    canvas.OnMouseDown(ImVec2(20, 20)); canvas.OnMouseMove(ImVec2(60, 60));
    EXPECT_FALSE(canvas.ExportDocument(error));
    canvas.FlushPendingInput();
    auto document = canvas.ExportDocument(error);
    ASSERT_TRUE(document) << error;
    EXPECT_EQ(document->layers.size(), 1u);
}

TEST(CaptureDocumentValidationTest, PixelSizeLimitsPreventOverflowBeforeAllocation) {
    EXPECT_FALSE(DocumentPixelBytes(-1, 100));
    EXPECT_FALSE(DocumentPixelBytes(100, 0));
    EXPECT_FALSE(DocumentPixelBytes(std::numeric_limits<int>::max(), std::numeric_limits<int>::max()));
    EXPECT_EQ(DocumentPixelBytes(4096, 16384), CaptureDocument::MaxPixelBytes);
    EXPECT_FALSE(DocumentPixelBytes(4097, 16384));
}
