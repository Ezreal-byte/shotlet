#include "../Layer.h"
#include "LayerRasterUtils.h"
#include "utils/Constants.h"
#include "utils/DashedLine.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace shotlet {

// ========== RectangleLayer ==========
RectangleLayer::RectangleLayer(ImVec2 start, ImVec2 end, ImU32 color, float thickness)
    : Layer(color, thickness), m_start(start), m_end(end) {}

ImRect RectangleLayer::GetBounds() const {
    ImVec2 minP(std::min(m_start.x, m_end.x), std::min(m_start.y, m_end.y));
    ImVec2 maxP(std::max(m_start.x, m_end.x), std::max(m_start.y, m_end.y));
    return ImRect(minP, maxP);
}

void RectangleLayer::Render(ImDrawList* drawList, const ImVec2& canvasOffset, float uiScale) {
    ImVec2 start(m_start.x + canvasOffset.x, m_start.y + canvasOffset.y);
    ImVec2 end(m_end.x + canvasOffset.x, m_end.y + canvasOffset.y);
    ImVec2 minP(std::min(start.x, end.x), std::min(start.y, end.y));
    ImVec2 maxP(std::max(start.x, end.x), std::max(start.y, end.y));

    const float scaledThickness = m_thickness * uiScale;

    if (m_filled) {
        drawList->AddRectFilled(minP, maxP, m_color);
    } else {
        drawList->AddRect(minP, maxP, m_color, 0.0f, 0, scaledThickness);
    }

    if (m_showBorder) {
        DrawDashedSelectionBorder(drawList, GetBounds(), true, uiScale);
    }
}

void RectangleLayer::RenderToImage(unsigned char* imageData, int imgWidth, int imgHeight,
                                   int rowPitch, const ImVec2& canvasOffset, float scaleFactor) {
    ImVec2 minP(std::min(m_start.x, m_end.x), std::min(m_start.y, m_end.y));
    minP.x *= scaleFactor;
    minP.y *= scaleFactor;
    ImVec2 maxP(std::max(m_start.x, m_end.x), std::max(m_start.y, m_end.y));
    maxP.x *= scaleFactor;
    maxP.y *= scaleFactor;

    // 填充矩形内部
    if (m_filled) {
        unsigned char cr = (m_color >> 0) & 0xFF;
        unsigned char cg = (m_color >> 8) & 0xFF;
        unsigned char cb = (m_color >> 16) & 0xFF;
        unsigned char ca = (m_color >> 24) & 0xFF;

        int x0 = static_cast<int>(std::round(minP.x - canvasOffset.x));
        int y0 = static_cast<int>(std::round(minP.y - canvasOffset.y));
        int x1 = static_cast<int>(std::round(maxP.x - canvasOffset.x));
        int y1 = static_cast<int>(std::round(maxP.y - canvasOffset.y));

        x0 = std::max(0, x0); y0 = std::max(0, y0);
        x1 = std::min(imgWidth - 1, x1); y1 = std::min(imgHeight - 1, y1);

        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                int idx = y * rowPitch + x * 4;
                float srcA = ca / 255.0f;
                float dstA = imageData[idx + 3] / 255.0f;
                float outA = srcA + dstA * (1.0f - srcA);
                if (outA > 0.0f) {
                    float srcRatio = srcA / outA;
                    float dstRatio = 1.0f - srcRatio;
                    imageData[idx]     = static_cast<unsigned char>(cr * srcRatio + imageData[idx] * dstRatio);
                    imageData[idx + 1] = static_cast<unsigned char>(cg * srcRatio + imageData[idx + 1] * dstRatio);
                    imageData[idx + 2] = static_cast<unsigned char>(cb * srcRatio + imageData[idx + 2] * dstRatio);
                    imageData[idx + 3] = static_cast<unsigned char>(std::min(255.0f, outA * 255.0f));
                }
            }
        }
    }

    // 描边（仅非填充模式绘制边框线）
    if (!m_filled) {
        DrawLineToImage(imageData, imgWidth, imgHeight, rowPitch, canvasOffset,
                        ImVec2(minP.x, minP.y), ImVec2(maxP.x, minP.y), m_color, m_thickness * scaleFactor);
        DrawLineToImage(imageData, imgWidth, imgHeight, rowPitch, canvasOffset,
                        ImVec2(maxP.x, minP.y), ImVec2(maxP.x, maxP.y), m_color, m_thickness * scaleFactor);
        DrawLineToImage(imageData, imgWidth, imgHeight, rowPitch, canvasOffset,
                        ImVec2(maxP.x, maxP.y), ImVec2(minP.x, maxP.y), m_color, m_thickness * scaleFactor);
        DrawLineToImage(imageData, imgWidth, imgHeight, rowPitch, canvasOffset,
                        ImVec2(minP.x, maxP.y), ImVec2(minP.x, minP.y), m_color, m_thickness * scaleFactor);
    }
}

