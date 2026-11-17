#include <gtest/gtest.h>
#include <SDL3/SDL.h>
#include "core/Canvas.h"
#include <algorithm>
#include <cstring>
#include <limits>

using namespace shotlet;

namespace {
void PrepareCanvas(Canvas& canvas, int width = 320, int height = 240) {
    auto data = std::make_unique<unsigned char[]>(width * height * 4);
    std::memset(data.get(), 255, width * height * 4);
    canvas.ReplaceScreenshotAndSelectAll(1, ImVec2(width, height), ImVec2(width, height), std::move(data), width * 4);
    canvas.SetTool(ToolType::Number);
}
void Click(Canvas& canvas, ImVec2 point) {
    canvas.OnMouseDown(point);
    canvas.OnMouseUp(point);
}
void ClickAndCommitInput(Canvas& canvas, ImVec2 point) {
    Click(canvas, point);
    // 编号、选择和拖动测试先结束自动输入，失焦点击由独立的输入测试覆盖。
    canvas.CommitTextInput();
}
NumberLayer* Marker(Canvas& canvas, size_t index) {
    return dynamic_cast<NumberLayer*>(canvas.GetLayerManager().GetLayers().at(index).get());
}
class NumberMarkerRenderTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(640, 480);
        io.DeltaTime = 1.0f / 60;
        io.FontDefault = io.Fonts->AddFontDefault();
        io.Fonts->Build();
        ImGui::NewFrame();
    }
    void TearDown() override {
        ImGui::EndFrame();
        ImGui::DestroyContext(context);
    }
    ImGuiContext* context = nullptr;
    void RenderCanvas(Canvas& canvas) {
        ImGui::Begin("数字说明测试");
        canvas.Render(ImGui::GetWindowDrawList());
        ImGui::End();
    }
};
}

TEST(NumberMarkerTest, ContinuousClicksAndDragCreateOnePointPerGesture) {
    Canvas canvas;
    PrepareCanvas(canvas);
    ClickAndCommitInput(canvas, ImVec2(50, 60));
    ClickAndCommitInput(canvas, ImVec2(110, 60));
    canvas.OnMouseDown(ImVec2(170, 60));
    canvas.OnMouseMove(ImVec2(230, 160));
    canvas.OnMouseUp(ImVec2(230, 160));
    ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 3u);
    for (size_t i = 0; i < 3; ++i) EXPECT_EQ(Marker(canvas, i)->GetNumber(), i + 1);
    EXPECT_FLOAT_EQ(Marker(canvas, 2)->GetCenter().x, 170);
    EXPECT_FLOAT_EQ(Marker(canvas, 2)->GetDiameter(), 32);
    EXPECT_EQ(canvas.GetTool(), ToolType::Number);
    EXPECT_FALSE(canvas.HasSelectedLayer());
}

TEST(NumberMarkerTest, SelectingThenClickingBlankImmediatelyCreatesNextPoint) {
    Canvas canvas;
    PrepareCanvas(canvas);
    ClickAndCommitInput(canvas, ImVec2(50, 60));
    ClickAndCommitInput(canvas, ImVec2(50, 60));
    ASSERT_TRUE(canvas.HasSelectedNumberLayer());
    ClickAndCommitInput(canvas, ImVec2(110, 60));
    ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 2u);
    EXPECT_EQ(Marker(canvas, 1)->GetNumber(), 2);
    EXPECT_FALSE(canvas.HasSelectedLayer());
}

