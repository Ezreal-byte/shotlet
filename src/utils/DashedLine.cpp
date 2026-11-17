#include "DashedLine.h"
#include "Colors.h"
#include "IStyle.h"

namespace shotlet {

void DrawDashedLine(ImDrawList* drawList,
                    const ImVec2& p1,
                    const ImVec2& p2,
                    const ImU32 color,
                    const float thickness,
                    const float dashLen,
                    const float gapLen,
                    const float uiScale) {
    const float scale = ImMax(uiScale, 1.0f);
    ImVec2 dir(p2.x - p1.x, p2.y - p1.y);
    const float len = sqrtf(dir.x * dir.x + dir.y * dir.y);
    if (len < 0.001f) return;
    dir.x /= len;
    dir.y /= len;

    float t = 0.0f;
    while (t < len) {
        const float dashEnd = ImMin(t + dashLen, len);
        drawList->AddLine(ImVec2(p1.x + dir.x * t, p1.y + dir.y * t),
                          ImVec2(p1.x + dir.x * dashEnd, p1.y + dir.y * dashEnd),
                          color, thickness * scale);
        t += dashLen * scale + gapLen * scale;
    }
}

void DrawDashedRect(ImDrawList* drawList,
                    const ImVec2& rectMin,
                    const ImVec2& rectMax,
                    const ImU32 color,
                    const float thickness,
                    const float dashLen,
                    const float gapLen,
                    const float uiScale) {
    // 上、下、左、右边框
    DrawDashedLine(drawList, rectMin, ImVec2(rectMax.x, rectMin.y), color, thickness, dashLen, gapLen, uiScale);
    DrawDashedLine(drawList, ImVec2(rectMin.x, rectMax.y), rectMax, color, thickness, dashLen, gapLen, uiScale);
    DrawDashedLine(drawList, rectMin, ImVec2(rectMin.x, rectMax.y), color, thickness, dashLen, gapLen, uiScale);
    DrawDashedLine(drawList, ImVec2(rectMax.x, rectMin.y), rectMax, color, thickness, dashLen, gapLen, uiScale);
}

    // 绘制图层选中后的虚线外边框，带 8 个圆形手柄操作点
void DrawDashedSelectionBorder(ImDrawList* drawList,
                               const ImRect& bounds,
                               const bool showHandles,
                               const float uiScale) {
    const float scale = ImMax(uiScale, 1.0f);
    DrawDashedRect(drawList, bounds.Min, bounds.Max, Colors::SelectedBorder,
                   IStyle::DashThickness,
                   IStyle::DashLen,
                   IStyle::GapLen,
                   scale);

    if (!showHandles) return;

    const float midX = (bounds.Min.x + bounds.Max.x) * 0.5f;
    const float midY = (bounds.Min.y + bounds.Max.y) * 0.5f;
    const ImVec2 handles[8] = {
        ImVec2(bounds.Min.x, bounds.Min.y),
        ImVec2(bounds.Max.x, bounds.Min.y),
        ImVec2(bounds.Min.x, bounds.Max.y),
        ImVec2(bounds.Max.x, bounds.Max.y),
        ImVec2(midX, bounds.Min.y),
        ImVec2(midX, bounds.Max.y),
        ImVec2(bounds.Min.x, midY),
        ImVec2(bounds.Max.x, midY)
    };
    for (int i = 0; i < 8; i++) {
        drawList->AddCircleFilled(handles[i], IStyle::HandleRadius * scale, Colors::HandleFill);
        drawList->AddCircle(handles[i], IStyle::HandleRadius * scale, Colors::HandleBorder, 0, scale);
    }
}

} // namespace shotlet
