// Canvas 渲染逻辑：
// 负责截图背景、暗化区域、选区边框、手柄、尺寸提示、
// 当前绘制图形、文本输入框和选中图层 UI 的绘制。
#include "Canvas.h"
#include "platform/FontProvider.h"
#include "utils/Logger.h"
#include "utils/Colors.h"
#include "utils/MaskGeometry.h"
#include "utils/GeometryUtils.h"
#include <array>
#include <cfloat>
#include <cmath>

namespace shotlet {
    namespace {
        // 水印平铺行列间距与字号成比例
        constexpr float WatermarkGapScale = 1.5f;
    }

    void Canvas::Render(ImDrawList *drawList) {
        const ImVec2 displaySize = ImGui::GetIO().DisplaySize;

        // 更新鼠标位置
        m_mousePos = ImGui::GetIO().MousePos;

        // 验证 m_selectedLayer 是否仍在图层列表中
        ValidateSelectedLayer();

        // 先同步说明文字的高度，让连接线和输入框在同一帧使用相同边界。
        ProcessTextInput();

        // ─── 光标管理 ───
        // 构建 CursorContext 并委托给 CursorManager
        if (m_cursorManager) {
            CursorContext ctx;
            ctx.mousePos = m_mousePos;
            ctx.currentTool = m_currentTool;
            ctx.hasSelection = HasSelection();
            ctx.selectionRect = GetSelectionRect();
            ctx.textInputActive = m_textInput.active;
            ctx.mouseDown = m_mouseDown;
            ctx.selectedLayer = m_selectedLayer;
            ctx.layers = &m_layerManager.GetLayers();
            ctx.selectionHandle = (!m_imageEditing && HasSelection() && !m_selectedLayer && !m_mouseDown)
                ? HitTestResizeHandle(m_mousePos) : ResizeHandle::None;
            ctx.padding = padding;
            ctx.textFrameScale = GetTextFrameScale();
            ctx.mosaicBrushRadius = m_imageEditing ? m_mosaicBrushRadius / m_uiScale : m_mosaicBrushRadius;
            ctx.uiScale = m_uiScale;
            m_lastCursorType = m_cursorManager->UpdateAndSetImGui(ctx);
        }

        // 1. 渲染截图背景
        RenderScreenshot(drawList);

        // 1.5 渲染智能区域高亮框（选区前才显示）
        if (!HasSelection() && m_smartRegion) {
            RenderSmartRegionBorder(drawList);
        }

        // 2. 渲染暗化区域（未选中部分）
        if (!m_imageEditing) RenderDimmedRegion(drawList);

        // 3. 渲染所有图层（offset=0,0 因为窗口覆盖单个屏幕）
        if (m_imageEditing) drawList->PushClipRect(ImVec2(0, 0), m_displaySize, true);
        // 高亮遮罩只压暗截图底图，已提交标注和正在绘制的预览始终位于其上方。
        RenderHighlightMask(drawList, ImVec2(0, 0), GetSelectionRect());
        m_layerManager.Render(drawList, ImVec2(0, 0), m_documentRenderScale);

        // 4. 渲染正在绘制的图形
        RenderCurrentShape(drawList);
        // 水印覆盖在全部标注之上，跟随选区与文字输入实时重绘
        RenderWatermark(drawList, ImVec2(0, 0), GetSelectionRect());
        if (m_imageEditing) drawList->PopClipRect();

        // 编辑窗口不显示截图选框；图层选择框仍正常渲染。
        if (HasSelection() && !m_imageEditing) {
            RenderSelectionBorder(drawList);
            RenderResizeHandles(drawList);
            RenderSizeTooltip(drawList);
        }

        // 6. 渲染文本输入框
        RenderTextInput();

        // 7. 渲染选中图层的边框和手柄（委托给 Layer::RenderSelectionUI）
        if (m_selectedLayer) {
            m_selectedLayer->RenderSelectionUI(drawList, m_uiScale);
        }

        // 8. 渲染自定义光标视觉（马赛克笔刷圆等）
        if (m_cursorManager) {
            CursorContext ctx;
            ctx.mousePos = m_mousePos;
            ctx.mosaicBrushRadius = m_imageEditing ? m_mosaicBrushRadius / m_uiScale : m_mosaicBrushRadius;
            ctx.uiScale = m_uiScale;
            m_cursorManager->RenderCustomVisual(drawList, ctx, m_lastCursorType);
        }
    }

