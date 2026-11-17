#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include "Colors.h"
#include "StringUtils.h"

namespace shotlet::ImGuiUtils {

// 多窗口共用线程时，临时切换上下文必须在作用域结束后恢复。
class ContextScope {
public:
    explicit ContextScope(ImGuiContext* context) : m_previous(ImGui::GetCurrentContext()) {
        ImGui::SetCurrentContext(context);
    }
    ~ContextScope() { ImGui::SetCurrentContext(m_previous); }
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
private:
    ImGuiContext* m_previous;
};

inline bool HasActiveTextInput(ImGuiContext* context) {
    if (!context) return false;
    const ContextScope scope(context);
    return ImGui::GetActiveID() != 0 && ImGui::GetInputTextState(ImGui::GetActiveID()) != nullptr;
}

inline void ResetTextInputSession(ImGuiID id) {
    auto& context = *ImGui::GetCurrentContext();
    if (ImGui::GetActiveID() == id) ImGui::ClearActiveID();
    // 数据源更换后，活动缓冲和失焦待回写缓冲都属于旧对象，不能应用到新对象。
    // 仅丢弃对应控件的会话标识；存储仍由 ImGui 持有，下次聚焦会正常重建撤销和初值。
    if (context.InputTextState.ID == id) context.InputTextState.ID = 0;
    if (context.InputTextDeactivatedState.ID == id) context.InputTextDeactivatedState.ClearFreeMemory();
}

inline int LimitUtf8InputCharacters(ImGuiInputTextCallbackData* data) {
    const auto limit = *static_cast<const size_t*>(data->UserData);
    const auto bytes = StringUtils::Utf8PrefixBytes(std::string_view(data->Buf, data->BufTextLen), limit);
    if (bytes < static_cast<size_t>(data->BufTextLen)) {
        // 在 ImGui 编辑回调中裁剪，同步内部文字、光标、选区与撤销记录。
        data->DeleteChars(static_cast<int>(bytes), data->BufTextLen - static_cast<int>(bytes));
    }
    return 0;
}

inline bool InputTextWithCharacterLimit(const char* label, const char* hint, char* buffer,
                                         size_t bufferSize, size_t maxCharacters) {
    return ImGui::InputTextWithHint(label, hint, buffer, bufferSize, ImGuiInputTextFlags_CallbackEdit,
                                    LimitUtf8InputCharacters, &maxCharacters);
}

inline void VerticalSpace(float height) {
    ImGui::Dummy(ImVec2(0, height));
}

inline void SeparatorWithoutSpacing() {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));
    ImGui::Separator();
    ImGui::PopStyleVar();
}