TEST(NumberMarkerTest, DeleteMiddleRenumbersAndAppendContinues) {
    Canvas canvas;
    PrepareCanvas(canvas);
    ClickAndCommitInput(canvas, ImVec2(50, 60));
    ClickAndCommitInput(canvas, ImVec2(110, 60));
    ClickAndCommitInput(canvas, ImVec2(170, 60));
    ClickAndCommitInput(canvas, ImVec2(110, 60));
    canvas.OnKeyDown(SDLK_DELETE, 0, false);
    ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 2u);
    EXPECT_EQ(Marker(canvas, 1)->GetNumber(), 2);
    EXPECT_FLOAT_EQ(Marker(canvas, 1)->GetCenter().x, 170);
    ClickAndCommitInput(canvas, ImVec2(230, 60));
    EXPECT_EQ(Marker(canvas, 2)->GetNumber(), 3);
    canvas.Undo();
    ClickAndCommitInput(canvas, ImVec2(230, 60));
    EXPECT_EQ(Marker(canvas, 2)->GetNumber(), 3);
}

TEST(NumberMarkerTest, MixedLayersKeepOrderAndOtherProperties) {
    LayerManager manager;
    manager.AddLayer(std::make_unique<NumberLayer>(ImVec2(20, 20), 50, Colors::PresetRed, 24));
    auto text = std::make_unique<TextLayer>(ImVec2(50, 20), "说明", Colors::PresetRed, 17);
    Layer* textPointer = text.get();
    manager.AddLayer(std::move(text));
    auto second = std::make_unique<NumberLayer>(ImVec2(80, 20), 90, Colors::PresetRed, 40);
    auto* secondPointer = second.get();
    manager.AddLayer(std::move(second));
    manager.RemoveLayer(manager.GetLayers()[0].get());
    EXPECT_EQ(manager.GetLayers()[0].get(), textPointer);
    EXPECT_EQ(manager.GetLayers()[1].get(), secondPointer);
    EXPECT_EQ(secondPointer->GetNumber(), 1);
    EXPECT_FLOAT_EQ(secondPointer->GetDiameter(), 40);
    manager.RemoveLayer(textPointer);
    EXPECT_EQ(secondPointer->GetNumber(), 1);
}

TEST(NumberMarkerTest, MovementClampsToSelectionAndDoesNotRenumber) {
    Canvas canvas;
    PrepareCanvas(canvas);
    ClickAndCommitInput(canvas, ImVec2(90, 60));
    ClickAndCommitInput(canvas, ImVec2(170, 60));
    canvas.OnMouseDown(ImVec2(170, 60));
    canvas.OnMouseMove(ImVec2(-100, 120));
    canvas.OnMouseUp(ImVec2(-100, 120));
    ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 2u);
    EXPECT_EQ(Marker(canvas, 1)->GetNumber(), 2);
    EXPECT_FLOAT_EQ(Marker(canvas, 1)->GetCircleBounds().Min.x, 0);
}

TEST(NumberMarkerTest, EdgeCreationAndOutsideClicksRespectSelection) {
    Canvas canvas;
    PrepareCanvas(canvas);
    ClickAndCommitInput(canvas, ImVec2(-10, 60));
    EXPECT_TRUE(canvas.GetLayerManager().IsEmpty());
    ClickAndCommitInput(canvas, ImVec2(2, 60));
    ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 1u);
    EXPECT_FLOAT_EQ(Marker(canvas, 0)->GetBounds().Min.x, 0);
    EXPECT_EQ(Marker(canvas, 0)->GetNumber(), 1);
}

TEST(NumberMarkerTest, TooSmallSelectionDoesNotConsumeNumberOrAcceptOversize) {
    Canvas canvas;
    PrepareCanvas(canvas, 30, 70);
    canvas.SetImageEditing(true);
    ClickAndCommitInput(canvas, ImVec2(15, 35));
    EXPECT_TRUE(canvas.GetLayerManager().IsEmpty());
    canvas.SetNumberSizeIndex(0);
    ClickAndCommitInput(canvas, ImVec2(15, 35));
    ASSERT_EQ(Marker(canvas, 0)->GetNumber(), 1);
    ClickAndCommitInput(canvas, ImVec2(15, 35));
    canvas.SetNumberSizeIndex(2);
    EXPECT_FLOAT_EQ(Marker(canvas, 0)->GetDiameter(), 24);
    EXPECT_EQ(canvas.GetNumberSizeIndex(), 0);
}

