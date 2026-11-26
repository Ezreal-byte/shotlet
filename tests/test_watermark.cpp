// 水印工具回归测试：
// 1) 水印文字按 UTF-8 字符数截断（中文按 1 个字符计，不产生残缺编码）
// 2) 水印工具不在画布上拖拽绘制图层
// 3) 水印子工具栏 = 颜色菜单 + 默认聚焦的输入框，窄窗口/高 DPI/双语言下不溢出
// 4) 水印文字以 45° 斜向平铺实时渲染到选区（预览与导出一致）
#include "ui/Toolbar.h"
#include "ui/SubToolbar.h"
#include "core/Canvas.h"
#include <gtest/gtest.h>
#include <cstring>
#include <cmath>
#include <vector>
#include <chrono>
#include "utils/ImGuiUtils.h"

using namespace shotlet;

namespace {

void PrepareCanvas(Canvas& canvas) {
    constexpr int width = 320, height = 240, channels = 4;
    auto pixels = std::make_unique<unsigned char[]>(width * height * channels);
    std::memset(pixels.get(), 255, width * height * channels);
    canvas.ReplaceScreenshotAndSelectAll(1, ImVec2(width, height), ImVec2(width, height),
                                         std::move(pixels), width * channels);
}

bool ValidUtf8(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t length = 0;
        if (c < 0x80) length = 1;
        else if ((c & 0xE0) == 0xC0) length = 2;
        else if ((c & 0xF0) == 0xE0) length = 3;
        else if ((c & 0xF8) == 0xF0) length = 4;
        else return false;
        if (i + length > text.size()) return false;
        for (size_t j = 1; j < length; ++j)
            if ((static_cast<unsigned char>(text[i + j]) & 0xC0) != 0x80) return false;
        i += length;
    }
    return true;
}

class WatermarkSubToolbarTest : public testing::Test {
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
        PrepareCanvas(canvas);
        toolbar.SetCanvas(&canvas);
        sub.SetCanvas(&canvas);
        sub.SetToolbar(&toolbar);
    }
    void TearDown() override {
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
    }
    void Frame(float width = 1280, float scale = 1) {
        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(width, 700);
        toolbar.SetUIScale(scale);
        sub.SetUIScale(scale);
        ImGui::NewFrame();
        toolbar.Render();
        sub.Render();
        ImGui::Render();
    }
    void Click(ImVec2 position) {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y); Frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); Frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); Frame();
    }
    Canvas canvas;
    Toolbar toolbar;
    SubToolbar sub;
    ImGuiContext* context = nullptr;
    ImGuiContext* previous = nullptr;
};

} // namespace

TEST(WatermarkTextTest, TruncatesAtTenUtf8CharactersWithoutBreakingEncoding) {
    Canvas canvas;
    // 恰好 10 个中文：原样保留
    canvas.SetWatermarkText("一二三四五六七八九十");
    EXPECT_EQ(canvas.GetWatermarkText(), "一二三四五六七八九十");
    // 12 个中文：截断到 10 个
    canvas.SetWatermarkText("一二三四五六七八九十超字");
    EXPECT_EQ(canvas.GetWatermarkText(), "一二三四五六七八九十");
    // 纯 ASCII 同样按字符数截断
    canvas.SetWatermarkText("1234567890ab");
    EXPECT_EQ(canvas.GetWatermarkText(), "1234567890");
    // 中英文混合：截断点不能落在多字节字符中间
    canvas.SetWatermarkText("abc中文defghijk");
    EXPECT_EQ(canvas.GetWatermarkText(), "abc中文defgh");
    EXPECT_TRUE(ValidUtf8(canvas.GetWatermarkText()));
    // 清空后再次设置
    canvas.SetWatermarkText("");
    EXPECT_TRUE(canvas.GetWatermarkText().empty());
}

TEST(WatermarkOpacityTest, StartsAtThirtyPercentAndClampsToSliderRange) {
    Canvas canvas;
    EXPECT_EQ(canvas.GetWatermarkOpacity(), 30);
    canvas.SetWatermarkOpacity(-1);
    EXPECT_EQ(canvas.GetWatermarkOpacity(), 0);
    canvas.SetWatermarkOpacity(101);
    EXPECT_EQ(canvas.GetWatermarkOpacity(), 100);
}