std::unique_ptr<Layer> RectangleLayer::Clone() const {
    return std::make_unique<RectangleLayer>(m_start, m_end, m_color, m_thickness);
}

// ========== RectangleLayer 拖拽/缩放/选中 UI ==========

void RectangleLayer::StartDrag(const ImVec2& mousePos) {
    m_dragging = true;
    m_dragStartMouse = mousePos;
    m_posStartBeforeDrag = m_start;
    m_posEndBeforeDrag = m_end;
}

void RectangleLayer::UpdateDrag(const ImVec2& delta) {
    ImVec2 newStart(m_posStartBeforeDrag.x + delta.x, m_posStartBeforeDrag.y + delta.y);
    ImVec2 newEnd(m_posEndBeforeDrag.x + delta.x, m_posEndBeforeDrag.y + delta.y);
    m_start = newStart;
    m_end = newEnd;
}

void RectangleLayer::EndDrag() {
    m_dragging = false;
}

ResizeHandle RectangleLayer::HitTestResizeHandle(const ImVec2& pos) const {
    ImRect bounds = GetBounds();
    float h = 8.0f + 2;  // HANDLE_SIZE + 2
    float midX = (bounds.Min.x + bounds.Max.x) * 0.5f;
    float midY = (bounds.Min.y + bounds.Max.y) * 0.5f;

    // 角点
    if (ImRect(ImVec2(bounds.Min.x - h, bounds.Min.y - h), ImVec2(bounds.Min.x + h, bounds.Min.y + h)).Contains(pos))
        return ResizeHandle::TopLeft;
    if (ImRect(ImVec2(bounds.Max.x - h, bounds.Min.y - h), ImVec2(bounds.Max.x + h, bounds.Min.y + h)).Contains(pos))
        return ResizeHandle::TopRight;
    if (ImRect(ImVec2(bounds.Min.x - h, bounds.Max.y - h), ImVec2(bounds.Min.x + h, bounds.Max.y + h)).Contains(pos))
        return ResizeHandle::BottomLeft;
    if (ImRect(ImVec2(bounds.Max.x - h, bounds.Max.y - h), ImVec2(bounds.Max.x + h, bounds.Max.y + h)).Contains(pos))
        return ResizeHandle::BottomRight;

    // 边中点
    if (ImRect(ImVec2(midX - h, bounds.Min.y - h), ImVec2(midX + h, bounds.Min.y + h)).Contains(pos))
        return ResizeHandle::Top;
    if (ImRect(ImVec2(midX - h, bounds.Max.y - h), ImVec2(midX + h, bounds.Max.y + h)).Contains(pos))
        return ResizeHandle::Bottom;
    if (ImRect(ImVec2(bounds.Min.x - h, midY - h), ImVec2(bounds.Min.x + h, midY + h)).Contains(pos))
        return ResizeHandle::Left;
    if (ImRect(ImVec2(bounds.Max.x - h, midY - h), ImVec2(bounds.Max.x + h, midY + h)).Contains(pos))
        return ResizeHandle::Right;

    return ResizeHandle::None;
}

void RectangleLayer::StartResize(ResizeHandle handle) {
    m_resizeHandle = handle;
    m_boundsBeforeResize = GetBounds();
}

