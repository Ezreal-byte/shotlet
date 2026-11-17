#include "MaskGeometry.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace shotlet::MaskGeometry {
namespace {
constexpr int CornerSegments = 4;
constexpr float MinimumArea = 0.0001f;
constexpr float SharedPointTolerance = 0.001f;
float Cross(ImVec2 a, ImVec2 b, ImVec2 p) {
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}
bool HasArea(const Polygon& polygon) {
    if (polygon.size() < 3) return false;
    float area = 0;
    for (size_t i = 1; i + 1 < polygon.size(); ++i) area += Cross(polygon[0], polygon[i], polygon[i + 1]);
    return std::abs(area) > MinimumArea;
}
ImRect Bounds(const Polygon& polygon) {
    ImRect bounds(polygon.front(), polygon.front());
    for (const auto point : polygon) bounds.Add(point);
    return bounds;
}
std::pair<Polygon, Polygon> Split(const Polygon& polygon, ImVec2 a, ImVec2 b) {
    Polygon inside, outside;
    auto previous = polygon.back();
    float previousSide = Cross(a, b, previous);
    for (const auto point : polygon) {
        const float side = Cross(a, b, point);
        if ((side >= 0) != (previousSide >= 0)) {
            const float fraction = previousSide / (previousSide - side);
            const ImVec2 intersection(previous.x + (point.x - previous.x) * fraction,
                                      previous.y + (point.y - previous.y) * fraction);
            inside.push_back(intersection);
            outside.push_back(intersection);
        }
        (side >= 0 ? inside : outside).push_back(point);
        previous = point;
        previousSide = side;
    }
    return {std::move(inside), std::move(outside)};
}
void Subtract(Polygon subject, const Polygon& hole, std::vector<Polygon>& result) {
    if (!Bounds(subject).Overlaps(Bounds(hole))) {
        result.push_back(std::move(subject));
        return;
    }
    for (size_t i = 0; i < hole.size() && HasArea(subject); ++i) {
        auto [inside, outside] = Split(subject, hole[i], hole[(i + 1) % hole.size()]);
        if (HasArea(outside)) result.push_back(std::move(outside));
        subject = std::move(inside);
    }
}

std::vector<Polygon> StitchEdges(std::vector<Polygon> polygons) {
    Polygon points;
    // 同一交点的不同计算路径可能相差少量浮点尾数，统一顶点后再拆分共享边。
    for (auto& polygon : polygons) {
        for (auto& point : polygon) {
            const auto found = std::find_if(points.begin(), points.end(), [&](ImVec2 existing) {
                return std::hypot(point.x - existing.x, point.y - existing.y) <= SharedPointTolerance;
            });
            if (found == points.end()) points.push_back(point);
            else point = *found;
        }
    }
    for (auto& polygon : polygons) {
        Polygon stitched;
        for (size_t i = 0; i < polygon.size(); ++i) {
            const auto a = polygon[i], b = polygon[(i + 1) % polygon.size()];
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float lengthSquared = dx * dx + dy * dy;
            if (lengthSquared == 0) continue;
            std::vector<std::pair<float, ImVec2>> middle;
            const float crossTolerance = SharedPointTolerance * std::sqrt(lengthSquared);
            for (const auto point : points) {
                const float dot = (point.x - a.x) * dx + (point.y - a.y) * dy;
                if (dot > 0 && dot < lengthSquared && std::abs(Cross(a, b, point)) <= crossTolerance)
                    middle.emplace_back(dot / lengthSquared, point);
            }
            std::sort(middle.begin(), middle.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
            stitched.push_back(a);
            for (const auto& [fraction, point] : middle) stitched.push_back(point);
        }
        polygon = std::move(stitched);
    }
    return polygons;
}
}

Polygon Ellipse(ImRect bounds, int segments) {
    if (bounds.GetWidth() <= 0 || bounds.GetHeight() <= 0 || segments < 3) return {};
    Polygon polygon;
    polygon.reserve(segments);
    const auto center = bounds.GetCenter();
    for (int i = 0; i < segments; ++i) {
        const float angle = 2 * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(segments);
        polygon.emplace_back(center.x + bounds.GetWidth() / 2 * std::cos(angle),
                             center.y + bounds.GetHeight() / 2 * std::sin(angle));
    }
    return polygon;
}

Polygon RoundedRectangle(ImRect bounds, float rounding) {
    if (bounds.GetWidth() <= 0 || bounds.GetHeight() <= 0) return {};
    const float radius = std::clamp(rounding, 0.0f, std::min(bounds.GetWidth(), bounds.GetHeight()) / 2);
    if (radius == 0) return {bounds.Min, ImVec2(bounds.Max.x, bounds.Min.y), bounds.Max, ImVec2(bounds.Min.x, bounds.Max.y)};
    const ImVec2 centers[] = {{bounds.Max.x - radius, bounds.Min.y + radius},
        {bounds.Max.x - radius, bounds.Max.y - radius}, {bounds.Min.x + radius, bounds.Max.y - radius},
        {bounds.Min.x + radius, bounds.Min.y + radius}};
    Polygon polygon;
    for (int corner = 0; corner < 4; ++corner) {
        for (int i = 0; i <= CornerSegments; ++i) {
            const float angle = (static_cast<float>(corner - 1) + static_cast<float>(i) / CornerSegments) *
                std::numbers::pi_v<float> / 2;
            polygon.emplace_back(centers[corner].x + radius * std::cos(angle), centers[corner].y + radius * std::sin(angle));
        }
    }
    return polygon;
}

std::vector<Polygon> SubtractHoles(Polygon outer, const std::vector<Polygon>& holes) {
    if (!HasArea(outer)) return {};
    std::vector<Polygon> polygons;
    polygons.push_back(std::move(outer));
    for (const auto& hole : holes) {
        if (!HasArea(hole)) continue;
        std::vector<Polygon> remaining;
        for (auto& polygon : polygons) Subtract(std::move(polygon), hole, remaining);
        polygons = std::move(remaining);
    }
    return polygons;
}

void Draw(ImDrawList* drawList, const std::vector<Polygon>& polygons, ImVec2 offset, ImU32 color) {
    if (!drawList || !(color & IM_COL32_A_MASK)) return;
    // 消除 T 形接缝；否则 GPU 光栅化在长边与分段短边之间可能出现漏亮点或重复混合的暗点。
    const auto stitched = StitchEdges(polygons);
    for (auto polygon : stitched) {
        if (!HasArea(polygon)) continue;
        for (auto& point : polygon) { point.x += offset.x; point.y += offset.y; }
        // 从内部中心做三角扇，避免以边界点为扇心时，共线分段产生退化三角形并重复混合。
        ImVec2 center(0, 0);
        for (const auto point : polygon) { center.x += point.x; center.y += point.y; }
        const int count = static_cast<int>(polygon.size());
        center.x /= static_cast<float>(count); center.y /= static_cast<float>(count);
        drawList->PrimReserve(count * 3, count + 1);
        const auto base = drawList->_VtxCurrentIdx;
        const auto uv = drawList->_Data->TexUvWhitePixel;
        drawList->PrimWriteVtx(center, uv, color);
        for (const auto point : polygon) drawList->PrimWriteVtx(point, uv, color);
        for (int i = 0; i < count; ++i) {
            drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base));
            drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + i));
            drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + (i + 1) % count));
        }
    }
}
}
