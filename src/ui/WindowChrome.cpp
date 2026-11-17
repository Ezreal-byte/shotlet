#include "WindowChrome.h"
#include "utils/Colors.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>

namespace shotlet::WindowChrome {

namespace {
constexpr float IconSize = 10.0f;
constexpr float IconStrokeWidth = 1.0f;
constexpr float RestoreOffset = 2.0f;
}

float WindowControlsWidth(float uiScale, bool includeMaximize) {
    const int buttonCount = includeMaximize ? 3 : 2;
    return std::ceil(buttonCount * WindowControlButtonWidth * uiScale);
}

namespace {

ImRect WindowControlsRect(float windowWidth, float height, float uiScale, bool includeMaximize) {
    const float width = WindowControlsWidth(uiScale, includeMaximize);
    // ImGui 会截断窗口位置和尺寸，命中区域必须使用同一像素边界，避免分数 DPI 下多出一行。
    const ImVec2 position(ImFloor(ImMax(0.0f, windowWidth - width)), 0);
    return ImRect(position, ImVec2(position.x + width, ImFloor(height)));
}

// 单个窗口控制按钮：悬停高亮并绘制最小化/最大化还原/关闭图标。
void RenderWindowControl(WindowAction action, ImVec2 size, Theme theme, Language language,
                         float uiScale, bool maximized, ImFont* tooltipFont,
                         const std::function<void(WindowAction)>& onAction) {
    const auto& palette = GetThemePalette(theme);
    const ImVec2 position = ImGui::GetCursorScreenPos();
    ImGui::PushID(static_cast<int>(action));
    const bool clicked = ImGui::InvisibleButton("window_control", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool close = action == WindowAction::Close;
    auto* draw = ImGui::GetWindowDrawList();
    if (hovered) draw->AddRectFilled(position, ImVec2(position.x + size.x, position.y + size.y),
                                     close ? palette.danger : palette.selectedBackground);
    const ImU32 color = close && hovered ? Colors::White : palette.icon;
    const float stroke = IconStrokeWidth * uiScale;
    const float iconSize = IconSize * uiScale;
    const ImVec2 minimum(std::floor(position.x + (size.x - iconSize) / 2), std::floor(position.y + (size.y - iconSize) / 2));
    const ImVec2 maximum(minimum.x + iconSize, minimum.y + iconSize);
    if (action == WindowAction::Minimize) {
        const float centerY = minimum.y + iconSize / 2;
        draw->AddLine(ImVec2(minimum.x, centerY), ImVec2(maximum.x, centerY), color, stroke);
    } else if (close) {
        draw->AddLine(minimum, maximum, color, stroke);
        draw->AddLine(ImVec2(maximum.x, minimum.y), ImVec2(minimum.x, maximum.y), color, stroke);
    } else if (maximized) {
        const float offset = RestoreOffset * uiScale;
        draw->AddRect(ImVec2(minimum.x, minimum.y + offset), ImVec2(maximum.x - offset, minimum.y), color, 0, 0, stroke);
        // 还原图标只绘制后方窗口露出的边，避免重叠位置出现双倍线宽。
        const ImVec2 back[] = {ImVec2(minimum.x + offset, minimum.y + offset), ImVec2(minimum.x + offset, minimum.y),
                              ImVec2(maximum.x, minimum.y), ImVec2(maximum.x, maximum.y - offset),
                              ImVec2(maximum.x - offset, maximum.y - offset)};
        draw->AddPolyline(back, IM_ARRAYSIZE(back), color, ImDrawFlags_None, stroke);
    } else {
        draw->AddRect(minimum, maximum, color, 0, 0, stroke);
    }
    if (hovered) {
        const auto label = close ? TextId::WindowClose : action == WindowAction::Minimize ? TextId::WindowMinimize
            : maximized ? TextId::WindowRestore : TextId::WindowMaximize;
        if (tooltipFont) ImGui::PushFont(tooltipFont);
        ImGui::SetTooltip("%s", Tr(language, label));
        if (tooltipFont) ImGui::PopFont();
    }
    ImGui::PopID();
    if (clicked && onAction) onAction(action);
}

} // namespace

bool IsWindowControlsPoint(ImVec2 point, float windowWidth, float height, float uiScale, bool includeMaximize) {
    return WindowControlsRect(windowWidth, height, uiScale, includeMaximize).Contains(point);
}

void RenderWindowControls(const char* windowName, float height, Theme theme, Language language,
                          float uiScale, bool maximized, bool includeMaximize, ImU32 backgroundColor,
                          ImFont* tooltipFont, const std::function<void(WindowAction)>& onAction) {
    const auto bounds = WindowControlsRect(ImGui::GetIO().DisplaySize.x, height, uiScale, includeMaximize);
    const float width = bounds.GetWidth();
    if (width <= 0) return;
    ImGui::SetNextWindowPos(bounds.Min);
    ImGui::SetNextWindowSize(bounds.GetSize());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, backgroundColor);
    const auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    const WindowAction actions[] = {WindowAction::Minimize, WindowAction::ToggleMaximize, WindowAction::Close};
    const int buttonCount = includeMaximize ? 3 : 2;
    if (ImGui::Begin(windowName, nullptr, flags)) {
        for (int index = 0; index < buttonCount; ++index) {
            // 中间按钮省略后余下像素仍均分给相邻按钮，整个控制组恰好覆盖到窗口最右侧。
            const float left = std::floor(width * index / buttonCount);
            const float right = std::floor(width * (index + 1) / buttonCount);
            ImGui::SetCursorPos(ImVec2(left, 0));
            RenderWindowControl(actions[includeMaximize ? index : (index == 1 ? 2 : 0)], ImVec2(right - left, bounds.GetHeight()),
                                theme, language, uiScale, maximized, tooltipFont, onAction);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

} // namespace shotlet::WindowChrome
