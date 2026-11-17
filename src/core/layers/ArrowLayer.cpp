#include "../Layer.h"
#include "LayerRasterUtils.h"
#include "utils/DashedLine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace shotlet {

namespace {
constexpr float kHeadLengthPerThickness = 8.5f;
constexpr float kMinHeadLength = 28.0f;
constexpr float kHeadWidthToLength = 0.8f;
constexpr float kHeadInsetRatio = 0.06f;
constexpr float kMaxHeadLengthFraction = 0.6f;
constexpr float kShaftWidthPerThickness = 2.0f;
constexpr float kMinShaftWidth = 5.0f;
constexpr float kTailWidth = 1.0f;
constexpr float kBoundsMarginPerThickness = 5.0f;
constexpr float kMinBoundsMargin = 16.0f;

struct ArrowGeometry {
    ImVec2 tailLeft, tailRight;
    ImVec2 neckLeft, neckRight;
    ImVec2 wingLeft, wingRight;
    ImVec2 tip;

    std::array<ImVec2, 7> Outline() const {
        return {tailLeft, tailRight, neckRight, wingRight, tip, wingLeft, neckLeft};
    }

    std::array<std::array<ImVec2, 3>, 5> Triangles() const {
        return {{{tailLeft, tailRight, neckRight}, {tailLeft, neckRight, neckLeft},
                 {tip, wingLeft, neckLeft}, {tip, neckLeft, neckRight},
                 {tip, neckRight, wingRight}}};
    }
};

// 起终点已处于目标坐标系，scale 仅缩放线宽，避免预览和导出重复缩放位置。
std::optional<ArrowGeometry> BuildArrowGeometry(ImVec2 start, ImVec2 end,
                                               float thickness, float scale) {
    const float length = std::hypot(end.x - start.x, end.y - start.y);
    if (length <= 0.0f || scale <= 0.0f) return std::nullopt;

    const ImVec2 direction((end.x - start.x) / length, (end.y - start.y) / length);
    const ImVec2 normal(-direction.y, direction.x);
    const float naturalHeadLength = std::max(thickness * kHeadLengthPerThickness, kMinHeadLength) * scale;
    // 短箭头等比缩小头部与颈部，防止翼角越过起点、轮廓发生自交。
    const float headLength = std::min(naturalHeadLength, length * kMaxHeadLengthFraction);
    const float headScale = headLength / naturalHeadLength;
    const float headHalfWidth = headLength * kHeadWidthToLength * 0.5f;
    const float neckHalfWidth = std::max(thickness * kShaftWidthPerThickness, kMinShaftWidth)
                                * scale * headScale * 0.5f;
    const float tailHalfWidth = std::min(kTailWidth * scale * 0.5f, neckHalfWidth);
    const ImVec2 wingCenter(end.x - direction.x * headLength, end.y - direction.y * headLength);
    // 翼角略向后伸出，颈部朝尖端内收，形成参考图中带倒钩的头部。
    const float neckDistance = headLength * (1.0f - kHeadInsetRatio);
    const ImVec2 neckCenter(end.x - direction.x * neckDistance, end.y - direction.y * neckDistance);
    return ArrowGeometry{
        ImVec2(start.x + normal.x * tailHalfWidth, start.y + normal.y * tailHalfWidth),
        ImVec2(start.x - normal.x * tailHalfWidth, start.y - normal.y * tailHalfWidth),
        ImVec2(neckCenter.x + normal.x * neckHalfWidth, neckCenter.y + normal.y * neckHalfWidth),
        ImVec2(neckCenter.x - normal.x * neckHalfWidth, neckCenter.y - normal.y * neckHalfWidth),
        ImVec2(wingCenter.x + normal.x * headHalfWidth, wingCenter.y + normal.y * headHalfWidth),
        ImVec2(wingCenter.x - normal.x * headHalfWidth, wingCenter.y - normal.y * headHalfWidth),
        end};
}
} // namespace

// ========== ArrowLayer ==========
ArrowLayer::ArrowLayer(ImVec2 start, ImVec2 end, ImU32 color, float thickness)
    : Layer(color, thickness), m_start(start), m_end(end) {}