// 保留整数滑块的鼠标、键盘和临时输入行为，只将默认长条抓手替换为圆形，
// 并把滑轨替换为居中的细胶囊，圆形滑块覆盖在细滑轨之上。
inline bool SliderIntWithCircularGrab(const char* label, int* value, int minimum, int maximum) {
    if (ImGui::GetCurrentWindow()->SkipItems) return false;
    // 与项目内 ImGui::SliderBehavior 的抓手内边距保持一致，单位为 ImGui 逻辑坐标。
    constexpr float grabPadding = 2.0f;
    constexpr float minimumGrabSize = 1.0f;
    constexpr float trackHeight = 4.0f;
    const auto position = ImGui::GetCursorScreenPos();
    const float width = ImGui::CalcItemWidth(), height = ImGui::GetFrameHeight();
    const float diameter = ImMax(minimumGrabSize, height - 2 * grabPadding);
    const ImU32 trackColor = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 normalColor = ImGui::GetColorU32(ImGuiCol_SliderGrab);
    const ImU32 activeColor = ImGui::GetColorU32(ImGuiCol_SliderGrabActive);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, diameter);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, Colors::Transparent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, Colors::Transparent);
    // 悬停/按下时 ImGui 会用 FrameBgHovered/FrameBgActive 绘制整块背景，一并置为透明。
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Colors::Transparent);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Colors::Transparent);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Colors::Transparent);
    const bool changed = ImGui::SliderInt(label, value, minimum, maximum, "", ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar();
    if (ImGui::IsItemVisible() && !ImGui::TempInputIsActive(ImGui::GetItemID())) {
        const float range = static_cast<float>(static_cast<double>(maximum) - minimum);
        const float extent = ImMax(minimumGrabSize, width - 2 * grabPadding);
        const float grabWidth = ImMin(extent, ImMax(diameter, extent / (ImAbs(range) + 1)));
        const float ratio = range == 0 ? 0 : ImClamp((static_cast<float>(*value) - static_cast<float>(minimum)) / range, 0.0f, 1.0f);
        const ImVec2 center(position.x + grabPadding + grabWidth / 2 + (extent - grabWidth) * ratio,
                            position.y + height / 2);
        auto* drawList = ImGui::GetWindowDrawList();
        // 先画细滑轨，再画圆形滑块，保证滑块覆盖在滑轨之上。
        drawList->AddRectFilled(
            ImVec2(position.x + grabPadding, center.y - trackHeight / 2),
            ImVec2(position.x + grabPadding + extent, center.y + trackHeight / 2),
            trackColor, trackHeight / 2);
        drawList->AddCircleFilled(center, diameter / 2, ImGui::IsItemActive() ? activeColor : normalColor);
    }
    return changed;
}

// 根据分组实际边界添加上下留白，避免默认 ItemSpacing 与行留白重复叠加。
class PaddedRow {
public:
    explicit PaddedRow(float verticalPadding, bool separator = true)
        : m_padding(verticalPadding), m_separator(separator) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + m_padding);
        ImGui::BeginGroup();
    }
    ~PaddedRow() {
        ImGui::EndGroup();
        auto position = ImGui::GetCursorScreenPos();
        position.y = ImGui::GetItemRectMax().y;
        ImGui::SetCursorScreenPos(position);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));
        ImGui::Dummy(ImVec2(0, m_padding));
        if (m_separator) ImGui::Separator();
        ImGui::PopStyleVar();
    }
    PaddedRow(const PaddedRow&) = delete;
    PaddedRow& operator=(const PaddedRow&) = delete;
private:
    float m_padding;
    bool m_separator;
};

inline void MutedWrappedText(const char* text) {
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

inline bool PrimaryButton(const char *label, ImVec2 size, ImU32 color) {
    const auto base = ImGui::ColorConvertU32ToFloat4(color);
    constexpr float hoverColorOffset = Colors::PrimaryButtonHoverOffset;
    const ImVec4 hovered(base.x + hoverColorOffset, base.y + hoverColorOffset, base.z + hoverColorOffset, base.w);
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::White);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

// 按钮和菜单共用危险操作配色；作用域结束时恢复，不能影响相邻的普通操作。
class DestructiveStyle {
public:
    explicit DestructiveStyle(const ThemePalette &palette) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.danger);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.dangerHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, palette.dangerActive);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, palette.dangerHovered);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, palette.dangerActive);
    }
    ~DestructiveStyle() { ImGui::PopStyleColor(5); }
    DestructiveStyle(const DestructiveStyle &) = delete;
    DestructiveStyle &operator=(const DestructiveStyle &) = delete;
};

inline bool DestructiveButton(const char *label, const ThemePalette &palette, ImVec2 size = {}) {
    const DestructiveStyle style(palette);
    return ImGui::Button(label, size);
}

inline bool DestructiveMenuItem(const char *label, const ThemePalette &palette, const char *shortcut = nullptr) {
    const DestructiveStyle style(palette);
    return ImGui::MenuItem(label, shortcut);
}

inline void DrawClippedText(ImDrawList *draw, ImVec2 position, ImVec2 maximum, ImU32 color, const char *text) {
    draw->PushClipRect(position, maximum, true);
    draw->AddText(position, color, text);
    draw->PopClipRect();
}