    void Canvas::RenderInEditor(ImDrawList* drawList, const ImageEditorTransform& transform,
                               const ImRect& viewport) {
        ImGuiIO& io = ImGui::GetIO();
        const ImVec2 screenMouse = io.MousePos;
        io.MousePos = transform.ToImage(screenMouse);
        const ImRect imageClip = transform.ToImage(viewport);
        // 单独的命令段采用图片坐标裁剪，然后统一变换顶点和裁剪框。
        // RenderTextInput 同样绘入当前窗口的 DrawList，因此文字/光标也使用同一变换。
        drawList->PushClipRect(imageClip.Min, imageClip.Max, false);
        drawList->AddDrawCmd();
        const int firstCommand = drawList->CmdBuffer.Size - 1;
        const int firstVertex = drawList->VtxBuffer.Size;
        Render(drawList);
        const int endCommand = drawList->CmdBuffer.Size;
        for (int i = firstVertex; i < drawList->VtxBuffer.Size; ++i) {
            drawList->VtxBuffer[i].pos = transform.ToScreen(drawList->VtxBuffer[i].pos);
        }
        for (int i = firstCommand; i < endCommand; ++i) {
            ImDrawCmd& command = drawList->CmdBuffer[i];
            const ImRect clip = transform.ToScreen(ImRect(command.ClipRect));
            command.ClipRect = ImVec4(ImMax(clip.Min.x, viewport.Min.x), ImMax(clip.Min.y, viewport.Min.y),
                                     ImMin(clip.Max.x, viewport.Max.x), ImMin(clip.Max.y, viewport.Max.y));
        }
        drawList->PopClipRect();
        drawList->AddDrawCmd();
        m_imeCursorPos = transform.ToScreen(m_imeCursorPos);
        io.MousePos = screenMouse;
    }

    void Canvas::RenderClean(ImDrawList *drawList, const ImVec2& renderOffset, const ImRect* region) {
        if (!m_screenshot) return;

        // 只渲染截图和图层，不渲染选框、暗化区域、手柄等 UI 元素
        // renderOffset：导出渲染时的平移（FBORenderer 只渲染选区时传 -selectionMin）
        const auto selection = region ? *region : GetSelectionRect();
        if (HasFractionalSampling(selection)) RenderImageRegion(drawList, selection, renderOffset);
        else drawList->AddImage(m_screenshot, renderOffset, ImVec2(m_displaySize.x + renderOffset.x, m_displaySize.y + renderOffset.y),
                               ImVec2(0, 0), ImVec2(1, 1), Colors::HandleFill);
        RenderHighlightMask(drawList, renderOffset, selection);
        m_layerManager.Render(drawList, renderOffset, m_documentRenderScale);
        RenderWatermark(drawList, renderOffset, selection);
    }

    // 在未旋转的网格中逐行求可见区间，只生成真正可能进入视口的文字。
    // 网格固定在文档原点；滚动、裁剪和离屏导出只改变裁剪范围，不改变平铺相位。
    void Canvas::RenderWatermark(ImDrawList* drawList, ImVec2 offset, ImRect region) const {
        if (m_watermarkText.empty() || m_watermarkOpacity == WatermarkSettings::MinimumOpacity) return;
        ImRect visible(ImVec2(region.Min.x + offset.x, region.Min.y + offset.y),
                       ImVec2(region.Max.x + offset.x, region.Max.y + offset.y));
        visible.ClipWith(ImRect(drawList->GetClipRectMin(), drawList->GetClipRectMax()));
        if (visible.GetWidth() <= 0 || visible.GetHeight() <= 0) return;
        const auto& io = ImGui::GetIO();
        ImFont* font = io.FontDefault ? io.FontDefault : ImGui::GetFont();
        const float fontSize = GetWatermarkFontSize();
        const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, m_watermarkText.c_str());
        if (textSize.x <= 0 || textSize.y <= 0) return;

