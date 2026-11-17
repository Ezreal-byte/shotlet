#pragma once
#include <imgui_internal.h>
#include <vector>

namespace shotlet::MaskGeometry {
using Polygon = std::vector<ImVec2>;
constexpr int EllipseSegments = 128;

Polygon Ellipse(ImRect bounds, int segments = EllipseSegments);
Polygon RoundedRectangle(ImRect bounds, float rounding = 0);
// 凸多边形逐个切除，返回互不重叠的凸片，多个孔洞取并集而不会重复压暗。
std::vector<Polygon> SubtractHoles(Polygon outer, const std::vector<Polygon>& holes);
void Draw(ImDrawList* drawList, const std::vector<Polygon>& polygons, ImVec2 offset, ImU32 color);
}