void RectangleLayer::UpdateResize(const ImVec2& delta, const ImRect& selection) {
    ImRect r = m_boundsBeforeResize;
    ImVec2 newMin = r.Min;
    ImVec2 newMax = r.Max;

    switch (m_resizeHandle) {
        case ResizeHandle::TopLeft:
            newMin = ImVec2(r.Min.x + delta.x, r.Min.y + delta.y);
            break;
        case ResizeHandle::Top:
            newMin = ImVec2(r.Min.x, r.Min.y + delta.y);
            break;
        case ResizeHandle::TopRight:
            newMin = ImVec2(r.Min.x, r.Min.y + delta.y);
            newMax = ImVec2(r.Max.x + delta.x, r.Max.y);
            break;
        case ResizeHandle::Left:
            newMin = ImVec2(r.Min.x + delta.x, r.Min.y);
            break;
        case ResizeHandle::Right:
            newMax = ImVec2(r.Max.x + delta.x, r.Max.y);
            break;
        case ResizeHandle::BottomLeft:
            newMin = ImVec2(r.Min.x + delta.x, r.Min.y);
            newMax = ImVec2(r.Max.x, r.Max.y + delta.y);
            break;
        case ResizeHandle::Bottom:
            newMax = ImVec2(r.Max.x, r.Max.y + delta.y);
            break;
        case ResizeHandle::BottomRight:
            newMax = ImVec2(r.Max.x + delta.x, r.Max.y + delta.y);
            break;
        default:
            break;
    }

    // 最小尺寸约束（防止缩成点）
    const float minSize = 10.0f;
    if (newMax.x - newMin.x < minSize) {
        if (m_resizeHandle == ResizeHandle::Left ||
            m_resizeHandle == ResizeHandle::TopLeft ||
            m_resizeHandle == ResizeHandle::BottomLeft) {
            newMin.x = newMax.x - minSize;
        } else {
            newMax.x = newMin.x + minSize;
        }
    }
    if (newMax.y - newMin.y < minSize) {
        if (m_resizeHandle == ResizeHandle::Top ||
            m_resizeHandle == ResizeHandle::TopLeft ||
            m_resizeHandle == ResizeHandle::TopRight) {
            newMin.y = newMax.y - minSize;
        } else {
            newMax.y = newMin.y + minSize;
        }
    }

    // 限制在选区范围内
    newMin.x = std::max(newMin.x, selection.Min.x);
    newMin.y = std::max(newMin.y, selection.Min.y);
    newMax.x = std::min(newMax.x, selection.Max.x);
    newMax.y = std::min(newMax.y, selection.Max.y);

    m_start = newMin;
    m_end = newMax;
}

void RectangleLayer::EndResize() {
    m_resizeHandle = ResizeHandle::None;
}

void RectangleLayer::RenderSelectionUI(ImDrawList* drawList, float uiScale) const {
    DrawDashedSelectionBorder(drawList, GetBounds(), true, uiScale);
}

bool RectangleLayer::IsPointNearBorder(const ImVec2& pos, float threshold) const {
    ImRect bounds = GetBounds();
    float minX = bounds.Min.x, maxX = bounds.Max.x;
    float minY = bounds.Min.y, maxY = bounds.Max.y;

    bool nearLeft   = pos.x >= minX - threshold && pos.x <= minX + threshold && pos.y >= minY - threshold && pos.y <= maxY + threshold;
    bool nearRight  = pos.x >= maxX - threshold && pos.x <= maxX + threshold && pos.y >= minY - threshold && pos.y <= maxY + threshold;
    bool nearTop    = pos.y >= minY - threshold && pos.y <= minY + threshold && pos.x >= minX - threshold && pos.x <= maxX + threshold;
    bool nearBottom = pos.y >= maxY - threshold && pos.y <= maxY + threshold && pos.x >= minX - threshold && pos.x <= maxX + threshold;

    if (nearLeft || nearRight || nearTop || nearBottom) return true;

    // 填充模式下，点在矩形内部也算选中
    if (m_filled) {
        return pos.x >= minX && pos.x <= maxX && pos.y >= minY && pos.y <= maxY;
    }

    return false;
}


} // namespace shotlet