        constexpr float diagonalUnit = 0.70710678f;
        const ImVec2 origin(m_watermarkOrigin.x + offset.x, m_watermarkOrigin.y + offset.y);
        const auto toLocal = [&](ImVec2 point) {
            const float x = point.x - origin.x, y = point.y - origin.y;
            return ImVec2((x - y) * diagonalUnit, (x + y) * diagonalUnit);
        };
        const std::array polygon{toLocal(visible.Min), toLocal(ImVec2(visible.Max.x, visible.Min.y)),
            toLocal(visible.Max), toLocal(ImVec2(visible.Min.x, visible.Max.y))};
        ImRect localBounds(polygon.front(), polygon.front());
        for (const auto point : polygon) localBounds.Add(point);
        const float stepX = textSize.x + fontSize * WatermarkGapScale;
        const float stepY = textSize.y + fontSize * WatermarkGapScale;
        // 为字体的字形悬垂留出一个字号余量，边缘文字最终仍由 GPU 精确裁剪。
        const float glyphMargin = fontSize;
        const int firstRow = static_cast<int>(std::floor((localBounds.Min.y - textSize.y - glyphMargin) / stepY));
        const int lastRow = static_cast<int>(std::ceil((localBounds.Max.y + glyphMargin) / stepY));
        constexpr ImU32 maximumAlpha = IM_COL32_A_MASK >> IM_COL32_A_SHIFT;
        const ImU32 alpha = static_cast<ImU32>(m_watermarkOpacity) * maximumAlpha / WatermarkSettings::MaximumOpacity;
        const ImU32 color = (m_toolColor & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
        // FullScreen 裁剪仍是窗口坐标，不能用于包含负坐标的未旋转网格，否则字形会提前缺失。
        const ImVec2 clipMargin(textSize.x + 2 * glyphMargin, textSize.y + 2 * glyphMargin);
        drawList->PushClipRect(ImVec2(localBounds.Min.x - clipMargin.x, localBounds.Min.y - clipMargin.y),
                               ImVec2(localBounds.Max.x + clipMargin.x, localBounds.Max.y + clipMargin.y), false);
        drawList->AddDrawCmd();
        const int firstCommand = drawList->CmdBuffer.Size - 1;
        const int firstVertex = drawList->VtxBuffer.Size;
        for (int row = firstRow; row <= lastRow; ++row) {
            const float y = static_cast<float>(row) * stepY;
            const auto bounds = GeometryUtils::HorizontalStripXBounds(polygon, y - glyphMargin, y + textSize.y + glyphMargin);
            if (!bounds) continue;
            const float shift = row % 2 != 0 ? stepX * 0.5f : 0;
            const int firstColumn = static_cast<int>(std::ceil((bounds->first - textSize.x - glyphMargin - shift) / stepX));
            const int lastColumn = static_cast<int>(std::floor((bounds->second + glyphMargin - shift) / stepX));
            for (int column = firstColumn; column <= lastColumn; ++column) {
                drawList->AddText(font, fontSize, ImVec2(static_cast<float>(column) * stepX + shift, y),
                                   color, m_watermarkText.c_str());
            }
        }
        // 临时放宽裁剪生成完整字形，再将新增顶点旋转 -45°、平移到文档网格原点。
        for (int i = firstVertex; i < drawList->VtxBuffer.Size; ++i) {
            auto& position = drawList->VtxBuffer[i].pos;
            position = ImVec2(origin.x + (position.x + position.y) * diagonalUnit,
                              origin.y + (position.y - position.x) * diagonalUnit);
        }
        for (int i = firstCommand; i < drawList->CmdBuffer.Size; ++i) {
            drawList->CmdBuffer[i].ClipRect = ImVec4(visible.Min.x, visible.Min.y, visible.Max.x, visible.Max.y);
        }
        drawList->PopClipRect();
    }

