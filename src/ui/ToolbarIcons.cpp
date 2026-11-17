#include "ToolbarIcons.h"
#include "utils/Colors.h"
#include "utils/DashedLine.h"
#include "utils/MaskGeometry.h"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>

namespace shotlet {
    namespace CropIconLayout {
        constexpr float OuterInset = 0.15f;
        constexpr float InnerInset = 0.30f;
        constexpr float InnerEnd = 1.0f - InnerInset;
        constexpr float OuterEnd = 1.0f - OuterInset;
    }

    void ToolbarIcons::DrawCropIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                                    ImU32 color, float thickness) {
        // 按参考图使用两条交错直角线，延伸的手柄与其他工具共用尺寸、颜色和线宽。
        const auto point = [&](float x, float y) { return ImVec2(p_min.x + iconSize * x, p_min.y + iconSize * y); };
        const ImVec2 lowerCorner[] = {
            point(CropIconLayout::InnerInset, CropIconLayout::OuterInset),
            point(CropIconLayout::InnerInset, CropIconLayout::InnerEnd),
            point(CropIconLayout::OuterEnd, CropIconLayout::InnerEnd)
        };
        const ImVec2 upperCorner[] = {
            point(CropIconLayout::OuterInset, CropIconLayout::InnerInset),
            point(CropIconLayout::InnerEnd, CropIconLayout::InnerInset),
            point(CropIconLayout::InnerEnd, CropIconLayout::OuterEnd)
        };
        drawList->AddPolyline(lowerCorner, IM_ARRAYSIZE(lowerCorner), color, ImDrawFlags_None, thickness);
        drawList->AddPolyline(upperCorner, IM_ARRAYSIZE(upperCorner), color, ImDrawFlags_None, thickness);
    }

    constexpr float kPi = std::numbers::pi_v<float>;
    static ImVec2 PointTowards(const ImVec2 from, const ImVec2 to, const float distance) {
        const float dx = to.x - from.x;
        const float dy = to.y - from.y;
        const float len = sqrtf(dx * dx + dy * dy);
        if (len <= 0.0f) {
            return from;
        }

        const float t = std::min(distance / len, 0.45f);
        return ImVec2(from.x + dx * t, from.y + dy * t);
    }

    void ToolbarIcons::DrawIcon(ImDrawList *drawList, const ImVec2 &p_min, const float iconSize,
                                const IconType iconType, const ImU32 color, const float thickness) {
        switch (iconType) {
            case IconType::Speaker:
            case IconType::Microphone:
            case IconType::Camera:
            case IconType::Cursor: {
                constexpr float Inset = .16f, Near = .32f, Center = .5f, Far = .68f, End = 1 - Inset;
                constexpr float Rounding = .12f, SoundRadius = .25f, CursorTail = .74f;
                const auto at = [&](float x, float y) { return ImVec2(p_min.x + iconSize * x, p_min.y + iconSize * y); };
                if (iconType == IconType::Microphone) {
                    drawList->AddRect(at(Near, Inset), at(Far, Far), color, iconSize * Rounding, 0, thickness);
                    drawList->AddBezierCubic(at(Inset, Center), at(Inset, 1), at(End, 1), at(End, Center), color, thickness);
                    drawList->AddLine(at(Center, End), at(Center, 1), color, thickness);
                } else if (iconType == IconType::Speaker) {
                    const ImVec2 body[]{at(Inset, Near), at(Near, Near), at(Center, Inset), at(Center, End), at(Near, Far), at(Inset, Far)};
                    drawList->AddPolyline(body, IM_ARRAYSIZE(body), color, ImDrawFlags_Closed, thickness);
                    drawList->PathArcTo(at(Center, Center), iconSize * SoundRadius, -kPi / 3, kPi / 3);
                    drawList->PathStroke(color, 0, thickness);
                } else if (iconType == IconType::Camera) {
                    drawList->AddRect(at(Inset, Near), at(Far, End), color, iconSize * Rounding, 0, thickness);
                    const ImVec2 lens[]{at(Far, Center), at(End, Near), at(End, End), at(Far, Far)};
                    drawList->AddPolyline(lens, IM_ARRAYSIZE(lens), color, ImDrawFlags_Closed, thickness);
                } else {
                    const ImVec2 cursor[]{at(Inset, Inset), at(End, Center), at(Center, Center), at(CursorTail, End),
                        at(Center, 1), at(Near, Far), at(Inset, End)};
                    drawList->AddPolyline(cursor, IM_ARRAYSIZE(cursor), color, ImDrawFlags_Closed, thickness);
                }
                break;
            }
            case IconType::Record: {
                constexpr float FrameInsetRatio = 0.12f;
                constexpr float CornerRadiusRatio = 0.10f;
                constexpr float RecordRadiusRatio = 0.17f;
                const ImVec2 center(p_min.x + iconSize / 2, p_min.y + iconSize / 2);
                const float inset = iconSize * FrameInsetRatio;
                drawList->AddRect(ImVec2(p_min.x + inset, p_min.y + inset),
                    ImVec2(p_min.x + iconSize - inset, p_min.y + iconSize - inset), color,
                    iconSize * CornerRadiusRatio, 0, thickness);
                drawList->AddCircleFilled(center, iconSize * RecordRadiusRatio, color);
                break;
            }
            case IconType::Highlight: {
                constexpr float insetRatio = 0.15f;
                constexpr float roundingRatio = 0.10f;
                constexpr float holeRadiusRatio = 0.20f;
                constexpr int holeSegments = 32;
                const ImVec2 center(p_min.x + iconSize / 2, p_min.y + iconSize / 2);
                // 与矩形图标共用 15% 外侧留白，圆角和中孔随实际绘制边长等比例缩小。
                const float inset = iconSize * insetRatio;
                const float bodySize = iconSize - 2 * inset;
                const float radius = bodySize * holeRadiusRatio;
                auto outer = MaskGeometry::RoundedRectangle(ImRect(p_min.x + inset, p_min.y + inset,
                    p_min.x + iconSize - inset, p_min.y + iconSize - inset), bodySize * roundingRatio);
                auto hole = MaskGeometry::Ellipse(ImRect(center.x - radius, center.y - radius,
                    center.x + radius, center.y + radius), holeSegments);
                MaskGeometry::Draw(drawList, MaskGeometry::SubtractHoles(std::move(outer), {hole}), ImVec2(0, 0), color);
                break;
            }
            case IconType::Crop:
                DrawCropIcon(drawList, p_min, iconSize, color, thickness);
                break;
            case IconType::ZoomOut:
            case IconType::ZoomIn: {
                // 与矩形等图标共用 15% 留白和 70% 绘制范围，圆弧采样密度与椭圆一致。
                const ImVec2 center(p_min.x + iconSize * 0.43f, p_min.y + iconSize * 0.43f);
                const float radius = iconSize * 0.28f;
                drawList->AddCircle(center, radius, color, 128, thickness);
                // AddCircle 的路径半径会内缩 0.5，手柄从同一轮廓接出，避免接缝堆叠。
                const float handleOffset = std::max(0.0f, radius - 0.5f) / std::sqrt(2.0f);
                const ImVec2 handle[] = {
                    ImVec2(center.x + handleOffset, center.y + handleOffset),
                    ImVec2(p_min.x + iconSize * 0.85f, p_min.y + iconSize * 0.85f)
                };
                drawList->AddPolyline(handle, 2, color, ImDrawFlags_None, thickness);
                drawList->AddLine(ImVec2(center.x - iconSize * 0.10f, center.y),
                                  ImVec2(center.x + iconSize * 0.10f, center.y), color, thickness);
                if (iconType == IconType::ZoomIn)
                    drawList->AddLine(ImVec2(center.x, center.y - iconSize * 0.10f),
                                      ImVec2(center.x, center.y + iconSize * 0.10f), color, thickness);
                break;
            }
            case IconType::ActualSize: {
                DrawRectangleIcon(drawList, p_min, iconSize, color, thickness);
                for (float x : {0.32f, 0.68f})
                    drawList->AddLine(ImVec2(p_min.x + iconSize * x, p_min.y + iconSize * 0.37f),
                                      ImVec2(p_min.x + iconSize * x, p_min.y + iconSize * 0.63f), color, thickness);
                for (float y : {0.43f, 0.57f})
                    drawList->AddCircleFilled(ImVec2(p_min.x + iconSize * 0.50f, p_min.y + iconSize * y), thickness * 0.65f, color, 16);
                break;
            }
            case IconType::Rotate: {
                drawList->AddRect(ImVec2(p_min.x + iconSize * 0.15f, p_min.y + iconSize * 0.43f),
                                  ImVec2(p_min.x + iconSize * 0.75f, p_min.y + iconSize * 0.85f), color, 0, 0, thickness);
                const ImVec2 center(p_min.x + iconSize * 0.60f, p_min.y + iconSize * 0.43f);
                const float radius = iconSize * 0.22f;
                const ImVec2 tip(center.x - iconSize * 0.08f, center.y - radius);
                // 箭杆和弧线使用连续路径，四分之一圆用 32 段保持与 128 段整圆相同的精度。
                drawList->PathLineTo(tip);
                drawList->PathArcTo(center, radius, -kPi / 2, 0, 32);
                drawList->PathStroke(color, ImDrawFlags_None, thickness);
                const ImVec2 arrowHead[] = {
                    ImVec2(center.x, tip.y - iconSize * 0.06f),
                    tip,
                    ImVec2(center.x, tip.y + iconSize * 0.06f)
                };
                drawList->AddPolyline(arrowHead, 3, color, ImDrawFlags_None, thickness);
                break;
            }
            case IconType::Rectangle:
                DrawRectangleIcon(drawList, p_min, iconSize, color, thickness);
                break;
            case IconType::Circle:
                DrawCircleIcon(drawList, p_min, iconSize, color, thickness);
                break;
            case IconType::Arrow:
                DrawArrowIcon(drawList, p_min, iconSize, color, thickness);
                break;
            case IconType::Brush:
                DrawBrushIcon(drawList, p_min, iconSize, color, thickness);
                break;
            case IconType::Mosaic:
                DrawMosaicIcon(drawList, p_min, iconSize, color);
                break;
            case IconType::Watermark:
                DrawWatermarkIcon(drawList, p_min, iconSize, color);
                break;
            case IconType::Text:
                DrawTextIcon(drawList, p_min, iconSize, color, thickness);
                break;
            case IconType::Undo:
                DrawUndoIcon(drawList, p_min, iconSize, color, thickness);
                break;
            case IconType::Number:
                DrawNumberIcon(drawList, p_min, iconSize, color, thickness);
                break;
            case IconType::Close:
                DrawCloseIcon(drawList, p_min, iconSize, color, thickness);
                break;
            case IconType::Ok:
                DrawOkIcon(drawList, p_min, iconSize, color, thickness);
                break;
            case IconType::Save:
                DrawSaveIcon(drawList, p_min, iconSize, color, thickness);
                break;
            case IconType::Pin:
                DrawPinIcon(drawList, p_min, iconSize, color, thickness);
                break;
            case IconType::Ocr:
                DrawOcrIcon(drawList, p_min, iconSize, color, thickness);
                break;
            case IconType::Scroll:
                DrawScrollIcon(drawList, p_min, iconSize, color, thickness);
                break;
            case IconType::Edit:
                DrawEditIcon(drawList, p_min, iconSize, color, thickness);
                break;
        }
    }

    void ToolbarIcons::DrawRectangleIcon(ImDrawList *drawList, const ImVec2 &p_min, const float iconSize,
                                         const ImU32 color, const float thickness) {
        const auto p_max = ImVec2(p_min.x + iconSize, p_min.y + iconSize);
        const ImVec2 rectMin(p_min.x + iconSize * 0.15f, p_min.y + iconSize * 0.15f);
        const ImVec2 rectMax(p_max.x - iconSize * 0.15f, p_max.y - iconSize * 0.15f);
        drawList->AddRect(rectMin, rectMax, color, 0.0f, 0, thickness);
    }

    void ToolbarIcons::DrawCircleIcon(ImDrawList *drawList, const ImVec2 &p_min, const float iconSize,
                                      const ImU32 color, const float thickness) {
        const ImVec2 center(p_min.x + iconSize / 2, p_min.y + iconSize / 2);
        const float radius = (iconSize * 0.75f) / 2;
        drawList->AddCircle(center, radius, color, 128, thickness);
    }

    void ToolbarIcons::DrawArrowIcon(ImDrawList *drawList, const ImVec2 &p_min, const float iconSize,
                                     const ImU32 color, const float thickness) {
        // 箭杆直线，从左下到右上角
        const ImVec2 start(p_min.x + iconSize * 0.2f, p_min.y + iconSize * 0.8f);
        const ImVec2 end(p_min.x + iconSize * 0.8f, p_min.y + iconSize * 0.2f);
        drawList->AddLine(start, end, color, thickness);

        // 箭头头部（两条边各与箭杆成45°角，即水平线和垂直线）
        const float arrowSize = iconSize * 0.4f;
        const float angle = atan2f(end.y - start.y, end.x - start.x);
        const ImVec2 arrow1(end.x + arrowSize * cosf(angle + kPi * 0.75f),
                      end.y + arrowSize * sinf(angle + kPi * 0.75f));
        const ImVec2 arrow2(end.x + arrowSize * cosf(angle - kPi * 0.75f),
                      end.y + arrowSize * sinf(angle - kPi * 0.75f));
        drawList->AddLine(end, arrow1, color, thickness);
        drawList->AddLine(end, arrow2, color, thickness);
    }

    void ToolbarIcons::DrawBrushIcon(ImDrawList *drawList, const ImVec2 &p_min, const float iconSize,
                                     const ImU32 color, const float thickness) {
        constexpr float kSvgViewportSize = 24.0f;
        // 原始轮廓占 viewBox 约 83%，缩小后与矩形等图标约 70% 的绘制范围接近。
        constexpr float kBrushContentScale = 0.85f;
        constexpr ImVec2 kCapArc[] = {
            {21.174f, 6.812f},
            {22.274980f, 5.711296f}, {22.275204f, 3.926480f}, {21.174500f, 2.825500f},
            {20.073796f, 1.724520f}, {18.288980f, 1.724296f}, {17.188f, 2.825f}
        };
        constexpr ImVec2 kLeftShoulderArc[] = {
            {3.842f, 16.174f},
            {3.609819f, 16.405500f}, {3.438114f, 16.690532f}, {3.342f, 17.004f}
        };
        constexpr ImVec2 kTipArc[] = {
            {2.021f, 21.356f},
            {1.968345f, 21.532195f}, {2.016659f, 21.723074f}, {2.146796f, 21.853002f},
            {2.276934f, 21.982931f}, {2.467890f, 22.030938f}, {2.644f, 21.978f}
        };
        constexpr ImVec2 kRightShoulderArc[] = {
            {6.997f, 20.658f},
            {7.310169f, 20.562751f}, {7.595173f, 20.392092f}, {7.827f, 20.161f}
        };
        constexpr ImVec2 kDividerStart(15.0f, 5.0f);
        constexpr ImVec2 kDividerEnd(19.0f, 9.0f);
        const float svgScale = iconSize * kBrushContentScale / kSvgViewportSize;
        const float contentInset = iconSize * (1.0f - kBrushContentScale) / 2.0f;
        const auto svgPoint = [&](const ImVec2 point) {
            return ImVec2(p_min.x + contentInset + point.x * svgScale,
                          p_min.y + contentInset + point.y * svgScale);
        };
        const auto appendSvgArc = [&](const auto &arc) {
            drawList->PathLineTo(svgPoint(arc[0]));
            for (size_t i = 1; i < std::size(arc); i += 3) {
                drawList->PathBezierCubicCurveTo(svgPoint(arc[i]), svgPoint(arc[i + 1]), svgPoint(arc[i + 2]));
            }
        };

        appendSvgArc(kCapArc);
        appendSvgArc(kLeftShoulderArc);
        appendSvgArc(kTipArc);
        appendSvgArc(kRightShoulderArc);
        drawList->PathStroke(color, ImDrawFlags_Closed, thickness);

        const ImVec2 divider[] = {svgPoint(kDividerStart), svgPoint(kDividerEnd)};
        drawList->AddPolyline(divider, 2, color, ImDrawFlags_None, thickness);
        // ImGui 折线没有圆形端帽，补上半线宽圆点
        const float capRadius = thickness / 2.0f;
        drawList->AddCircleFilled(divider[0], capRadius, color);
        drawList->AddCircleFilled(divider[1], capRadius, color);
    }

    void ToolbarIcons::DrawMosaicIcon(ImDrawList *drawList, const ImVec2 &p_min, const float iconSize,
                                      const ImU32 color) {
        // 绘制 1px 正方形边框
        drawList->AddRect(
            ImVec2(p_min.x + iconSize * 0.15f, p_min.y + iconSize * 0.15f),
            ImVec2(p_min.x + iconSize * 0.85f, p_min.y + iconSize * 0.85f),
            color, 0.0f, 0, 1.0f);

        // 绘制马赛克（5x5 黑白棋盘格）
        constexpr ImU32 blackColor = Colors::MosaicBlack;
        constexpr ImU32 whiteColorMosaic = Colors::MosaicWhite;
        const float innerLeft = p_min.x + iconSize * 0.3f;
        const float innerTop = p_min.y + iconSize * 0.3f;
        const float innerRight = p_min.x + iconSize * 0.7f;
        const float innerBottom = p_min.y + iconSize * 0.7f;
        const float cellW = (innerRight - innerLeft) / 5.0f;
        const float cellH = (innerBottom - innerTop) / 5.0f;

        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 5; col++) {
                const bool isBlack = (row + col) % 2 != 0;
                const ImU32 cellColor = isBlack ? blackColor : whiteColorMosaic;
                ImVec2 cellMin(innerLeft + col * cellW, innerTop + row * cellH);
                ImVec2 cellMax(cellMin.x + cellW, cellMin.y + cellH);
                drawList->AddRectFilled(cellMin, cellMax, cellColor);
            }
        }
    }

    void ToolbarIcons::DrawWatermarkIcon(ImDrawList* drawList, const ImVec2& p_min, const float iconSize,
                                         const ImU32 color) {
        // 原 SVG viewBox 为 1024，6 条路径均为方向 (1,-1) 的斜线。
        // 此处换算为归一化轴线端点，绘制范围 0.16~0.86，与其他图标的留白一致。
        constexpr ImVec2 kLines[][2] = {
            {{0.1643f, 0.2876f}, {0.2815f, 0.1704f}},
            {{0.6669f, 0.8358f}, {0.8297f, 0.7186f}},
            {{0.1639f, 0.5638f}, {0.5577f, 0.1699f}},
            {{0.4363f, 0.8362f}, {0.8301f, 0.4424f}},
            {{0.1661f, 0.8340f}, {0.4460f, 0.5540f}},
            {{0.5509f, 0.4491f}, {0.8308f, 0.1692f}},
        };
        const float stroke = iconSize * (2.0f * 33.0f / 1024.0f);
        for (const auto& line : kLines) {
            const ImVec2 from(p_min.x + line[0].x * iconSize, p_min.y + line[0].y * iconSize);
            const ImVec2 to(p_min.x + line[1].x * iconSize, p_min.y + line[1].y * iconSize);
            // 仅绘制直线，使用平齐端点，不再叠加圆形端帽。
            drawList->AddLine(from, to, color, stroke);
        }
    }

    void ToolbarIcons::DrawTextIcon(ImDrawList *drawList, const ImVec2 &p_min, const float iconSize,
                                    const ImU32 color, const float thickness) {
        // 绘制 1px 正方形边框
        drawList->AddRect(
            ImVec2(p_min.x + iconSize * 0.15f, p_min.y + iconSize * 0.15f),
            ImVec2(p_min.x + iconSize * 0.85f, p_min.y + iconSize * 0.85f),
            color, 0.0f, 0, 1.0f);

        // 绘制缩小版的 T 字母
        // T 的顶部横线
        drawList->AddLine(
            ImVec2(p_min.x + iconSize * 0.3f, p_min.y + iconSize * 0.3f),
            ImVec2(p_min.x + iconSize * 0.7f, p_min.y + iconSize * 0.3f),
            color, thickness);
        // T 的垂直线
        drawList->AddLine(
            ImVec2(p_min.x + iconSize * 0.5f, p_min.y + iconSize * 0.3f),
            ImVec2(p_min.x + iconSize * 0.5f, p_min.y + iconSize * 0.7f),
            color, thickness);
    }

    void ToolbarIcons::DrawNumberIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                                     ImU32 color, float thickness) {
        const ImVec2 center(p_min.x + iconSize * 0.5f, p_min.y + iconSize * 0.5f);
        drawList->AddCircle(center, iconSize * 0.35f, color, 0, thickness);
        // 数字 1 使用线段绘制，竖线和底部横线中心对齐圆心，不依赖当前字体。
        drawList->AddLine(ImVec2(center.x - iconSize * 0.10f, center.y - iconSize * 0.09f),
                          ImVec2(center.x, center.y - iconSize * 0.17f), color, thickness);
        drawList->AddLine(ImVec2(center.x, center.y - iconSize * 0.17f),
                          ImVec2(center.x, center.y + iconSize * 0.17f), color, thickness);
        drawList->AddLine(ImVec2(center.x - iconSize * 0.10f, center.y + iconSize * 0.17f),
                          ImVec2(center.x + iconSize * 0.10f, center.y + iconSize * 0.17f), color, thickness);
    }

    void ToolbarIcons::DrawUndoIcon(ImDrawList *drawList, const ImVec2 &p_min, const float iconSize,
                                    const ImU32 color, const float thickness) {
        const float size = iconSize;

        // 基础参数：半圆半径和中心点
        const float radius = size * 0.25f;
        const auto center = ImVec2(p_min.x + size * 0.55f, p_min.y + size * 0.5f);

        // 半圆弧底部起点和顶部终点
        const auto arcBottom = ImVec2(center.x, center.y + radius);
        const auto arcTop = ImVec2(center.x, center.y - radius);

        // 用一条连续路径绘制：底部横线 → 半圆弧 → 上方横线，衔接无缝
        drawList->PathLineTo(ImVec2(p_min.x + size * 0.1f, arcBottom.y));

        // 半圆弧：从 90°（底部）到 -90°（顶部），顺时针
        drawList->PathArcTo(center, radius, kPi * 0.5f, -kPi * 0.5f);

        // 上方横线：从圆弧顶部终点向左水平延伸到箭头尖端
        const float arrowSize = size * 0.18f;
        const float shaftLen = size * 0.25f;  // 短线长度
        const auto  shaftEnd = ImVec2(arcTop.x - shaftLen, arcTop.y);  // 水平短线末端
        drawList->PathLineTo(shaftEnd);
        drawList->PathStroke(color, 0, thickness);

        // 箭头：从短线末端出发，两条边向右展开各成45°夹角（一条右上，一条右下）
        const ImVec2 arrow1(shaftEnd.x + arrowSize, shaftEnd.y - arrowSize);
        const ImVec2 arrow2(shaftEnd.x + arrowSize, shaftEnd.y + arrowSize);
        drawList->AddLine(shaftEnd, arrow1, color, thickness);
        drawList->AddLine(shaftEnd, arrow2, color, thickness);
    }

    void ToolbarIcons::DrawCloseIcon(ImDrawList *drawList, const ImVec2 &p_min, const float iconSize,
                                     const ImU32 color, const float thickness) {
        // 左上到右下线段
        drawList->AddLine(ImVec2(p_min.x + iconSize * 0.2f, p_min.y + iconSize * 0.2f),
                          ImVec2(p_min.x + iconSize * 0.8f, p_min.y + iconSize * 0.8f), color, thickness);
        // 右上到左下线段
        drawList->AddLine(ImVec2(p_min.x + iconSize * 0.8f, p_min.y + iconSize * 0.2f),
                          ImVec2(p_min.x + iconSize * 0.2f, p_min.y + iconSize * 0.8f), color, thickness);
    }

    void ToolbarIcons::DrawOkIcon(ImDrawList *drawList, const ImVec2 &p_min, const float iconSize,
                                  const ImU32 color, const float thickness) {
        const float sz = iconSize;
        const float ox = p_min.x;
        const float oy = p_min.y;
        // 勾形：从左上到右下拐点，再到右上
        drawList->AddLine(ImVec2(ox + sz * 0.1f, oy + sz * 0.5f),
                          ImVec2(ox + sz * 0.35f, oy + sz * 0.75f), color, thickness);
        drawList->AddLine(ImVec2(ox + sz * 0.35f, oy + sz * 0.75f),
                          ImVec2(ox + sz * 0.85f, oy + sz * 0.2f), color, thickness);
    }

    void ToolbarIcons::DrawSaveIcon(ImDrawList *drawList, const ImVec2 &p_min, const float iconSize,
                                     const ImU32 color, const float thickness) {
        const float cx = p_min.x + iconSize * 0.5f;
        const float boxLeft = p_min.x + iconSize * 0.20f;
        const float boxRight = p_min.x + iconSize * 0.80f;
        const float boxTop = p_min.y + iconSize * 0.22f;
        const float boxBottom = p_min.y + iconSize * 0.80f;
        const float topGapHalfWidth = iconSize * 0.15f;
        const float arrowTop = p_min.y + iconSize * 0.05f;
        const float arrowBottom = p_min.y + iconSize * 0.55f;
        const float arrowShoulderY = p_min.y + iconSize * 0.4f;
        const float arrowHalfWidth = iconSize * 0.12f;

        // 顶部边框中间留半边宽度缺口
        drawList->AddLine(ImVec2(boxLeft, boxTop),
                          ImVec2(cx - topGapHalfWidth, boxTop), color, thickness);
        drawList->AddLine(ImVec2(cx + topGapHalfWidth, boxTop),
                          ImVec2(boxRight, boxTop), color, thickness);
        drawList->AddLine(ImVec2(boxLeft, boxTop),
                          ImVec2(boxLeft, boxBottom), color, thickness);
        drawList->AddLine(ImVec2(boxLeft, boxBottom),
                          ImVec2(boxRight, boxBottom), color, thickness);
        drawList->AddLine(ImVec2(boxRight, boxBottom),
                          ImVec2(boxRight, boxTop), color, thickness);

        // 向下箭头，箭杆从方框顶部开口伸出
        drawList->AddLine(ImVec2(cx, arrowTop),
                          ImVec2(cx, arrowBottom), color, thickness);
        drawList->AddLine(ImVec2(cx - arrowHalfWidth, arrowShoulderY),
                          ImVec2(cx, arrowBottom), color, thickness);
        drawList->AddLine(ImVec2(cx + arrowHalfWidth, arrowShoulderY),
                          ImVec2(cx, arrowBottom), color, thickness);
    }

    void ToolbarIcons::DrawEditIcon(ImDrawList* drawList, const ImVec2& p, float size,
                                    ImU32 color, float thickness) {
        const float x = p.x, y = p.y;
        drawList->AddLine({x + size * 0.15f, y + size * 0.18f},
                          {x + size * 0.15f, y + size * 0.82f}, color, thickness);
        drawList->AddLine({x + size * 0.15f, y + size * 0.82f},
                          {x + size * 0.82f, y + size * 0.82f}, color, thickness);
        drawList->AddLine({x + size * 0.82f, y + size * 0.82f},
                          {x + size * 0.82f, y + size * 0.57f}, color, thickness);
        drawList->AddLine({x + size * 0.15f, y + size * 0.18f},
                          {x + size * 0.49f, y + size * 0.18f}, color, thickness);
        drawList->AddLine({x + size * 0.43f, y + size * 0.57f},
                          {x + size * 0.83f, y + size * 0.17f}, color, thickness * 1.2f);
    }

    void ToolbarIcons::DrawPinIcon(ImDrawList* drawList, const ImVec2& p_min, const float iconSize,
                                const ImU32 color, const float thickness) {
        // 推杆图钉图标：帽70% + 竖杆30%
        // 帽子保留 6 个关键角点，仅给 4 个外凸角加很小圆角；中间收腰凹角保持尖锐。

        const float s = iconSize * 0.68f;
        const float ox = p_min.x + iconSize * 0.16f;
        const float oy = p_min.y + iconSize * 0.16f;

        // 帽轮廓（顺时针）：顶部窄横条 → 中部收腰 → 底部宽横条
        const ImVec2 head[] = {
            ImVec2(ox + s * 0.25f, oy + s * 0.00f),   // 0: 顶部左端
            ImVec2(ox + s * 0.75f, oy + s * 0.00f),   // 1: 顶部右端
            ImVec2(ox + s * 0.62f, oy + s * 0.25f),   // 2: 右侧收腰
            ImVec2(ox + s * 0.90f, oy + s * 0.70f),   // 3: 底部右端
            ImVec2(ox + s * 0.10f, oy + s * 0.70f),   // 4: 底部左端
            ImVec2(ox + s * 0.38f, oy + s * 0.25f),   // 5: 左侧收腰
        };

        const float cornerRadius = s * 0.08f;
        const ImVec2 p0Out = PointTowards(head[0], head[1], cornerRadius);
        const ImVec2 p1In = PointTowards(head[1], head[0], cornerRadius);
        const ImVec2 p1Out = PointTowards(head[1], head[2], cornerRadius);
        const ImVec2 p3In = PointTowards(head[3], head[2], cornerRadius);
        const ImVec2 p3Out = PointTowards(head[3], head[4], cornerRadius);
        const ImVec2 p4In = PointTowards(head[4], head[3], cornerRadius);
        const ImVec2 p4Out = PointTowards(head[4], head[5], cornerRadius);
        const ImVec2 p0In = PointTowards(head[0], head[5], cornerRadius);

        drawList->PathLineTo(p0Out);
        drawList->PathLineTo(p1In);
        drawList->PathBezierQuadraticCurveTo(head[1], p1Out, 3);
        drawList->PathLineTo(head[2]);
        drawList->PathLineTo(p3In);
        drawList->PathBezierQuadraticCurveTo(head[3], p3Out, 3);
        drawList->PathLineTo(p4In);
        drawList->PathBezierQuadraticCurveTo(head[4], p4Out, 3);
        drawList->PathLineTo(head[5]);
        drawList->PathLineTo(p0In);
        drawList->PathBezierQuadraticCurveTo(head[0], p0Out, 3);
        drawList->PathStroke(color, 0, thickness);

        // 竖杆（一条直线，占下方30%）
        const float pinCX = ox + s * 0.50f;
        drawList->AddLine(ImVec2(pinCX, oy + s * 0.70f), ImVec2(pinCX, oy + s * 1.00f), color, thickness);
    }

    void ToolbarIcons::DrawOcrIcon(ImDrawList* drawList, const ImVec2& p_min, const float iconSize,
                                     const ImU32 color, const float thickness) {
        // 断开的方形边框 + 大写字母 A
        // 边框比例与 TextIcon 的 T 相同（0.15~0.85），每条边中间断开 2/5 边长。
        // 这里不知道为什么视觉上看起来比文本工具要大，所以调整为(0.18 ~ 0.82)比例
        const float left = p_min.x + iconSize * 0.18f;
        const float top = p_min.y + iconSize * 0.18f;
        const float right = p_min.x + iconSize * 0.82f;
        const float bottom = p_min.y + iconSize * 0.82f;
        const float side = right - left;
        const float segment = side * 0.3f;

        drawList->AddLine(ImVec2(left, top), ImVec2(left + segment, top), color, thickness);
        drawList->AddLine(ImVec2(right - segment, top), ImVec2(right, top), color, thickness);
        drawList->AddLine(ImVec2(right, top), ImVec2(right, top + segment), color, thickness);
        drawList->AddLine(ImVec2(right, bottom - segment), ImVec2(right, bottom), color, thickness);
        drawList->AddLine(ImVec2(right, bottom), ImVec2(right - segment, bottom), color, thickness);
        drawList->AddLine(ImVec2(left + segment, bottom), ImVec2(left, bottom), color, thickness);
        drawList->AddLine(ImVec2(left, bottom), ImVec2(left, bottom - segment), color, thickness);
        drawList->AddLine(ImVec2(left, top + segment), ImVec2(left, top), color, thickness);

        // A 左腿：从左下到顶部中心，整体略向内收
        drawList->AddLine(
            ImVec2(p_min.x + iconSize * 0.3f, p_min.y + iconSize * 0.7f),
            ImVec2(p_min.x + iconSize * 0.5f, p_min.y + iconSize * 0.3f),
            color, thickness);
        // A 右腿：从顶部中心到右下，整体略向内收
        drawList->AddLine(
            ImVec2(p_min.x + iconSize * 0.5f, p_min.y + iconSize * 0.3f),
            ImVec2(p_min.x + iconSize * 0.7f, p_min.y + iconSize * 0.7f),
            color, thickness);
        // A 横杠
        drawList->AddLine(
            ImVec2(p_min.x + iconSize * 0.4f, p_min.y + iconSize * 0.55f),
            ImVec2(p_min.x + iconSize * 0.6f, p_min.y + iconSize * 0.55f),
            color, thickness);
    }

    void ToolbarIcons::DrawScrollIcon(ImDrawList *drawList, const ImVec2 &p_min, const float iconSize,
                                      const ImU32 color, const float thickness) {
        // 坐标比例基于 25px 原始设计稿换算，纵向范围 0.15~0.85 与矩形/文字图标保持视觉一致
        const float ox = p_min.x;
        const float oy = p_min.y;
        const float left = ox + iconSize * 0.2f;
        const float right = ox + iconSize * 0.8f;
        const float top = oy + iconSize * 0.15f;
        const float bottom = oy + iconSize * 0.85f;

        // 上、下实线边框
        drawList->AddLine(ImVec2(left, top), ImVec2(right, top), color, thickness);
        drawList->AddLine(ImVec2(left, bottom), ImVec2(right, bottom), color, thickness);

        // 左右虚线边框：dash/gap 各 0.10，复用 DrawDashedLine
        const float dashLen = iconSize * 0.10f;
        DrawDashedLine(drawList, ImVec2(left, top), ImVec2(left, bottom), color, thickness, dashLen, dashLen);
        DrawDashedLine(drawList, ImVec2(right, top), ImVec2(right, bottom), color, thickness, dashLen, dashLen);

        // 箭杆与两个箭头共用外框中心，保证左右对称。
        const float centerX = (left + right) * 0.5f;
        const float arrowHalfWidth = iconSize * 0.12f;
        const float arrowTop = oy + iconSize * 0.25f;
        const float arrowBottom = oy + iconSize * 0.75f;

        // 箭杆连接上下尖端，线宽与保存图标的箭头一致。
        drawList->AddLine(ImVec2(centerX, arrowTop),
                          ImVec2(centerX, arrowBottom), color, thickness);

        // 顶部向上箭头：两条斜线，比例与保存图标一致。
        drawList->AddLine(ImVec2(centerX - arrowHalfWidth, oy + iconSize * 0.40f),
                          ImVec2(centerX, arrowTop), color, thickness);
        drawList->AddLine(ImVec2(centerX + arrowHalfWidth, oy + iconSize * 0.40f),
                          ImVec2(centerX, arrowTop), color, thickness);

        // 底部向下箭头，与顶部上下对称。
        drawList->AddLine(ImVec2(centerX - arrowHalfWidth, oy + iconSize * 0.60f),
                          ImVec2(centerX, arrowBottom), color, thickness);
        drawList->AddLine(ImVec2(centerX + arrowHalfWidth, oy + iconSize * 0.60f),
                          ImVec2(centerX, arrowBottom), color, thickness);
    }
} // namespace shotlet
