#pragma once
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <utility>
#include <limits>
#include <optional>
#include <span>

namespace shotlet::GeometryUtils {

// 求凸多边形与水平带状区域相交部分的横向范围，避免把斜长矩形展开成巨大的包围盒。
inline std::optional<std::pair<float, float>> HorizontalStripXBounds(
    std::span<const ImVec2> polygon, float top, float bottom) {
    float minimum = std::numeric_limits<float>::max();
    float maximum = std::numeric_limits<float>::lowest();
    const auto include = [&](float x) { minimum = std::min(minimum, x); maximum = std::max(maximum, x); };
    for (size_t i = 0; i < polygon.size(); ++i) {
        const auto a = polygon[i], b = polygon[(i + 1) % polygon.size()];
        if (a.y >= top && a.y <= bottom) include(a.x);
        if (a.y == b.y) continue;
        for (const float y : {top, bottom}) {
            if (y >= std::min(a.y, b.y) && y <= std::max(a.y, b.y)) {
                include(a.x + (b.x - a.x) * ((y - a.y) / (b.y - a.y)));
            }
        }
    }
    if (minimum > maximum) return std::nullopt;
    return std::pair{minimum, maximum};
}

// 取矩形边界上距离圆心最近的触点，再连接到圆周，避免穿过任一形状。
// 圆心位于框的水平范围内时保持垂直连线，靠近选区边缘创建也不例外。
inline std::pair<ImVec2, ImVec2> ConnectRectangleToCircle(const ImRect& rectangle, ImVec2 center, float radius) {
    const ImVec2 start(std::clamp(center.x, rectangle.Min.x, rectangle.Max.x),
                       std::clamp(center.y, rectangle.Min.y, rectangle.Max.y));
    const float toFrameX = start.x - center.x, toFrameY = start.y - center.y;
    const float distance = std::hypot(toFrameX, toFrameY);
    // 相交或相切时两者已接触，不生成方向反转或长度为负的连线。
    if (distance <= radius) return {start, start};
    return {start, ImVec2(center.x + toFrameX * radius / distance, center.y + toFrameY * radius / distance)};
}
} // namespace shotlet::GeometryUtils