    void Canvas::RenderHighlightMask(ImDrawList* drawList, ImVec2 offset, ImRect region) const {
        if (m_highlightTransparency == HighlightSettings::MaximumTransparency) return;
        std::vector<MaskGeometry::Polygon> holes;
        const auto append = [&](const Layer* layer) {
            const auto* highlight = dynamic_cast<const HighlightLayer*>(layer);
            if (!highlight) return;
            const auto bounds = highlight->GetBounds();
            if (bounds.GetWidth() <= 0 || bounds.GetHeight() <= 0) return;
            holes.push_back(highlight->IsEllipse() ? MaskGeometry::Ellipse(bounds) : MaskGeometry::RoundedRectangle(bounds));
        };
        for (const auto& layer : m_layerManager.GetLayers()) append(layer.get());
        append(m_currentLayer.get());
        if (holes.empty()) return;
        constexpr int maximumAlpha = IM_COL32_A_MASK >> IM_COL32_A_SHIFT;
        const int alpha = (HighlightSettings::MaximumTransparency - m_highlightTransparency) * maximumAlpha /
            HighlightSettings::MaximumTransparency;
        const auto polygons = MaskGeometry::SubtractHoles(MaskGeometry::RoundedRectangle(region), holes);
        MaskGeometry::Draw(drawList, polygons, offset, IM_COL32(0, 0, 0, alpha));
    }

    void Canvas::RenderScreenshot(ImDrawList *drawList) const {
        if (!m_screenshot) return;

        // offset=(0,0)，窗口覆盖单个屏幕
        if (HasFractionalSampling(ImRect(ImVec2(0, 0), m_displaySize)))
            RenderImageRegion(drawList, ImRect(ImVec2(0, 0), m_displaySize), ImVec2(0, 0));
        else drawList->AddImage(m_screenshot, ImVec2(0, 0), m_displaySize, ImVec2(0, 0), ImVec2(1, 1), Colors::HandleFill);
    }

    bool Canvas::HasFractionalSampling() const {
        return HasFractionalSampling(GetSelectionRect());
    }
    bool Canvas::HasFractionalSampling(ImRect region) const {
        const auto a = ToPixel(region.Min), b = ToPixel(region.Max);
        for (float v : {a.x, a.y, b.x, b.y}) if (std::abs(v - std::round(v)) > 0.0001f) return true;
        return false;
    }

    void Canvas::RenderImageRegion(ImDrawList* list, ImRect region, ImVec2 offset) const {
        const auto scale = GetPixelScale();
        const auto a = ToPixel(region.Min), b = ToPixel(region.Max);
        const int x0 = std::clamp(int(std::ceil(a.x - .5f)), 0, int(m_screenshotSize.x) - 1);
        const int y0 = std::clamp(int(std::ceil(a.y - .5f)), 0, int(m_screenshotSize.y) - 1);
        const int x1 = std::clamp(int(std::ceil(b.x - .5f)) - 1, x0, int(m_screenshotSize.x) - 1);
        const int y1 = std::clamp(int(std::ceil(b.y - .5f)) - 1, y0, int(m_screenshotSize.y) - 1);
        // 九宫格将边缘的采样位置钳制到区内像素中心，不读取区外原始像素。
        const float xs[] = {region.Min.x, (x0 + .5f + m_sampleOffset.x) / scale.x,
                            (x1 + .5f + m_sampleOffset.x) / scale.x, region.Max.x};
        const float ys[] = {region.Min.y, (y0 + .5f + m_sampleOffset.y) / scale.y,
                            (y1 + .5f + m_sampleOffset.y) / scale.y, region.Max.y};
        const float us[] = {(x0 + .5f) / m_screenshotSize.x, (x0 + .5f) / m_screenshotSize.x,
                            (x1 + .5f) / m_screenshotSize.x, (x1 + .5f) / m_screenshotSize.x};
        const float vs[] = {(y0 + .5f) / m_screenshotSize.y, (y0 + .5f) / m_screenshotSize.y,
                            (y1 + .5f) / m_screenshotSize.y, (y1 + .5f) / m_screenshotSize.y};
        for (int y = 0; y < 3; ++y) for (int x = 0; x < 3; ++x) {
            if (xs[x + 1] <= xs[x] || ys[y + 1] <= ys[y]) continue;
            list->AddImage(m_screenshot, ImVec2(xs[x] + offset.x, ys[y] + offset.y),
                ImVec2(xs[x + 1] + offset.x, ys[y + 1] + offset.y),
                ImVec2(us[x], vs[y]), ImVec2(us[x + 1], vs[y + 1]), Colors::HandleFill);
        }
    }

