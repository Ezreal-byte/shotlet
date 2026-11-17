#include "../Layer.h"
#include "LayerRasterUtils.h"
#include "utils/Constants.h"
#include "utils/DashedLine.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace shotlet {

// ========== BrushLayer ==========
BrushLayer::BrushLayer(std::vector<ImVec2> points, ImU32 color, float thickness)
    : Layer(color, thickness), m_points(std::move(points)) {}

ImRect BrushLayer::GetBounds() const {
    if (m_points.empty()) return ImRect(0, 0, 0, 0);

    float minX = m_points[0].x, maxX = m_points[0].x;
    float minY = m_points[0].y, maxY = m_points[0].y;
    for (const auto& p : m_points) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    return ImRect(minX, minY, maxX, maxY);
}

void BrushLayer::Render(ImDrawList* drawList, const ImVec2& canvasOffset, float uiScale) {
    if (m_points.size() < 2) return;

    if (canvasOffset.x == 0.0f && canvasOffset.y == 0.0f) {
        // 常见路径（渲染循环恒传 (0,0)）：直接用原始点，免去每帧拷贝整个笔迹
        drawList->AddPolyline(m_points.data(), static_cast<int>(m_points.size()), m_color, ImDrawFlags_None, m_thickness * uiScale);
        return;
    }

    std::vector<ImVec2> pts;
    pts.reserve(m_points.size());
    for (const auto& p : m_points) {
        pts.push_back(ImVec2(p.x + canvasOffset.x, p.y + canvasOffset.y));
    }
    drawList->AddPolyline(pts.data(), static_cast<int>(pts.size()), m_color, ImDrawFlags_None, m_thickness * uiScale);
}

void BrushLayer::RenderToImage(unsigned char* imageData, int imgWidth, int imgHeight,
                               int rowPitch, const ImVec2& canvasOffset, float scaleFactor) {
    if (m_points.size() < 2) return;

    for (size_t i = 0; i < m_points.size() - 1; i++) {
        ImVec2 p1(m_points[i].x * scaleFactor, m_points[i].y * scaleFactor);
        ImVec2 p2(m_points[i + 1].x * scaleFactor, m_points[i + 1].y * scaleFactor);
        DrawLineToImage(imageData, imgWidth, imgHeight, rowPitch, canvasOffset,
                        p1, p2, m_color, m_thickness * scaleFactor);
    }
}

std::unique_ptr<Layer> BrushLayer::Clone() const {
    return std::make_unique<BrushLayer>(m_points, m_color, m_thickness);
}

void BrushLayer::OffsetAllPoints(const ImVec2& offset) {
    for (auto& p : m_points) {
        p.x += offset.x;
        p.y += offset.y;
    }
}

// ========== BrushLayer 拖拽/选中 UI ==========

void BrushLayer::StartDrag(const ImVec2& mousePos) {
    m_dragging = true;
    m_boundsBeforeDrag = GetBounds();
    m_totalDelta = ImVec2(0, 0);
}

void BrushLayer::UpdateDrag(const ImVec2& delta) {
    // 计算新总偏移
    ImVec2 newTotalDelta = ImVec2(m_totalDelta.x + delta.x, m_totalDelta.y + delta.y);
    ImVec2 correctedIncrement = ImVec2(newTotalDelta.x - m_totalDelta.x,
                                        newTotalDelta.y - m_totalDelta.y);
    m_totalDelta = newTotalDelta;

    OffsetAllPoints(correctedIncrement);
}

void BrushLayer::EndDrag() {
    m_dragging = false;
}

void BrushLayer::RenderSelectionUI(ImDrawList* drawList, float uiScale) const {
    DrawDashedSelectionBorder(drawList, GetBounds(), false, uiScale);
}

bool BrushLayer::IsPointNearBorder(const ImVec2& pos, float threshold) const {
    if (m_points.size() < 2) return false;

    for (size_t i = 0; i < m_points.size() - 1; i++) {
        ImVec2 p1 = m_points[i];
        ImVec2 p2 = m_points[i + 1];

        ImVec2 seg(p2.x - p1.x, p2.y - p1.y);
        float segLenSq = seg.x * seg.x + seg.y * seg.y;

        if (segLenSq < 0.001f) {
            float dx = pos.x - p1.x;
            float dy = pos.y - p1.y;
            if (dx * dx + dy * dy <= threshold * threshold) return true;
            continue;
        }

        float t = ((pos.x - p1.x) * seg.x + (pos.y - p1.y) * seg.y) / segLenSq;
        t = std::max(0.0f, std::min(1.0f, t));

        ImVec2 proj(p1.x + t * seg.x, p1.y + t * seg.y);
        float dx = pos.x - proj.x;
        float dy = pos.y - proj.y;
        if (dx * dx + dy * dy <= threshold * threshold) return true;
    }

    return false;
}


} // namespace shotlet