TEST(NumberMarkerTest, SelectedStyleChangesApplyToNextPoint) {
    Canvas canvas;
    PrepareCanvas(canvas);
    ClickAndCommitInput(canvas, ImVec2(20, 60));
    ClickAndCommitInput(canvas, ImVec2(20, 60));
    canvas.SetNumberSizeIndex(2);
    canvas.SetSelectedLayerColor(Colors::PresetBlue);
    EXPECT_FLOAT_EQ(Marker(canvas, 0)->GetDiameter(), 40);
    EXPECT_GE(Marker(canvas, 0)->GetBounds().Min.x, 0);
    ClickAndCommitInput(canvas, ImVec2(110, 60));
    EXPECT_FLOAT_EQ(Marker(canvas, 1)->GetDiameter(), 40);
    EXPECT_EQ(Marker(canvas, 1)->GetColor(), Colors::PresetBlue);
    EXPECT_FALSE(Marker(canvas, 1)->SupportsFontSize());
}

TEST(NumberMarkerTest, SwitchingToolEscapeAndResetCancelPendingPoint) {
    Canvas canvas;
    PrepareCanvas(canvas);
    canvas.OnMouseDown(ImVec2(50, 60));
    canvas.SetTool(ToolType::Arrow);
    canvas.OnMouseUp(ImVec2(50, 60));
    EXPECT_TRUE(canvas.GetLayerManager().IsEmpty());
    canvas.SetTool(ToolType::Number);
    canvas.OnMouseDown(ImVec2(50, 60));
    canvas.OnKeyDown(SDLK_ESCAPE, 0, false);
    canvas.OnMouseUp(ImVec2(50, 60));
    EXPECT_TRUE(canvas.GetLayerManager().IsEmpty());
    ClickAndCommitInput(canvas, ImVec2(50, 60));
    ClickAndCommitInput(canvas, ImVec2(50, 60));
    canvas.Reset();
    PrepareCanvas(canvas);
    ClickAndCommitInput(canvas, ImVec2(50, 60));
    EXPECT_EQ(Marker(canvas, 0)->GetNumber(), 1);
}

TEST(NumberMarkerTest, DoubleClickEventCanCreateNextPoint) {
    Canvas canvas;
    PrepareCanvas(canvas);
    ClickAndCommitInput(canvas, ImVec2(50, 60));
    canvas.OnMouseDoubleClick(ImVec2(110, 60));
    canvas.OnMouseUp(ImVec2(110, 60));
    ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 2u);
    EXPECT_EQ(Marker(canvas, 1)->GetNumber(), 2);
}

TEST(NumberMarkerTest, CloneAndCircularHitTestPreserveGeometry) {
    NumberLayer layer(ImVec2(100, 100), 7, Colors::PresetBlue, 40);
    EXPECT_TRUE(layer.IsPointNearBorder(ImVec2(100, 100), 5));
    EXPECT_FALSE(layer.IsPointNearBorder(ImVec2(119, 119), 5));
    layer.StartDrag(ImVec2(100, 100));
    layer.UpdateDrag(ImVec2(10, 20));
    layer.EndDrag();
    auto clone = layer.Clone();
    const auto* marker = dynamic_cast<const NumberLayer*>(clone.get());
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->GetNumber(), 7);
    EXPECT_EQ(marker->GetColor(), Colors::PresetBlue);
    EXPECT_FLOAT_EQ(marker->GetCenter().x, 110);
    EXPECT_FLOAT_EQ(marker->GetCenter().y, 120);
    EXPECT_FALSE(marker->IsDragging());
}