ImRect ArrowLayer::GetBounds() const {
    ImVec2 minP(std::min(m_start.x, m_end.x), std::min(m_start.y, m_end.y));
    ImVec2 maxP(std::max(m_start.x, m_end.x), std::max(m_start.y, m_end.y));
    const float margin = std::max(m_thickness * kBoundsMarginPerThickness, kMinBoundsMargin);
    return ImRect(minP.x - margin, minP.y - margin,
                  maxP.x + margin, maxP.y + margin);
}

void ArrowLayer::Render(ImDrawList* drawList, const ImVec2& canvasOffset, float uiScale) {
    ImVec2 start(m_start.x + canvasOffset.x, m_start.y + canvasOffset.y);
    ImVec2 end(m_end.x + canvasOffset.x, m_end.y + canvasOffset.y);
    if (const auto geometry = BuildArrowGeometry(start, end, m_thickness, uiScale)) {
        const auto outline = geometry->Outline();
        // 整体填充凹多边形，避免箭杆与头部分开抗锯齿时在连接处出现细缝。
        drawList->AddConcavePolyFilled(outline.data(), static_cast<int>(outline.size()), m_color);
    }

    // 绘制过程中的手柄：两个白色小圆点 + 1px 连线
    if (m_showHandles) {
        ImU32 whiteColor = Colors::SelectedBorder;
        DrawDashedLine(drawList, start, end, whiteColor, 1.0f * uiScale, 5.0f * uiScale, 3.0f * uiScale);
        float radius = 4.0f * uiScale;
        drawList->AddCircleFilled(start, radius, Colors::HandleFill);
        drawList->AddCircle(start, radius, Colors::HandleBorder, 0, 1.0f * uiScale);
        drawList->AddCircleFilled(end, radius, Colors::HandleFill);
        drawList->AddCircle(end, radius, Colors::HandleBorder, 0, 1.0f * uiScale);
    }
}

void ArrowLayer::RenderToImage(unsigned char* imageData, int imgWidth, int imgHeight,
                               int rowPitch, const ImVec2& canvasOffset, float scaleFactor) {
    const ImVec2 start(m_start.x * scaleFactor - canvasOffset.x, m_start.y * scaleFactor - canvasOffset.y);
    const ImVec2 end(m_end.x * scaleFactor - canvasOffset.x, m_end.y * scaleFactor - canvasOffset.y);
    const auto geometry = BuildArrowGeometry(start, end, m_thickness, scaleFactor);
    if (!geometry) return;

    for (const auto& triangle : geometry->Triangles()) {
        FillTriangleToImage(imageData, imgWidth, imgHeight, rowPitch,
                            triangle[0], triangle[1], triangle[2], m_color);
    }
}

std::unique_ptr<Layer> ArrowLayer::Clone() const {
    return std::make_unique<ArrowLayer>(m_start, m_end, m_color, m_thickness);
}

// ========== ArrowLayer 拖拽/缩放/选中 UI ==========

void ArrowLayer::StartDrag(const ImVec2& mousePos) {
    m_dragging = true;
    m_dragStartMouse = mousePos;
    m_posStartBeforeDrag = m_start;
    m_posEndBeforeDrag = m_end;
}

void ArrowLayer::UpdateDrag(const ImVec2& delta) {
    ImVec2 newStart(m_posStartBeforeDrag.x + delta.x, m_posStartBeforeDrag.y + delta.y);
    ImVec2 newEnd(m_posEndBeforeDrag.x + delta.x, m_posEndBeforeDrag.y + delta.y);
    m_start = newStart;
    m_end = newEnd;
}

void ArrowLayer::EndDrag() {
    m_dragging = false;
}

ResizeHandle ArrowLayer::HitTestResizeHandle(const ImVec2& pos) const {
    float hitRadius = 8.0f;

    // 起点手柄
    if (ImRect(ImVec2(m_start.x - hitRadius, m_start.y - hitRadius),
               ImVec2(m_start.x + hitRadius, m_start.y + hitRadius)).Contains(pos))
        return ResizeHandle::TopLeft;  // 用 TopLeft 代表起点手柄

    // 终点手柄
    if (ImRect(ImVec2(m_end.x - hitRadius, m_end.y - hitRadius),
               ImVec2(m_end.x + hitRadius, m_end.y + hitRadius)).Contains(pos))
        return ResizeHandle::BottomRight;  // 用 BottomRight 代表终点手柄

    return ResizeHandle::None;
}