TEST(WatermarkToolTest, SelectingToolExposesSubToolbarAndDragDoesNotDraw) {
    Canvas canvas;
    PrepareCanvas(canvas);
    canvas.SetTool(ToolType::Watermark);
    EXPECT_EQ(canvas.GetPropertyTool(), ToolType::Watermark);
    // 水印工具暂不支持画布拖拽绘制：拖拽只移动选区，不产生图层。
    canvas.OnMouseDown(ImVec2(30, 30));
    canvas.OnMouseMove(ImVec2(60, 50));
    canvas.OnMouseUp(ImVec2(60, 50));
    EXPECT_TRUE(canvas.GetLayerManager().IsEmpty());
    EXPECT_TRUE(canvas.HasSelection());
    // 再次点击切回关闭
    canvas.SetTool(ToolType::None, false);
    EXPECT_EQ(canvas.GetTool(), ToolType::None);
}

TEST_F(WatermarkSubToolbarTest, InputIsFocusedByDefaultAndFeedsCanvasText) {
    sub.SetLanguage(Language::ChineseSimplified);
    canvas.SetTool(ToolType::Watermark);
    Frame(); Frame();
    auto* window = ImGui::FindWindowByName("SubToolbar");
    ASSERT_NE(window, nullptr);
    // 直接验证输入框持有活动焦点；WantTextInput 有一帧延迟，不适合作为同步焦点断言。
    const ImGuiID inputId = window->GetID("##watermark_input");
    EXPECT_EQ(ImGui::GetActiveID(), inputId);
    // 键盘输入（含超出 10 个字符）写入画布并截断
    ImGui::GetIO().AddInputCharactersUTF8("水印测试文字一二三四五六七");
    Frame();
    EXPECT_EQ(canvas.GetWatermarkText(), "水印测试文字一二三四");
    // 切换工具再切回：输入框恢复焦点并同步画布中的文字
    canvas.SetTool(ToolType::Rectangle);
    Frame(); Frame(); // 隐藏控件的活动 ID 在下一帧清理
    EXPECT_NE(ImGui::GetActiveID(), inputId);
    canvas.SetTool(ToolType::Watermark);
    Frame(); Frame();
    EXPECT_EQ(ImGui::GetActiveID(), inputId);
}

TEST_F(WatermarkSubToolbarTest, ColorSwatchUpdatesWatermarkColor) {
    canvas.SetTool(ToolType::Watermark);
    Frame(); Frame();
    const auto* window = ImGui::FindWindowByName("SubToolbar");
    ASSERT_NE(window, nullptr);
    // 第一个色块（PresetBlue）位于窗口内容区起点，尺寸 20、间距 12（scale=1）。
    const ImVec2 firstColor(window->Pos.x + window->WindowPadding.x + 10,
                            window->Pos.y + window->WindowPadding.y + 10);
    Click(firstColor);
    EXPECT_EQ(canvas.GetToolColor(), Canvas::PRESET_COLORS[0]);
}

TEST_F(WatermarkSubToolbarTest, OpacitySliderUpdatesWatermarkWithoutChangingHighlight) {
    canvas.SetTool(ToolType::Watermark);
    canvas.SetWatermarkText("WM");
    Frame(); Frame();
    EXPECT_EQ(canvas.GetWatermarkOpacity(), 30);
    const auto* window = ImGui::FindWindowByName("SubToolbar");
    ASSERT_NE(window, nullptr);
    // 与高亮同款滑条均位于数值左侧；点击滑轨右侧应提高显示强度，并让文字输入框失焦。
    Click(ImVec2(window->Pos.x + window->Size.x - window->WindowPadding.x - window->Size.y,
                 window->Pos.y + window->Size.y / 2));
    EXPECT_GT(canvas.GetWatermarkOpacity(), 30);
    EXPECT_EQ(canvas.GetHighlightTransparency(), HighlightSettings::DefaultTransparency);
    EXPECT_EQ(canvas.GetWatermarkText(), "WM");
    EXPECT_FALSE(ImGuiUtils::HasActiveTextInput(context));
}