TEST(NumberMarkerTest, BackspaceAllAndToolSwitchKeepSequenceCorrect) {
    Canvas canvas;
    PrepareCanvas(canvas);
    ClickAndCommitInput(canvas, ImVec2(50, 60));
    canvas.SetTool(ToolType::Brush);
    canvas.SetTool(ToolType::Number);
    ClickAndCommitInput(canvas, ImVec2(110, 60));
    EXPECT_EQ(Marker(canvas, 1)->GetNumber(), 2);
    ClickAndCommitInput(canvas, ImVec2(50, 60));
    canvas.OnKeyDown(SDLK_BACKSPACE, 0, false);
    EXPECT_EQ(Marker(canvas, 0)->GetNumber(), 1);
    ClickAndCommitInput(canvas, ImVec2(110, 60));
    canvas.OnKeyDown(SDLK_BACKSPACE, 0, false);
    ClickAndCommitInput(canvas, ImVec2(170, 60));
    EXPECT_EQ(Marker(canvas, 0)->GetNumber(), 1);
}

TEST(NumberMarkerTest, NumberBoundsPreventSelectionFromShrinkingPastCircle) {
    Canvas canvas;
    PrepareCanvas(canvas);
    ClickAndCommitInput(canvas, ImVec2(50, 60));
    canvas.SetTool(ToolType::None);
    // 拖动右侧选区手柄向左，不能越过圆的右边界。
    canvas.OnMouseDown(ImVec2(320, 120));
    canvas.OnMouseMove(ImVec2(40, 120));
    canvas.OnMouseUp(ImVec2(40, 120));
    EXPECT_GE(canvas.GetSelection().Max.x, Marker(canvas, 0)->GetBounds().Max.x);
}

TEST_F(NumberMarkerRenderTest, MultiDigitInkIsCenteredInsideCircleWithBackgroundColorRule) {
    for (const int number : {9, 10, 99, 100}) {
        for (const ImU32 background : {Colors::PresetBlue, Colors::PresetGreen, Colors::PresetYellow,
                                     Colors::PresetGray, Colors::PresetWhite, Colors::PresetRed,
                                     IM_COL32(255, 255, 255, 255)}) {
            ImDrawList list(ImGui::GetDrawListSharedData());
            list._ResetForNewFrame();
            list.PushClipRectFullScreen();
            NumberLayer layer(ImVec2(100, 100), number, background, 32);
            layer.Render(&list, ImVec2(20, 30), 2);
            ImVec2 min(1000, 1000), max(-1000, -1000);
            const ImU32 expected = background == Colors::PresetWhite || background == IM_COL32(255, 255, 255, 255)
                ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
            int glyphVertices = 0;
            for (const auto& vertex : list.VtxBuffer) {
                if (vertex.col != expected) continue;
                ++glyphVertices;
                min.x = std::min(min.x, vertex.pos.x); min.y = std::min(min.y, vertex.pos.y);
                max.x = std::max(max.x, vertex.pos.x); max.y = std::max(max.y, vertex.pos.y);
                const float dx = vertex.pos.x - 120, dy = vertex.pos.y - 130;
                EXPECT_LE(dx * dx + dy * dy, 16 * 16);
            }
            EXPECT_EQ(glyphVertices, std::to_string(number).size() * 4);
            EXPECT_NEAR((min.x + max.x) * 0.5f, 120, 1);
            EXPECT_NEAR((min.y + max.y) * 0.5f, 130, 1);
        }
    }
}

