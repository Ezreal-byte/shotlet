#include "../Layer.h"
#include "LayerRasterUtils.h"
#include "utils/Constants.h"
#include "utils/DashedLine.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace shotlet {

// ========== EllipseLayer ==========
EllipseLayer::EllipseLayer(ImVec2 start, ImVec2 end, ImU32 color, float thickness)
    : Layer(color, thickness), m_start(start), m_end(end) {}

ImRect EllipseLayer::GetBounds() const {
    ImVec2 minP(std::min(m_start.x, m_end.x), std::min(m_start.y, m_end.y));
    ImVec2 maxP(std::max(m_start.x, m_end.x), std::max(m_start.y, m_end.y));
    return ImRect(minP, maxP);
}

void EllipseLayer::Render(ImDrawList* drawList, const ImVec2& canvasOffset, float uiScale) {
    ImVec2 minP(std::min(m_start.x, m_end.x) + canvasOffset.x,
                std::min(m_start.y, m_end.y) + canvasOffset.y);
    ImVec2 maxP(std::max(m_start.x, m_end.x) + canvasOffset.x,
                std::max(m_start.y, m_end.y) + canvasOffset.y);

    const float scaledThickness = m_thickness * uiScale;
    ImVec2 center((minP.x + maxP.x) * 0.5f, (minP.y + maxP.y) * 0.5f);
    float rx = (maxP.x - minP.x) * 0.5f;
    float ry = (maxP.y - minP.y) * 0.5f;

    // 椭圆
    if (rx > 0 && ry > 0) {
        if (m_filled) {
            drawList->AddEllipseFilled(center, ImVec2(rx, ry), m_color);
        } else {
            // ImGui 内置椭圆绘制（内部用缓存的 sin/cos 表），替代每帧
            // 手写 64 次 cosf/sinf + vector 分配
            drawList->AddEllipse(center, ImVec2(rx, ry), m_color, 0.0f, 64, scaledThickness);
        }
    }

    // 虚线边框 + 8 个圆形手柄（仅拖拽预览时显示，在椭圆上层）
    if (m_showBorder) {
        DrawDashedSelectionBorder(drawList, ImRect(minP, maxP), true, uiScale);
    }
}