TEST_F(WatermarkSubToolbarTest, LengthLimitUpdatesActiveBufferAndAllowsReplacingSelection) {
    canvas.SetTool(ToolType::Watermark);
    Frame(); Frame();
    auto* window = ImGui::FindWindowByName("SubToolbar");
    ASSERT_NE(window, nullptr);
    const auto inputId = window->GetID("##watermark_input");
    auto& io = ImGui::GetIO();
    io.AddInputCharactersUTF8("123456789012345");
    Frame(); Frame();
    auto* state = ImGui::GetInputTextState(inputId);
    ASSERT_NE(state, nullptr);
    EXPECT_STREQ(state->GetText(), "1234567890");
    EXPECT_EQ(canvas.GetWatermarkText(), state->GetText());
    const auto shortcutModifier = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
    io.AddKeyEvent(shortcutModifier, true);
    io.AddKeyEvent(ImGuiKey_A, true); Frame();
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(shortcutModifier, false);
    io.AddInputCharactersUTF8("一二三四五六七八九十多字"); Frame(); Frame();
    EXPECT_STREQ(state->GetText(), "一二三四五六七八九十");
    EXPECT_EQ(canvas.GetWatermarkText(), state->GetText());
    // 完整选区可替换为更短文字，超限输入不能留下隐藏尾部影响后续编辑。
    io.AddKeyEvent(shortcutModifier, true);
    io.AddKeyEvent(ImGuiKey_A, true); Frame();
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(shortcutModifier, false);
    io.AddInputCharactersUTF8("修改"); Frame();
    EXPECT_STREQ(state->GetText(), "修改");
    EXPECT_EQ(canvas.GetWatermarkText(), "修改");
    io.AddKeyEvent(shortcutModifier, true);
    io.AddKeyEvent(ImGuiKey_Z, true); Frame();
    io.AddKeyEvent(ImGuiKey_Z, false);
    io.AddKeyEvent(shortcutModifier, false); Frame();
    EXPECT_EQ(canvas.GetWatermarkText(), state->GetText());
    EXPECT_LE(ImTextCountCharsFromUtf8(state->GetText(), nullptr), WatermarkSettings::MaxCharacters);
}

TEST_F(WatermarkSubToolbarTest, TurningToolOffAndOnRefocusesAndSynchronizesText) {
    canvas.SetTool(ToolType::Watermark);
    Frame(); Frame();
    canvas.SetTool(ToolType::None, false);
    Frame(); Frame();
    canvas.SetWatermarkText("重新同步");
    canvas.SetTool(ToolType::Watermark);
    Frame(); Frame();
    auto* window = ImGui::FindWindowByName("SubToolbar");
    ASSERT_NE(window, nullptr);
    const auto id = window->GetID("##watermark_input");
    EXPECT_EQ(ImGui::GetActiveID(), id);
    auto* state = ImGui::GetInputTextState(id);
    ASSERT_NE(state, nullptr);
    EXPECT_STREQ(state->GetText(), "重新同步");
    // 查询其他窗口的输入状态后，调用者的 ImGui 上下文必须保留。
    EXPECT_TRUE(ImGuiUtils::HasActiveTextInput(context));
    EXPECT_EQ(ImGui::GetCurrentContext(), context);
}

TEST_F(WatermarkSubToolbarTest, RebindingCanvasSynchronizesWatermarkInput) {
    canvas.SetWatermarkText("旧画布");
    canvas.SetTool(ToolType::Watermark);
    Frame(); Frame();
    ImGui::GetIO().AddInputCharactersUTF8("编辑旧画布");
    Frame();
    Canvas restored;
    PrepareCanvas(restored);
    restored.SetWatermarkText("新画布");
    restored.SetTool(ToolType::Watermark);
    toolbar.SetCanvas(&restored);
    sub.SetCanvas(&restored);
    Frame(); Frame();
    auto* window = ImGui::FindWindowByName("SubToolbar");
    ASSERT_NE(window, nullptr);
    auto* state = ImGui::GetInputTextState(window->GetID("##watermark_input"));
    ASSERT_NE(state, nullptr);
    EXPECT_STREQ(state->GetText(), "新画布");
    toolbar.SetCanvas(&canvas);
    sub.SetCanvas(&canvas);
}