TEST_F(NumberMarkerRenderTest, CleanRenderMatchesLayerAndExcludesSelectionUI) {
    Canvas canvas;
    PrepareCanvas(canvas);
    canvas.SetUIScale(2);
    ClickAndCommitInput(canvas, ImVec2(80, 100));
    ClickAndCommitInput(canvas, ImVec2(80, 100));
    ASSERT_TRUE(canvas.HasSelectedNumberLayer());
    EXPECT_FLOAT_EQ(Marker(canvas, 0)->GetDiameter(), 64);
    ImDrawList clean(ImGui::GetDrawListSharedData()), direct(ImGui::GetDrawListSharedData());
    clean._ResetForNewFrame(); direct._ResetForNewFrame();
    clean.PushClipRectFullScreen(); direct.PushClipRectFullScreen();
    const ImVec2 offset(-10, -20);
    canvas.RenderClean(&clean, offset);
    Marker(canvas, 0)->Render(&direct, offset, 2);
    ASSERT_EQ(clean.VtxBuffer.Size, direct.VtxBuffer.Size + 4);
    for (int i = 0; i < direct.VtxBuffer.Size; ++i) {
        EXPECT_FLOAT_EQ(clean.VtxBuffer[i + 4].pos.x, direct.VtxBuffer[i].pos.x);
        EXPECT_FLOAT_EQ(clean.VtxBuffer[i + 4].pos.y, direct.VtxBuffer[i].pos.y);
        EXPECT_EQ(clean.VtxBuffer[i + 4].col, direct.VtxBuffer[i].col);
    }
}

TEST(NumberMarkerTest, DraggingCircleAndLabelPreservesOtherPartAndClone) {
    NumberLayer marker(ImVec2(150, 180), 1, Colors::PresetBlue, 32);
    marker.InitializeLabel(ImRect(0, 0, 400, 300), 24, 1);
    marker.GetLabel()->SetText("说明");
    const auto position = marker.GetLabel()->GetPosition();
    const auto initialFrame = marker.GetLabelFrameBounds();
    const auto [start, end] = marker.GetConnector();
    EXPECT_FLOAT_EQ(start.x, marker.GetCenter().x);
    EXPECT_FLOAT_EQ(start.y, initialFrame.Max.y);
    EXPECT_FLOAT_EQ(end.y, marker.GetCircleBounds().Min.y);
    marker.StartDrag(marker.GetCenter());
    EXPECT_FLOAT_EQ(marker.GetDragBounds().Min.x, marker.GetCircleBounds().Min.x);
    marker.UpdateDrag(ImVec2(80, -150));
    marker.EndDrag();
    EXPECT_FLOAT_EQ(marker.GetLabel()->GetPosition().x, position.x);
    EXPECT_FLOAT_EQ(marker.GetLabel()->GetPosition().y, position.y);
    EXPECT_FLOAT_EQ(marker.GetConnector().first.y, initialFrame.Min.y);
    const auto movedCenter = marker.GetCenter();
    marker.StartDrag(initialFrame.GetCenter());
    marker.UpdateDrag(ImVec2(10, 20));
    marker.EndDrag();
    EXPECT_FLOAT_EQ(marker.GetCenter().x, movedCenter.x);
    EXPECT_FLOAT_EQ(marker.GetCenter().y, movedCenter.y);
    auto clone = marker.Clone();
    const auto* copy = dynamic_cast<NumberLayer*>(clone.get());
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->GetLabel()->GetText(), "说明");
    EXPECT_FLOAT_EQ(copy->GetLabel()->GetPosition().x, position.x + 10);
    EXPECT_FLOAT_EQ(copy->GetLabel()->GetPosition().y, position.y + 20);
    marker.GetLabel()->SetText("修改");
    EXPECT_EQ(dynamic_cast<NumberLayer*>(clone.get())->GetLabel()->GetText(), "说明");
}

