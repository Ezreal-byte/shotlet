#pragma once
#include <imgui.h>
#include <imgui_internal.h>

namespace shotlet {

// 绘制两点之间的虚线（沿线段方向步进绘制 dash）
void DrawDashedLine(ImDrawList* drawList,
                    const ImVec2& p1,
                    const ImVec2& p2,
                    ImU32 color,
                    float thickness = 1.0f,
                    float dashLen = 5.0f,
                    float gapLen = 3.0f,
                    float uiScale = 1.0f);

// 用 DrawList 绘制虚线矩形边框
void DrawDashedRect(ImDrawList* drawList,
                    const ImVec2& rectMin,
                    const ImVec2& rectMax,
                    ImU32 color,
                    float thickness = 1.0f,
                    float dashLen = 5.0f,
                    float gapLen = 3.0f,
                    float uiScale = 1.0f);

// 绘制带 8 个圆形手柄的虚线选中边框（图层选中/智能区域/绘制预览等通用）
// showHandles=false 时只画虚线边框不画手柄（如 BrushLayer）
void DrawDashedSelectionBorder(ImDrawList* drawList,
                               const ImRect& bounds,
                               bool showHandles = true,
                               float uiScale = 1.0f);

} // namespace shotlet
