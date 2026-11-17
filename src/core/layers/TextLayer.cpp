#include "../Layer.h"
#include "utils/Constants.h"
#include <cfloat>

namespace shotlet {

int CountEmptyLines(const std::string& text) {
    int emptyLineCount = 0;
    if (text.empty()) return 0;
    // 首行以 \n 开头，第一行是空行
    if (text[0] == '\n') emptyLineCount++;
    // 每个 \n 后如果紧跟 \n 或文本末尾，该 \n 产生的行是空行
    for (int i = 0; i < static_cast<int>(text.size()); i++) {
        if (text[i] == '\n') {
            char next = (i + 1 < static_cast<int>(text.size())) ? text[i + 1] : '\0';
            if (next == '\n' || next == '\0') emptyLineCount++;
        }
    }
    return emptyLineCount;
}


// ========== TextLayer ==========
TextLayer::TextLayer(ImVec2 position, const std::string& text, ImU32 color, float fontSize, float wrapWidth)
    : Layer(color, 1.0f), m_position(position), m_text(text), m_fontSize(fontSize), m_wrapWidth(wrapWidth) {}

ImRect TextLayer::GetBounds() const {
    // 使用 ImGui 计算文本边界
    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = io.FontDefault;
    if (!font) {
        return ImRect(m_position, ImVec2(m_position.x + 100, m_position.y + 20));
    }

    ImVec2 textSize = font->CalcTextSizeA(m_fontSize, FLT_MAX, m_wrapWidth, m_text.c_str(), nullptr);
    float singleLineHeight = font->CalcTextSizeA(m_fontSize, FLT_MAX, 0.0f, " ", nullptr, nullptr).y;
    textSize.y += CountEmptyLines(m_text) * singleLineHeight;

    return ImRect(m_position, ImVec2(m_position.x + textSize.x, m_position.y + textSize.y));
}

ImRect TextLayer::GetFrameBounds(const float uiScale) const {
    const float scale = ImMax(uiScale, 1.0f);
    ImRect bounds = GetBounds();
    return ImRect(
        ImVec2(bounds.Min.x - FramePadding * 2.0f * scale, bounds.Min.y - FramePadding * scale),
        ImVec2(bounds.Max.x + FramePadding * 2.0f * scale, bounds.Max.y + FramePadding * scale));
}

void TextLayer::Render(ImDrawList* drawList, const ImVec2& canvasOffset, float uiScale) {
    if (m_text.empty()) return;
    if (!drawList) return;

    ImVec2 pos(m_position.x + canvasOffset.x, m_position.y + canvasOffset.y);

    // 绘制透明背景（高度随文本行数自适应）
    ImRect frameBounds = GetFrameBounds(uiScale);
    ImVec2 bgMin(frameBounds.Min.x + canvasOffset.x, frameBounds.Min.y + canvasOffset.y);
    ImVec2 bgMax(frameBounds.Max.x + canvasOffset.x, frameBounds.Max.y + canvasOffset.y);
    drawList->AddRectFilled(bgMin, bgMax, Colors::TextTransparentBg);

    // 绘制文字
    // pos 是 m_position（点击位置），ImGui AddText 的 pos.y 是文字顶部，直接用 pos.y
    ImVec2 textPos(pos.x, pos.y);
    ImFont* font = ImGui::GetIO().FontDefault;
    if (font) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        drawList->AddText(font, m_fontSize, textPos, m_color, m_text.c_str(), nullptr, m_wrapWidth);
        ImGui::PopStyleVar();
    }
}

void TextLayer::RenderToImage(unsigned char* imageData, int imgWidth, int imgHeight,
                               int rowPitch, const ImVec2& canvasOffset, float scaleFactor) {
    // 文本渲染到像素数据需要字体光栅化，暂不支持
    // 文本应该已经通过 ImGui 渲染到截图纹理中
    // 注意：scaleFactor 参数目前未使用，文本导出需要更复杂的实现
    (void)imageData;
    (void)imgWidth;
    (void)imgHeight;
    (void)rowPitch;
    (void)canvasOffset;
    (void)scaleFactor;
}

std::unique_ptr<Layer> TextLayer::Clone() const {
    return std::make_unique<TextLayer>(m_position, m_text, m_color, m_fontSize, m_wrapWidth);
}

void TextLayer::SetText(const std::string& text) {
    m_text = text;
}

// ========== TextLayer 拖拽/选中 UI ==========

void TextLayer::StartDrag(const ImVec2& mousePos) {
    m_dragging = true;
    m_dragStartMouse = mousePos;
    m_posBeforeDrag = m_position;
}

void TextLayer::UpdateDrag(const ImVec2& delta) {
    ImVec2 newPos(m_posBeforeDrag.x + delta.x, m_posBeforeDrag.y + delta.y);
    m_position = newPos;
}

void TextLayer::EndDrag() {
    m_dragging = false;
}

void TextLayer::RenderSelectionUI(ImDrawList* drawList, float uiScale) const {
    ImRect bounds = GetFrameBounds(uiScale);
    drawList->AddRect(bounds.Min, bounds.Max, Colors::TextSelectedBorder, 0.0f, 0, 2.0f * uiScale);
}

bool TextLayer::IsPointNearBorder(const ImVec2& pos, float threshold) const {
    (void)threshold;
    return GetFrameBounds(1.0f).Contains(pos);
}

} // namespace shotlet