TEST(NumberMarkerTest, ConnectorChangesEdgesWithoutEnteringFrameOrCircle) {
    NumberLayer marker(ImVec2(200, 200), 1, Colors::PresetBlue, 32);
    marker.SetLabel(ImVec2(110, 105), "", 20, 80, 1);
    const auto frame = marker.GetLabelFrameBounds();
    const auto middle = frame.GetCenter();
    for (const auto center : {ImVec2(middle.x, 220), ImVec2(middle.x, 20), ImVec2(frame.Min.x + 5, 220),
                              ImVec2(20, middle.y), ImVec2(300, middle.y), ImVec2(300, 20)}) {
        marker.SetCenter(center);
        const auto [start, end] = marker.GetConnector();
        EXPECT_TRUE(start.x == frame.Min.x || start.x == frame.Max.x ||
                    start.y == frame.Min.y || start.y == frame.Max.y);
        EXPECT_NEAR(std::hypot(end.x - center.x, end.y - center.y), marker.GetDiameter() / 2, 0.001f);
        const ImVec2 midpoint((start.x + end.x) / 2, (start.y + end.y) / 2);
        EXPECT_FALSE(frame.Contains(midpoint));
        EXPECT_FALSE(marker.ContainsCircle(midpoint));
        if (center.x >= frame.Min.x && center.x <= frame.Max.x) EXPECT_FLOAT_EQ(start.x, center.x);
    }
    marker.SetCenter(middle);
    const auto [start, end] = marker.GetConnector();
    EXPECT_TRUE(std::isfinite(start.x));
    EXPECT_FLOAT_EQ(start.x, end.x);
    EXPECT_FLOAT_EQ(start.y, end.y);
}

TEST(NumberMarkerTest, HighDpiPaddingFitsNarrowSelections) {
    constexpr float selectionWidth = 30;
    const ImRect selection(0, 0, selectionWidth, 240);
    NumberLayer marker(ImVec2(selectionWidth / 2, 180), 1, Colors::PresetBlue, 24);
    marker.InitializeLabel(selection, 24, 3);
    const auto frame = marker.GetLabelFrameBounds();
    EXPECT_GE(frame.Min.x, selection.Min.x);
    EXPECT_LE(frame.Max.x, selection.Max.x);
    EXPECT_GT(marker.GetLabel()->GetWrapWidth(), 0);
    EXPECT_GE(marker.GetLabelFrameScale(), 1);
    EXPECT_FLOAT_EQ(marker.GetConnector().first.x, marker.GetCenter().x);
}

TEST_F(NumberMarkerRenderTest, OutsideClickOnlyEndsInputAndNextClickCreatesMarker) {
    Canvas canvas;
    PrepareCanvas(canvas, 640, 480);
    Click(canvas, ImVec2(200, 300));
    ASSERT_TRUE(canvas.IsTextInputActive());
    ImGui::GetIO().InputQueueCharacters.push_back(0x4E2D); // 中
    ImGui::GetIO().InputQueueCharacters.push_back('A');
    RenderCanvas(canvas);
    EXPECT_EQ(canvas.GetTextInputText(), "中A");
    Click(canvas, ImVec2(450, 300));
    ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 1u);
    EXPECT_FALSE(canvas.IsTextInputActive());
    EXPECT_FALSE(canvas.HasSelectedLayer());
    EXPECT_EQ(Marker(canvas, 0)->GetLabel()->GetText(), "中A");
    Click(canvas, ImVec2(450, 300));
    ASSERT_EQ(canvas.GetLayerManager().GetLayers().size(), 2u);
    EXPECT_EQ(Marker(canvas, 0)->GetLabel()->GetText(), "中A");
    EXPECT_TRUE(canvas.IsTextInputActive());
    canvas.OnKeyDown(SDLK_ESCAPE, 0, false);
    EXPECT_FALSE(canvas.IsTextInputActive());
    const auto labelPosition = Marker(canvas, 0)->GetLabel()->GetPosition();
    canvas.OnMouseDown(ImVec2(200, 300));
    canvas.OnMouseMove(ImVec2(200, 100));
    canvas.OnMouseUp(ImVec2(200, 100));
    EXPECT_FLOAT_EQ(Marker(canvas, 0)->GetLabel()->GetPosition().y, labelPosition.y);
    EXPECT_FLOAT_EQ(Marker(canvas, 0)->GetConnector().first.y, Marker(canvas, 0)->GetLabelFrameBounds().Min.y);
    canvas.OnMouseDoubleClick(labelPosition);
    ASSERT_TRUE(canvas.IsTextInputActive());
    ImGui::GetIO().InputQueueCharacters.push_back('B');
    RenderCanvas(canvas);
    canvas.CancelTextInput();
    EXPECT_EQ(Marker(canvas, 0)->GetLabel()->GetText(), "中A");
    canvas.OnMouseDoubleClick(labelPosition);
    ImGui::GetIO().InputQueueCharacters.push_back('C');
    RenderCanvas(canvas);
    canvas.SetTool(ToolType::Brush);
    EXPECT_FALSE(canvas.IsTextInputActive());
    EXPECT_EQ(Marker(canvas, 0)->GetLabel()->GetText(), "中AC");
    canvas.SetTool(ToolType::Number);
    Click(canvas, ImVec2(450, 400));
    canvas.Undo();
    EXPECT_FALSE(canvas.IsTextInputActive());
    EXPECT_EQ(canvas.GetLayerManager().GetLayers().size(), 2u);
}

