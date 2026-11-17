#include "utils/Colors.h"
#include "../Layer.h"
#include "utils/GeometryUtils.h"
#include <algorithm>
#include <limits>

namespace shotlet {
namespace NumberLabelLayout {
    constexpr float GapLogical = 24;
    constexpr float MinimumWrapWidth = 1;
}

NumberLayer::NumberLayer(ImVec2 center, int number, ImU32 color, float diameter)
    : Layer(color, 0.0f), m_center(center), m_number(number), m_diameter(diameter) {}

void NumberLayer::Render(ImDrawList* drawList, const ImVec2& canvasOffset, float) {
    if (!drawList) return;
    RenderLabel(drawList, canvasOffset);
    const ImVec2 center(m_center.x + canvasOffset.x, m_center.y + canvasOffset.y);
    drawList->AddCircleFilled(center, m_diameter * 0.5f, m_color);
    ImFont* font = ImGui::GetFont();
    if (!font) return;
    const std::string text = std::to_string(m_number);
    // 用字形墨迹边界居中，避免字体基线导致数字偏上或偏下。
    ImVec2 inkMin(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    ImVec2 inkMax(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
    float fontSize = m_diameter * 0.66f;
    // 使用本项目 ImGui 的按字号烘焙接口，并以最终字号重新测量墨迹。
    for (int attempt = 0; attempt < 4; ++attempt) {
        ImFontBaked* baked = font->GetFontBaked(fontSize);
        const float scale = fontSize / baked->Size;
        inkMin = ImVec2(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
        inkMax = ImVec2(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
        float advance = 0.0f;
        for (const unsigned char digit : text) {
            const ImFontGlyph* glyph = baked->FindGlyph(digit);
            if (!glyph) continue;
            inkMin.x = std::min(inkMin.x, (advance + glyph->X0) * scale);
            inkMin.y = std::min(inkMin.y, glyph->Y0 * scale);
            inkMax.x = std::max(inkMax.x, (advance + glyph->X1) * scale);
            inkMax.y = std::max(inkMax.y, glyph->Y1 * scale);
            advance += glyph->AdvanceX;
        }
        const float extent = std::max(inkMax.x - inkMin.x, inkMax.y - inkMin.y);
        if (extent <= 0.0f) return;
        if (extent <= m_diameter * 0.68f || attempt == 3) break;
        // 内接正方形留出边距，多位数字缩小字号，圆大小保持不变。
        fontSize *= m_diameter * 0.66f / extent;
    }
    const ImVec2 position(center.x - (inkMin.x + inkMax.x) * 0.5f,
                          center.y - (inkMin.y + inkMax.y) * 0.5f);
    // 预设白色为 RGB(254,254,254)，同时兼容纯白；其余背景固定使用白色数字。
    constexpr ImU32 rgbMask = Colors::White & ~IM_COL32_A_MASK;
    const ImU32 rgb = m_color & rgbMask;
    const bool whiteBackground = rgb == (Colors::PresetWhite & rgbMask) || rgb == rgbMask;
    const ImU32 textColor = whiteBackground ? Colors::Black : Colors::White;
    drawList->AddText(font, fontSize, position, textColor, text.c_str());
}

void NumberLayer::RenderLabel(ImDrawList* drawList, const ImVec2& canvasOffset) {
    // 空说明框不进入导出结果，选中时由 RenderSelectionUI 临时显示。
    if (!m_label || (!m_labelEditing && !HasLabelText())) return;
    // 编辑时文字、边框和光标由 Canvas 共用文本工具的输入 UI 绘制。
    RenderLabelFrame(drawList, canvasOffset, !m_labelEditing);
    if (m_labelEditing) return;
    m_label->SetColor(m_color);
    m_label->Render(drawList, canvasOffset, m_labelFrameScale);
}

void NumberLayer::RenderLabelFrame(ImDrawList* drawList, const ImVec2& canvasOffset, bool drawFrame) const {
    const float thickness = m_label->GetThickness() * m_labelFrameScale;
    const auto [start, end] = GetConnector();
    if (start.x != end.x || start.y != end.y) {
        drawList->AddLine(ImVec2(start.x + canvasOffset.x, start.y + canvasOffset.y),
            ImVec2(end.x + canvasOffset.x, end.y + canvasOffset.y), m_color, thickness);
    }
    if (!drawFrame) return;
    const auto frame = GetLabelFrameBounds();
    drawList->AddRect(ImVec2(frame.Min.x + canvasOffset.x, frame.Min.y + canvasOffset.y),
        ImVec2(frame.Max.x + canvasOffset.x, frame.Max.y + canvasOffset.y), m_color, 0, 0, thickness);
}

bool NumberLayer::IsLabelVisible() const {
    return m_label && (m_labelEditing || IsSelected() || HasLabelText());
}

void NumberLayer::RenderToImage(unsigned char*, int, int, int, const ImVec2&, float) {
    // 与文字图层一致，圆和数字统一由 RenderClean 的 GPU 离屏路径输出。
}

ImRect NumberLayer::GetCircleBounds() const {
    const float radius = m_diameter * 0.5f;
    return ImRect(m_center.x - radius, m_center.y - radius, m_center.x + radius, m_center.y + radius);
}

bool NumberLayer::ContainsCircle(ImVec2 point) const {
    const float dx = point.x - m_center.x, dy = point.y - m_center.y;
    return dx * dx + dy * dy <= m_diameter * m_diameter * 0.25f;
}

ImRect NumberLayer::GetBounds() const {
    auto bounds = GetCircleBounds();
    // 隐藏的空说明框不参与内容边界，避免限制选区缩放。
    if (m_label && (m_labelEditing || HasLabelText())) bounds.Add(GetLabelFrameBounds());
    return bounds;
}

ImRect NumberLayer::GetDragBounds() const {
    return m_dragLabel ? GetLabelFrameBounds() : GetCircleBounds();
}

ImRect NumberLayer::GetLabelFrameBounds() const {
    if (!m_label) return ImRect(m_center, m_center);
    const auto position = m_label->GetPosition();
    const float pad = TextLayer::FramePadding * m_labelFrameScale;
    // 和文本工具一样，空框宽度仅包含左右内边距，输入后随文字内容自适应。
    // 没有字体上下文的纯几何调用仍保留一行高度。
    ImVec2 textSize(0, m_label->GetFontSize());
    if (!m_label->GetText().empty() && ImGui::GetCurrentContext()) {
        const auto bounds = m_label->GetBounds();
        textSize.x = bounds.GetWidth();
        textSize.y = std::max(textSize.y, bounds.GetHeight());
    }
    return ImRect(ImVec2(position.x - 2 * pad, position.y - pad),
        ImVec2(position.x + textSize.x + 2 * pad, position.y + textSize.y + pad));
}

void NumberLayer::SetLabel(ImVec2 position, const std::string& text, float fontSize, float wrapWidth, float frameScale) {
    m_label = std::make_unique<TextLayer>(position, text, m_color, fontSize, wrapWidth);
    m_labelFrameScale = frameScale;
}

void NumberLayer::InitializeLabel(const ImRect& selection, float fontSize, float frameScale) {
    // 极窄的历史截图在高 DPI 窗口中新建说明时，边距也必须留出至少一个坐标单位的文字宽度。
    frameScale = std::min(frameScale, (selection.GetWidth() - NumberLabelLayout::MinimumWrapWidth) /
        (4 * TextLayer::FramePadding));
    const float pad = TextLayer::FramePadding * frameScale;
    const float height = fontSize + 2 * pad;
    const float textX = std::clamp(m_center.x, selection.Min.x + 2 * pad,
        selection.Max.x - 2 * pad - NumberLabelLayout::MinimumWrapWidth);
    const float top = std::clamp(m_center.y - m_diameter / 2 - NumberLabelLayout::GapLogical * frameScale - height,
        selection.Min.y, std::max(selection.Min.y, selection.Max.y - height));
    const float wrapWidth = std::max(NumberLabelLayout::MinimumWrapWidth, selection.Max.x - textX - 2 * pad);
    SetLabel(ImVec2(textX, top + pad), "", fontSize, wrapWidth, frameScale);
}

std::pair<ImVec2, ImVec2> NumberLayer::GetConnector() const {
    return GeometryUtils::ConnectRectangleToCircle(GetLabelFrameBounds(), m_center, m_diameter / 2);
}

std::unique_ptr<Layer> NumberLayer::Clone() const {
    auto clone = std::make_unique<NumberLayer>(m_center, m_number, m_color, m_diameter);
    if (m_label) clone->SetLabel(m_label->GetPosition(), m_label->GetText(), m_label->GetFontSize(),
                                m_label->GetWrapWidth(), m_labelFrameScale);
    return clone;
}

void NumberLayer::StartDrag(const ImVec2& mousePos) {
    m_dragging = true;
    m_centerBeforeDrag = m_center;
    m_dragLabel = IsLabelVisible() && !ContainsCircle(mousePos) && GetLabelFrameBounds().Contains(mousePos);
    if (m_label) m_labelBeforeDrag = m_label->GetPosition();
}

void NumberLayer::UpdateDrag(const ImVec2& delta) {
    // 只更新当前命中的部分，圆点拖动不能带着说明框移动。
    if (m_dragLabel) m_label->SetPosition(ImVec2(m_labelBeforeDrag.x + delta.x, m_labelBeforeDrag.y + delta.y));
    else m_center = ImVec2(m_centerBeforeDrag.x + delta.x, m_centerBeforeDrag.y + delta.y);
}

bool NumberLayer::IsPointNearBorder(const ImVec2& pos, float threshold) const {
    if (ContainsCircle(pos)) return true;
    if (!IsLabelVisible()) return false;
    if (GetLabelFrameBounds().Contains(pos)) return true;
    const auto [start, end] = GetConnector();
    // 两个形状重叠时没有外部线段，避免将零长度线段交给投影计算导致除零。
    if (start.x == end.x && start.y == end.y) return false;
    const auto nearest = ImLineClosestPoint(start, end, pos);
    return std::hypot(pos.x - nearest.x, pos.y - nearest.y) <= threshold;
}

void NumberLayer::RenderSelectionUI(ImDrawList* drawList, float uiScale) const {
    const ImRect bounds = GetCircleBounds();
    drawList->AddRect(bounds.Min, bounds.Max, Colors::TextSelectedBorder, 0.0f, 0, uiScale);
    // 有文字的说明框已有与圆点同色的常驻边框；空说明框仅在选中时显示，方便双击补充文字。
    if (m_label && !m_labelEditing && !HasLabelText()) RenderLabelFrame(drawList, ImVec2(0, 0));
}

} // namespace shotlet