TEST_F(WatermarkSubToolbarTest, FitsThemesDpiLanguagesAndNarrowWindows) {
    canvas.SetTool(ToolType::Watermark);
    for (auto language : {Language::English, Language::ChineseSimplified}) {
        toolbar.SetLanguage(language);
        sub.SetLanguage(language);
        for (auto theme : {Theme::Dark, Theme::Light}) {
            toolbar.SetTheme(theme);
            sub.SetTheme(theme);
            for (float scale : {1.0f, 1.5f, 2.0f}) {
                for (float width : {380.0f, 640.0f, 1280.0f}) {
                    SCOPED_TRACE(testing::Message() << "width=" << width << ", scale=" << scale);
                    Frame(width, scale);
                    Frame(width, scale);
                    const auto* window = ImGui::FindWindowByName("SubToolbar");
                    ASSERT_NE(window, nullptr);
                    EXPECT_TRUE(toolbar.HasSelectedToolPos());
                    EXPECT_LE(window->DC.CursorMaxPos.x, sub.GetWindowRect().Max.x + 1);
                    EXPECT_LE(window->DC.CursorMaxPos.y, sub.GetWindowRect().Max.y + 1);
                    EXPECT_LE(sub.GetWindowRect().Max.x, width + 1);
                }
            }
        }
    }
}

TEST_F(WatermarkSubToolbarTest, ToolbarWatermarkButtonTogglesTool) {
    // 先程序化选中拿到图标实际位置，再取消选中后用点击验证映射。
    canvas.SetTool(ToolType::Watermark);
    Frame(); Frame();
    ASSERT_TRUE(toolbar.HasSelectedToolPos());
    const ImVec2 pos = toolbar.GetSelectedToolPos();
    const float size = toolbar.GetSelectedIconSize();
    const ImVec2 center(pos.x + size / 2, pos.y + size / 2);
    Click(center);
    EXPECT_EQ(canvas.GetTool(), ToolType::None);
    Click(center);
    EXPECT_EQ(canvas.GetTool(), ToolType::Watermark);
}

// ─── 水印渲染测试（文字实时平铺到选区） ───