TEST_F(NumberMarkerRenderTest, EmptyLabelHiddenUntilSelectedAndCanBeEditedAgain) {
    Canvas canvas;
    PrepareCanvas(canvas, 640, 480);
    ClickAndCommitInput(canvas, ImVec2(200, 300));
    auto* marker = Marker(canvas, 0);
    ASSERT_NE(marker->GetLabel(), nullptr);
    const auto frame = marker->GetLabelFrameBounds();
    const auto labelPoint = frame.GetCenter();
    // 未选中时空说明框不渲染、不参与命中和内容边界。
    EXPECT_FALSE(marker->IsLabelVisible());
    EXPECT_FALSE(marker->IsPointNearBorder(labelPoint, 5));
    EXPECT_FLOAT_EQ(marker->GetBounds().Min.y, marker->GetCircleBounds().Min.y);
    ImDrawList hidden(ImGui::GetDrawListSharedData()), circleOnly(ImGui::GetDrawListSharedData());
    hidden._ResetForNewFrame(); circleOnly._ResetForNewFrame();
    hidden.PushClipRectFullScreen(); circleOnly.PushClipRectFullScreen();
    marker->Render(&hidden, ImVec2(0, 0));
    NumberLayer bare(marker->GetCenter(), marker->GetNumber(), marker->GetColor(), marker->GetDiameter());
    bare.Render(&circleOnly, ImVec2(0, 0));
    EXPECT_EQ(hidden.VtxBuffer.Size, circleOnly.VtxBuffer.Size);

    // 选中后在选中 UI 中显示空说明框，可拖动和双击编辑。
    ASSERT_TRUE(canvas.SelectLayerById(marker->GetId()));
    EXPECT_TRUE(marker->IsLabelVisible());
    EXPECT_TRUE(marker->IsPointNearBorder(labelPoint, 5));
    ImDrawList selectionUI(ImGui::GetDrawListSharedData());
    selectionUI._ResetForNewFrame();
    selectionUI.PushClipRectFullScreen();
    marker->RenderSelectionUI(&selectionUI, 1);
    bool frameDrawn = false;
    for (const auto& vertex : selectionUI.VtxBuffer)
        if (vertex.col == marker->GetColor() && std::fabs(vertex.pos.y - frame.Min.y) < 2) frameDrawn = true;
    EXPECT_TRUE(frameDrawn);
    canvas.OnMouseDoubleClick(labelPoint);
    ASSERT_TRUE(canvas.IsTextInputActive());
    ImGui::GetIO().InputQueueCharacters.push_back('X');
    RenderCanvas(canvas);
    canvas.CommitTextInput();
    EXPECT_EQ(marker->GetLabel()->GetText(), "X");
    EXPECT_TRUE(marker->IsLabelVisible());

    // 清空文字后再次隐藏；双击圆点仍可重新编辑。
    canvas.OnMouseDoubleClick(marker->GetCenter());
    ASSERT_TRUE(canvas.IsTextInputActive());
    canvas.OnKeyDown(SDLK_BACKSPACE, 0, false);
    RenderCanvas(canvas);
    canvas.CommitTextInput();
    EXPECT_TRUE(marker->GetLabel()->GetText().empty());
    EXPECT_FALSE(marker->IsLabelVisible());
}