    void Canvas::RenderSmartRegionBorder(ImDrawList *drawList) const {
        if (!m_smartRegion) return;

        const ImRect& bounds = m_smartRegion->bounds;

        // 将边界裁剪到屏幕范围内，留出边框线宽和手柄的渲染空间
        const float margin = GetSelectionHandleSize() * 0.2f;
        const float minRx = ImMax(ImTrunc(bounds.Min.x) + 0.5f, margin);
        const float minRy = ImMax(ImTrunc(bounds.Min.y) + 0.5f, margin);
        const float maxRx = ImMin(ImTrunc(bounds.Max.x) + 0.5f, m_displaySize.x - margin);
        const float maxRy = ImMin(ImTrunc(bounds.Max.y) + 0.5f, m_displaySize.y - margin);

        // 实线边框（与截图选区边框风格一致）
        constexpr ImU32 borderColor = Colors::SelectionBorder;
        const float borderWidth = GetSelectionBorderWidth();
        drawList->AddLine(ImVec2(minRx, minRy), ImVec2(maxRx, minRy), borderColor, borderWidth);  // 上
        drawList->AddLine(ImVec2(maxRx, minRy), ImVec2(maxRx, maxRy), borderColor, borderWidth);  // 右
        drawList->AddLine(ImVec2(maxRx, maxRy), ImVec2(minRx, maxRy), borderColor, borderWidth);  // 下
        drawList->AddLine(ImVec2(minRx, maxRy), ImVec2(minRx, minRy), borderColor, borderWidth);  // 左

        // 8个方形手柄（与截图选区手柄风格一致），坐标同样裁剪到屏幕内
        ImVec2 handles[8] = {
            ImVec2(minRx, minRy),
            ImVec2((minRx + maxRx) * 0.5f, minRy),
            ImVec2(maxRx, minRy),
            ImVec2(minRx, (minRy + maxRy) * 0.5f),
            ImVec2(maxRx, (minRy + maxRy) * 0.5f),
            ImVec2(minRx, maxRy),
            ImVec2((minRx + maxRx) * 0.5f, maxRy),
            ImVec2(maxRx, maxRy)
        };
        const float h = GetSelectionHandleSize();
        for (const auto & handle : handles) {
            drawList->AddRectFilled(
                ImVec2(handle.x - h * 0.5f, handle.y - h * 0.5f),
                ImVec2(handle.x + h * 0.5f, handle.y + h * 0.5f),
                Colors::SelectionBorder);
        }
    }

    void Canvas::RenderDimmedRegion(ImDrawList *drawList) const {
        if (!HasSelection()) {
            return;
        }

        const ImRect sel = GetSelectionRect();
        const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        constexpr ImU32 dimColor = Colors::DimOverlay;

        // 左
        drawList->AddRectFilled(ImVec2(0, 0), ImVec2(sel.Min.x, displaySize.y), dimColor);
        // 右
        drawList->AddRectFilled(ImVec2(sel.Max.x, 0), ImVec2(displaySize.x, displaySize.y), dimColor);
        // 上
        drawList->AddRectFilled(ImVec2(sel.Min.x, 0), ImVec2(sel.Max.x, sel.Min.y), dimColor);
        // 下
        drawList->AddRectFilled(ImVec2(sel.Min.x, sel.Max.y), ImVec2(sel.Max.x, displaySize.y), dimColor);
    }