inline void DrawImageContained(ImDrawList* draw, ImTextureID texture, ImVec2 pixelSize,
                                const ImRect& bounds, ImU32 border) {
    draw->AddRect(bounds.Min, bounds.Max, border);
    if (!texture || pixelSize.x <= 0 || pixelSize.y <= 0) return;
    const float fit = ImMin(bounds.GetWidth() / pixelSize.x, bounds.GetHeight() / pixelSize.y);
    const ImVec2 extent(pixelSize.x * fit, pixelSize.y * fit);
    const ImVec2 position(bounds.GetCenter().x - extent.x * .5f, bounds.GetCenter().y - extent.y * .5f);
    draw->AddImage(texture, position, ImVec2(position.x + extent.x, position.y + extent.y));
}

namespace SearchInputLayout {
    constexpr float IconSize = 16;
    constexpr float TextGap = 7;
    constexpr float LensRadius = 5;
    constexpr float LensCenter = 6;
    constexpr float HandleEnd = 14;
    constexpr float DiagonalUnit = 0.70710678f;
    constexpr float StrokeWidth = 1.5f;
} // namespace SearchInputLayout

inline bool InputTextWithSearchIcon(const char *label, const char *hint, char *buffer, size_t bufferSize, float scale) {
    if (ImGui::GetCurrentWindow()->SkipItems)
        return false;

    const auto padding = ImGui::GetStyle().FramePadding;
    const auto position = ImGui::GetCursorScreenPos();
    const float textInset = (SearchInputLayout::IconSize + SearchInputLayout::TextGap) * scale;
    const float textLeft = position.x + padding.x + textInset;
    auto *draw = ImGui::GetWindowDrawList();
    const auto clipMin = draw->GetClipRectMin();
    const auto clipMax = draw->GetClipRectMax();

    // 仅裁剪绘制区域，原生输入框仍处理整块区域的点击和输入法定位；长文本不会滚到图标下方。
    draw->PushClipRect(ImVec2(textLeft, clipMin.y), clipMax, true);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padding.x + textInset, padding.y));
    const bool changed = ImGui::InputTextWithHint(label, hint, buffer, bufferSize);
    ImGui::PopStyleVar();
    draw->PopClipRect();

    const ImRect frame(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    draw->PushClipRect(clipMin, ImVec2(textLeft, clipMax.y), true);
    ImGui::RenderNavCursor(frame, ImGui::GetItemID());
    ImGui::RenderFrame(frame.Min, frame.Max, ImGui::GetColorU32(ImGuiCol_FrameBg), true,
                       ImGui::GetStyle().FrameRounding);
    draw->PopClipRect();

    if (ImGui::IsItemActive()) {
        const auto &style = ImGui::GetStyle();
        const float borderWidth = ImMax(style.FrameBorderSize, scale);
        // 在两侧背景绘制完成后统一描边，让输入焦点的高亮覆盖整个搜索框。
        draw->AddRect(frame.Min, frame.Max, ImGui::GetColorU32(ImGuiCol_NavCursor), style.FrameRounding,
                      ImDrawFlags_None, borderWidth);
    }

    const ImVec2 iconMin(frame.Min.x + padding.x, frame.GetCenter().y - SearchInputLayout::IconSize * scale * .5f);
    const ImVec2 center(iconMin.x + SearchInputLayout::LensCenter * scale,
                        iconMin.y + SearchInputLayout::LensCenter * scale);
    const float radius = SearchInputLayout::LensRadius * scale;
    const float diagonal = radius * SearchInputLayout::DiagonalUnit;
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    draw->AddCircle(center, radius, color, 0, SearchInputLayout::StrokeWidth * scale);
    draw->AddLine(
            ImVec2(center.x + diagonal, center.y + diagonal),
            ImVec2(iconMin.x + SearchInputLayout::HandleEnd * scale, iconMin.y + SearchInputLayout::HandleEnd * scale),
            color, SearchInputLayout::StrokeWidth * scale);
    return changed;
}

} // namespace shotlet::ImGuiUtils