void EllipseLayer::RenderToImage(unsigned char* imageData, int imgWidth, int imgHeight,
                                int rowPitch, const ImVec2& canvasOffset, float scaleFactor) {
    ImVec2 minP(std::min(m_start.x, m_end.x) * scaleFactor, std::min(m_start.y, m_end.y) * scaleFactor);
    ImVec2 maxP(std::max(m_start.x, m_end.x) * scaleFactor, std::max(m_start.y, m_end.y) * scaleFactor);

    float cx = (minP.x + maxP.x) * 0.5f;
    float cy = (minP.y + maxP.y) * 0.5f;
    float rx = (maxP.x - minP.x) * 0.5f;
    float ry = (maxP.y - minP.y) * 0.5f;

    // 白色矩形边框（仅拖拽预览时显示）
    if (m_showBorder) {
        ImU32 borderColor = Colors::SelectedBorder;
        DrawLineToImage(imageData, imgWidth, imgHeight, rowPitch, canvasOffset,
                        ImVec2(minP.x, minP.y), ImVec2(maxP.x, minP.y), borderColor, 1.0f);
        DrawLineToImage(imageData, imgWidth, imgHeight, rowPitch, canvasOffset,
                        ImVec2(maxP.x, minP.y), ImVec2(maxP.x, maxP.y), borderColor, 1.0f);
        DrawLineToImage(imageData, imgWidth, imgHeight, rowPitch, canvasOffset,
                        ImVec2(maxP.x, maxP.y), ImVec2(minP.x, maxP.y), borderColor, 1.0f);
        DrawLineToImage(imageData, imgWidth, imgHeight, rowPitch, canvasOffset,
                        ImVec2(minP.x, maxP.y), ImVec2(minP.x, minP.y), borderColor, 1.0f);
    }

    // 椭圆
    if (rx > 0 && ry > 0) {
        // 填充椭圆内部（像素级椭圆判定）
        if (m_filled) {
            unsigned char cr = (m_color >> 0) & 0xFF;
            unsigned char cg = (m_color >> 8) & 0xFF;
            unsigned char cb = (m_color >> 16) & 0xFF;
            unsigned char ca = (m_color >> 24) & 0xFF;

            int x0 = static_cast<int>(std::floor(minP.x - canvasOffset.x));
            int y0 = static_cast<int>(std::floor(minP.y - canvasOffset.y));
            int x1 = static_cast<int>(std::ceil(maxP.x - canvasOffset.x));
            int y1 = static_cast<int>(std::ceil(maxP.y - canvasOffset.y));

            x0 = std::max(0, x0); y0 = std::max(0, y0);
            x1 = std::min(imgWidth - 1, x1); y1 = std::min(imgHeight - 1, y1);

            float ellipseCX = cx - canvasOffset.x;
            float ellipseCY = cy - canvasOffset.y;

            for (int y = y0; y <= y1; y++) {
                for (int x = x0; x <= x1; x++) {
                    float dx = static_cast<float>(x) - ellipseCX;
                    float dy = static_cast<float>(y) - ellipseCY;
                    if (dx * dx / (rx * rx) + dy * dy / (ry * ry) <= 1.0f) {
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
        }

        // 描边椭圆（仅非填充模式绘制边框线）
        if (!m_filled) {
            int numSegments = 64;
            ImVec2 prev;
            for (int i = 0; i <= numSegments; i++) {
                float angle = (float)i / (float)numSegments * 2.0f * M_PI;
                ImVec2 p(cx + rx * cosf(angle), cy + ry * sinf(angle));
                if (i > 0) {
                    DrawLineToImage(imageData, imgWidth, imgHeight, rowPitch, canvasOffset,
                                    prev, p, m_color, m_thickness * scaleFactor);
                }
                prev = p;
            }
        }
    }
}

std::unique_ptr<Layer> EllipseLayer::Clone() const {
    return std::make_unique<EllipseLayer>(m_start, m_end, m_color, m_thickness);
}

// ========== EllipseLayer 拖拽/缩放/选中 UI ==========

void EllipseLayer::StartDrag(const ImVec2& mousePos) {
    m_dragging = true;
    m_dragStartMouse = mousePos;
    m_posStartBeforeDrag = m_start;
    m_posEndBeforeDrag = m_end;
}

void EllipseLayer::UpdateDrag(const ImVec2& delta) {
    ImVec2 newStart(m_posStartBeforeDrag.x + delta.x, m_posStartBeforeDrag.y + delta.y);
    ImVec2 newEnd(m_posEndBeforeDrag.x + delta.x, m_posEndBeforeDrag.y + delta.y);
    m_start = newStart;
    m_end = newEnd;
}

void EllipseLayer::EndDrag() {
    m_dragging = false;
}

ResizeHandle EllipseLayer::HitTestResizeHandle(const ImVec2& pos) const {
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

void EllipseLayer::StartResize(ResizeHandle handle) {
    m_resizeHandle = handle;
    m_boundsBeforeResize = GetBounds();
}

void EllipseLayer::UpdateResize(const ImVec2& delta, const ImRect& selection) {
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

    // 最小尺寸约束
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

void EllipseLayer::EndResize() {
    m_resizeHandle = ResizeHandle::None;
}

void EllipseLayer::RenderSelectionUI(ImDrawList* drawList, float uiScale) const {
    DrawDashedSelectionBorder(drawList, GetBounds(), true, uiScale);
}

bool EllipseLayer::IsPointNearBorder(const ImVec2& pos, float threshold) const {
    ImRect bounds = GetBounds();
    float cx = (bounds.Min.x + bounds.Max.x) * 0.5f;
    float cy = (bounds.Min.y + bounds.Max.y) * 0.5f;
    float rx = (bounds.Max.x - bounds.Min.x) * 0.5f;
    float ry = (bounds.Max.y - bounds.Min.y) * 0.5f;

    if (rx <= 0 || ry <= 0) return false;

    // 归一化圆法检测椭圆边框距离
    float u = (pos.x - cx) / rx;
    float v = (pos.y - cy) / ry;
    float d = sqrtf(u * u + v * v);

    if (d < 1e-6f) return (rx < ry ? rx : ry) <= threshold;

    float nearX = cx + (pos.x - cx) / d;
    float nearY = cy + (pos.y - cy) / d;
    float dx = pos.x - nearX;
    float dy = pos.y - nearY;
    float dist = sqrtf(dx * dx + dy * dy);

    if (dist <= threshold) return true;

    // 填充模式下，点在椭圆内部也算选中
    if (m_filled) {
        // u²+v² < 1 表示点在椭圆内部
        return (u * u + v * v) <= 1.0f;
    }

    return false;
}


} // namespace shotlet