void ArrowLayer::StartResize(ResizeHandle handle) {
    m_resizeHandle = handle;
    m_posStartBeforeResize = m_start;
    m_posEndBeforeResize = m_end;
    // 记录箭头手柄点（1=起点，2=终点）
    if (handle == ResizeHandle::TopLeft) m_arrowHandlePoint = 1;
    else if (handle == ResizeHandle::BottomRight) m_arrowHandlePoint = 2;
    else m_arrowHandlePoint = 0;
}

void ArrowLayer::UpdateResize(const ImVec2& delta, const ImRect& selection) {
    if (m_arrowHandlePoint == 1) {
        // 起点手柄：移动起点
        ImVec2 newStart(m_posStartBeforeResize.x + delta.x, m_posStartBeforeResize.y + delta.y);
        newStart.x = std::max(selection.Min.x, std::min(newStart.x, selection.Max.x));
        newStart.y = std::max(selection.Min.y, std::min(newStart.y, selection.Max.y));
        m_start = newStart;
    } else if (m_arrowHandlePoint == 2) {
        // 终点手柄：移动终点
        ImVec2 newEnd(m_posEndBeforeResize.x + delta.x, m_posEndBeforeResize.y + delta.y);
        newEnd.x = std::max(selection.Min.x, std::min(newEnd.x, selection.Max.x));
        newEnd.y = std::max(selection.Min.y, std::min(newEnd.y, selection.Max.y));
        m_end = newEnd;
    }
}

void ArrowLayer::EndResize() {
    m_resizeHandle = ResizeHandle::None;
    m_arrowHandlePoint = 0;
}

int ArrowLayer::HitTestArrowHandle(const ImVec2& pos) const {
    float hitRadius = 8.0f;

    if (ImRect(ImVec2(m_start.x - hitRadius, m_start.y - hitRadius),
               ImVec2(m_start.x + hitRadius, m_start.y + hitRadius)).Contains(pos))
        return 1;

    if (ImRect(ImVec2(m_end.x - hitRadius, m_end.y - hitRadius),
               ImVec2(m_end.x + hitRadius, m_end.y + hitRadius)).Contains(pos))
        return 2;

    return 0;
}

void ArrowLayer::RenderSelectionUI(ImDrawList* drawList, float uiScale) const {
    // 1px 虚线连线
    DrawDashedLine(drawList, m_start, m_end, Colors::SelectedBorder, 1.0f * uiScale, 5.0f * uiScale, 3.0f * uiScale);

    // 两个手柄圆点
    float radius = 4.0f * uiScale;
    drawList->AddCircleFilled(m_start, radius, Colors::HandleFill);
    drawList->AddCircle(m_start, radius, Colors::HandleBorder, 0, 1.0f * uiScale);
    drawList->AddCircleFilled(m_end, radius, Colors::HandleFill);
    drawList->AddCircle(m_end, radius, Colors::HandleBorder, 0, 1.0f * uiScale);
}

bool ArrowLayer::IsPointNearBorder(const ImVec2& pos, float threshold) const {
    const auto geometry = BuildArrowGeometry(m_start, m_end, m_thickness, 1.0f);
    if (!geometry) {
        return std::hypot(pos.x - m_start.x, pos.y - m_start.y) <= threshold;
    }

    // 点击范围跟随实际头部，保证加长后的翼角仍可选中，内收处空白不会误选。
    for (const auto& triangle : geometry->Triangles()) {
        if (ImTriangleContainsPoint(triangle[0], triangle[1], triangle[2], pos)) return true;
    }
    const auto outline = geometry->Outline();
    for (size_t i = 0; i < outline.size(); ++i) {
        const ImVec2 closest = ImLineClosestPoint(outline[i], outline[(i + 1) % outline.size()], pos);
        if (std::hypot(pos.x - closest.x, pos.y - closest.y) <= threshold) return true;
    }
    return false;
}


} // namespace shotlet