TEST_F(NumberMarkerRenderTest, LabelDefaultWidthMatchesTextToolAndGrowsWithContent) {
    constexpr float fontSize = 24;
    for (const float frameScale : {1.0f, 2.0f, 3.0f}) {
        NumberLayer marker(ImVec2(200, 300), 1, Colors::PresetBlue, 32);
        marker.InitializeLabel(ImRect(0, 0, 640, 480), fontSize, frameScale);
        auto* label = marker.GetLabel();
        TextLayer text(label->GetPosition(), "", label->GetColor(), fontSize, label->GetWrapWidth());
        const float initialWidth = marker.GetLabelFrameBounds().GetWidth();
        EXPECT_FLOAT_EQ(initialWidth, text.GetFrameBounds(frameScale).GetWidth());
        label->SetText("说明输入框");
        text.SetText(label->GetText());
        EXPECT_GT(marker.GetLabelFrameBounds().GetWidth(), initialWidth);
        EXPECT_FLOAT_EQ(marker.GetLabelFrameBounds().GetWidth(), text.GetFrameBounds(frameScale).GetWidth());
    }
}

TEST_F(NumberMarkerRenderTest, LabelUsesMarkerColorWithoutCornerHandlesInEveryState) {
    Canvas canvas;
    PrepareCanvas(canvas, 640, 480);
    canvas.SetImageEditing(true);
    canvas.SetToolColor(Colors::PresetBlue);
    Click(canvas, ImVec2(200, 300));
    auto* marker = Marker(canvas, 0);
    const auto verifyLabelStyle = [&] {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(640, 480));
        ImGui::Begin("数字说明边框测试", nullptr, ImGuiWindowFlags_NoDecoration);
        auto* list = ImGui::GetWindowDrawList();
        const int firstVertex = list->VtxBuffer.Size;
        canvas.Render(list);
        auto area = marker->GetLabelFrameBounds();
        area.Expand(IStyle::HandleRadius * 2);
        int coloredVertices = 0;
        // 检查框内及四角外侧的实际绘制数据，捕获灰边、白色角点和选中红边的回归。
        for (int i = firstVertex; i < list->VtxBuffer.Size; ++i) {
            const auto& vertex = list->VtxBuffer[i];
            if (!(vertex.col & IM_COL32_A_MASK) || !area.Contains(vertex.pos)) continue;
            EXPECT_EQ(vertex.col & ~IM_COL32_A_MASK, marker->GetColor() & ~IM_COL32_A_MASK);
            ++coloredVertices;
        }
        EXPECT_GT(coloredVertices, 0);
        ImGui::End();
    };
    ASSERT_TRUE(canvas.IsTextInputActive());
    verifyLabelStyle();
    canvas.SetSelectedLayerColor(Colors::PresetGreen);
    EXPECT_EQ(marker->GetColor(), Colors::PresetGreen);
    EXPECT_EQ(canvas.GetSelectedLayerColor(), Colors::PresetGreen);
    verifyLabelStyle();
    canvas.CommitTextInput();
    ASSERT_TRUE(canvas.SelectLayerById(marker->GetId()));
    canvas.SetSelectedLayerColor(Colors::PresetBlue);
    verifyLabelStyle();
    canvas.OnMouseDoubleClick(marker->GetLabel()->GetPosition());
    ASSERT_TRUE(canvas.IsTextInputActive());
    canvas.SetSelectedLayerColor(Colors::PresetGreen);
    verifyLabelStyle();
}