    // 渲染矩形编辑选区

    void Canvas::RenderSelectionBorder(ImDrawList *drawList) const {
        const ImRect sel = GetSelectionRect();

        // 使用整数坐标避免半像素抗锯齿问题
        const ImVec2 minP(ImTrunc(sel.Min.x) + 0.5f, ImTrunc(sel.Min.y) + 0.5f);
        const ImVec2 maxP(ImTrunc(sel.Max.x) + 0.5f, ImTrunc(sel.Max.y) + 0.5f);

        // 外边框（白色）- 使用 4 条线统一绘制方式
        constexpr ImU32 whiteColor = Colors::SelectionBorder;
        const float borderWidth = GetSelectionBorderWidth();
        drawList->AddLine(ImVec2(minP.x, minP.y), ImVec2(maxP.x, minP.y), whiteColor, borderWidth);  // 上
        drawList->AddLine(ImVec2(maxP.x, minP.y), ImVec2(maxP.x, maxP.y), whiteColor, borderWidth);  // 右
        drawList->AddLine(ImVec2(maxP.x, maxP.y), ImVec2(minP.x, maxP.y), whiteColor, borderWidth);  // 下
        drawList->AddLine(ImVec2(minP.x, maxP.y), ImVec2(minP.x, minP.y), whiteColor, borderWidth);  // 左
    }

    void Canvas::RenderResizeHandles(ImDrawList *drawList) const {
        const ImRect sel = GetSelectionRect();
        // 8个调整手柄位置
        ImVec2 handles[8] = {
            ImVec2(sel.Min.x, sel.Min.y), // 左上
            ImVec2((sel.Min.x + sel.Max.x) * 0.5f, sel.Min.y), // 上
            ImVec2(sel.Max.x, sel.Min.y), // 右上
            ImVec2(sel.Min.x, (sel.Min.y + sel.Max.y) * 0.5f), // 左
            ImVec2(sel.Max.x, (sel.Min.y + sel.Max.y) * 0.5f), // 右
            ImVec2(sel.Min.x, sel.Max.y), // 左下
            ImVec2((sel.Min.x + sel.Max.x) * 0.5f, sel.Max.y), // 下
            ImVec2(sel.Max.x, sel.Max.y) // 右下
        };

        const float h = GetSelectionHandleSize();
        for (const auto & handle : handles) {
            drawList->AddRectFilled(
                ImVec2(handle.x - h * 0.5f, handle.y - h * 0.5f),
                ImVec2(handle.x + h * 0.5f, handle.y + h * 0.5f),
                Colors::SelectionBorder
            );
        }
    }

    // 矩形选区左上角的尺寸提示信息

    void Canvas::RenderSizeTooltip(ImDrawList *drawList) const {
        const ImRect sel = GetSelectionRect();
        // 计算实际像素尺寸：逻辑点 * (像素尺寸 / 显示尺寸) = scaleFactor
        const float scaleX = m_screenshotSize.x / m_displaySize.x;
        const float scaleY = m_screenshotSize.y / m_displaySize.y;
        const int w = static_cast<int>((sel.Max.x - sel.Min.x) * scaleX);
        const int h = static_cast<int>((sel.Max.y - sel.Min.y) * scaleY);

        char text[64];
        snprintf(text, sizeof(text), "%d x %d", w, h);

        ImFont* font = m_tooltipFont ? m_tooltipFont : ImGui::GetFont();
        const float fontSize = GetTopTooltipFontSize() * m_uiScale;
        const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
        const ImVec2 pos(sel.Min.x, sel.Min.y - textSize.y - 4);

        // 白色文字，无背景
        drawList->AddText(font, fontSize, pos, Colors::White, text);
    }

    void Canvas::RenderCurrentShape(ImDrawList *drawList) const {
        // 渲染正在绘制的图形
        if (m_state != SelectionState::Drawing || !m_currentLayer) return;

        m_currentLayer->Render(drawList, ImVec2(0, 0), m_documentRenderScale);
    }
} // namespace shotlet