namespace {

// 采集 drawList 中水印色顶点（文字字形 quad，4 顶点/字符）。
std::vector<const ImDrawVert*> WatermarkVertices(const ImDrawList& list, const Canvas& canvas) {
    constexpr ImU32 maximumAlpha = IM_COL32_A_MASK >> IM_COL32_A_SHIFT;
    const ImU32 alpha = static_cast<ImU32>(canvas.GetWatermarkOpacity()) * maximumAlpha / WatermarkSettings::MaximumOpacity;
    const ImU32 expected = (canvas.GetToolColor() & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
    std::vector<const ImDrawVert*> vertices;
    for (const auto& vertex : list.VtxBuffer) {
        if (vertex.col == expected) vertices.push_back(&vertex);
    }
    return vertices;
}

class WatermarkRenderTest : public testing::Test {
protected:
    void SetUp() override {
        previous = ImGui::GetCurrentContext();
        context = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(640, 480);
        io.DeltaTime = 1.0f / 60;
        io.FontDefault = io.Fonts->AddFontDefault();
        io.Fonts->Build();
        ImGui::NewFrame();
        PrepareCanvas(canvas); // 320x240 全选
    }
    void TearDown() override {
        ImGui::EndFrame();
        ImGui::SetCurrentContext(previous);
        ImGui::DestroyContext(context);
    }
    Canvas canvas;
    ImGuiContext* context = nullptr;
    ImGuiContext* previous = nullptr;
};

// 就地新建独立 drawList（ImDrawList 不可拷贝，无法封装为返回值）。
#define NEW_DRAW_LIST(name) \
    ImDrawList name(ImGui::GetDrawListSharedData()); \
    name._ResetForNewFrame(); \
    name.PushClipRectFullScreen()

} // namespace

// 空文字时导出渲染不产生任何水印顶点。
TEST_F(WatermarkRenderTest, CleanRenderSkipsWatermarkWhenTextEmpty) {
    canvas.SetWatermarkText("");
    NEW_DRAW_LIST(clean);
    canvas.RenderClean(&clean, ImVec2(0, 0));
    EXPECT_TRUE(WatermarkVertices(clean, canvas).empty());
}

// 非空文字时导出渲染平铺多份水印，全部顶点落在选区内且被选区裁剪命令约束。
TEST_F(WatermarkRenderTest, CleanRenderTilesWatermarkInsideSelection) {
    canvas.SetWatermarkText("WM");
    NEW_DRAW_LIST(clean);
    canvas.RenderClean(&clean, ImVec2(0, 0));
    const auto vertices = WatermarkVertices(clean, canvas);
    // 平铺至少两份 "WM"（4 字符 quad = 16 顶点）才称得上平铺
    ASSERT_GE(vertices.size(), 16u);
    for (const auto* vertex : vertices) {
        // 平铺网格覆盖"选区旋转后投影"，选区外的字形顶点仍在 VtxBuffer 中，
        // 但被裁剪命令剔除（下方 ClipRect 断言）；这里只验证平铺围绕选区中心、
        // 未发散到选区对角线半径之外（320x240 对角线 400）
        const float dx = vertex->pos.x - 160.0f;
        const float dy = vertex->pos.y - 120.0f;
        EXPECT_LE(dx * dx + dy * dy, 460.0f * 460.0f);
    }
    // 水印命令的裁剪矩形收紧在选区附近（±2px 取整容差），溢出选区的字形被裁掉
    for (const auto& command : clean.CmdBuffer) {
        if (command.ElemCount == 0 || command.ClipRect.z - command.ClipRect.x <= 0) continue;
        EXPECT_LE(command.ClipRect.x, 2.0f);
        EXPECT_LE(command.ClipRect.y, 2.0f);
        EXPECT_GE(command.ClipRect.z, 318.0f);
        EXPECT_GE(command.ClipRect.w, 238.0f);
    }
}

// 45° 旋转生效：字形 quad 的上边不再水平（相邻顶点 y 出现明显落差）。
TEST_F(WatermarkRenderTest, TiledTextIsRotatedDiagonally) {
    canvas.SetWatermarkText("WM");
    NEW_DRAW_LIST(clean);
    canvas.RenderClean(&clean, ImVec2(0, 0));
    const auto vertices = WatermarkVertices(clean, canvas);
    ASSERT_GE(vertices.size(), 8u);
    // 未旋转时每个 quad 的前两个顶点（上边）y 完全相同；旋转后必有一处明显错开
    bool diagonalFound = false;
    for (size_t i = 0; i + 1 < vertices.size(); ++i) {
        if (std::abs(vertices[i]->pos.y - vertices[i + 1]->pos.y) > 1.0f) {
            diagonalFound = true;
            break;
        }
    }
    EXPECT_TRUE(diagonalFound);
}

// 预览渲染（Canvas::Render）直接读当前文字：修改立即反映，清空立即消失。
TEST_F(WatermarkRenderTest, PreviewRenderReflectsTextChangesImmediately) {
    canvas.SetWatermarkText("WM");
    NEW_DRAW_LIST(first);
    canvas.Render(&first);
    const size_t shortCount = WatermarkVertices(first, canvas).size();
    EXPECT_GE(shortCount, 8u);

    canvas.SetWatermarkText("WATERMARK");
    NEW_DRAW_LIST(second);
    canvas.Render(&second);
    const size_t longCount = WatermarkVertices(second, canvas).size();
    EXPECT_GT(longCount, shortCount);

    canvas.SetWatermarkText("");
    NEW_DRAW_LIST(third);
    canvas.Render(&third);
    EXPECT_TRUE(WatermarkVertices(third, canvas).empty());
}

// 不同显示强度只改变水印 alpha，RGB 始终取当前工具颜色。
TEST_F(WatermarkRenderTest, OpacityControlsAlphaAndZeroSkipsWatermarkGeometry) {
    canvas.SetWatermarkText("WM");
    canvas.SetSelectedLayerColor(Colors::PresetGreen);
    for (const auto [percent, alpha] : {std::pair{0, 0}, {30, 76}, {38, 96}, {65, 165}, {100, 255}}) {
        SCOPED_TRACE(percent);
        canvas.SetWatermarkOpacity(percent);
        NEW_DRAW_LIST(clean);
        canvas.RenderClean(&clean, ImVec2(0, 0));
        const auto vertices = WatermarkVertices(clean, canvas);
        if (percent == 0) { EXPECT_TRUE(vertices.empty()); continue; }
        ASSERT_FALSE(vertices.empty());
        const ImU32 expected = (Colors::PresetGreen & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
        for (const auto* vertex : vertices) EXPECT_EQ(vertex->col, expected);
    }
}

TEST_F(WatermarkRenderTest, DocumentRoundTripAndCropPreserveWatermarkLayout) {
    canvas.SetWatermarkText("WM");
    canvas.SetWatermarkOpacity(65);
    canvas.SetSelectedLayerColor(Colors::PresetGreen);
    std::string error;
    auto document = canvas.ExportDocument(error);
    ASSERT_TRUE(document) << error;
    ASSERT_TRUE(document->watermark);
    Canvas restored;
    restored.SetUIScale(2);
    ASSERT_TRUE(restored.RestoreDocument(*document, 1, error)) << error;
    EXPECT_EQ(restored.GetWatermarkText(), "WM");
    EXPECT_EQ(restored.GetWatermarkOpacity(), 65);
    EXPECT_EQ(restored.GetToolColor(), Colors::PresetGreen);
    NEW_DRAW_LIST(original);
    NEW_DRAW_LIST(roundTrip);
    canvas.RenderClean(&original, ImVec2(0, 0));
    restored.RenderClean(&roundTrip, ImVec2(0, 0));
    const auto before = WatermarkVertices(original, canvas);
    const auto after = WatermarkVertices(roundTrip, restored);
    ASSERT_EQ(before.size(), after.size());
    for (size_t i = 0; i < before.size(); ++i) {
        EXPECT_FLOAT_EQ(before[i]->pos.x, after[i]->pos.x);
        EXPECT_FLOAT_EQ(before[i]->pos.y, after[i]->pos.y);
    }
    const ImRect crop(ImVec2(40, 30), ImVec2(220, 190));
    canvas.SetImageEditing(true);
    auto cropped = canvas.ExportCroppedDocument(crop, error);
    ASSERT_TRUE(cropped) << error;
    ASSERT_TRUE(cropped->watermark);
    EXPECT_EQ(cropped->watermark->opacity, 65);
    EXPECT_FLOAT_EQ(cropped->watermark->origin.x, -crop.Min.x);
    EXPECT_FLOAT_EQ(cropped->watermark->origin.y, -crop.Min.y);
    ASSERT_TRUE(restored.RestoreDocument(*cropped, 1, error)) << error;
    EXPECT_EQ(restored.GetWatermarkText(), "WM");
    document->watermark.reset();
    ASSERT_TRUE(restored.RestoreDocument(*document, 1, error)) << error;
    EXPECT_TRUE(restored.GetWatermarkText().empty());
    EXPECT_EQ(restored.GetWatermarkOpacity(), 30);
}

TEST_F(WatermarkRenderTest, LongImageUsesVisibleRowsInsteadOfDiagonalSquare) {
    canvas.SetWatermarkText("WM");
    constexpr int longSide = 16384;
    constexpr int maxNarrowImageVertices = 100000;
    for (const auto size : {ImVec2(40, longSide), ImVec2(longSide, 40), ImVec2(320, longSide)}) {
        SCOPED_TRACE(testing::Message() << size.x << "x" << size.y);
        canvas.ReplaceScreenshotAndSelectAll(1, size, size, nullptr, static_cast<int>(size.x) * 4);
        NEW_DRAW_LIST(clean);
        clean.Flags |= ImDrawListFlags_AllowVtxOffset;
        const auto start = std::chrono::steady_clock::now();
        canvas.RenderClean(&clean, ImVec2(0, 0));
        RecordProperty("render_microseconds", std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count()));
        EXPECT_GT(clean.VtxBuffer.Size, 0);
        EXPECT_LT(clean.VtxBuffer.Size, maxNarrowImageVertices);
        // 放大长图时屏外部分不应持续生成水印几何；视口改变不移动网格原点。
        NEW_DRAW_LIST(viewport);
        viewport.PushClipRect(ImVec2(0, 0), ImVec2(320, 240), false);
        canvas.RenderClean(&viewport, ImVec2(0, 0));
        EXPECT_LT(viewport.VtxBuffer.Size, 4000);
        viewport.PopClipRect();
    }
}
