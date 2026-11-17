// Canvas 鼠标交互逻辑：
// 负责选区创建、移动、缩放，图层拖拽，绘图工具绘制，
// 以及鼠标按下/移动/抬起过程中与状态机相关的交互处理。
#include "Canvas.h"
#include "utils/Logger.h"
#include <algorithm>

namespace shotlet {
    namespace {
        constexpr float SHAPE_PREVIEW_DRAG_THRESHOLD = 4.0f;

        bool IsShapePreviewThresholdTool(const ToolType tool) {
            return tool == ToolType::Rectangle ||
                   tool == ToolType::Ellipse ||
                   tool == ToolType::Arrow || tool == ToolType::Highlight;
        }

        bool HasExceededShapePreviewThreshold(const ImVec2& start, const ImVec2& current) {
            return std::fabs(current.x - start.x) > SHAPE_PREVIEW_DRAG_THRESHOLD ||
                   std::fabs(current.y - start.y) > SHAPE_PREVIEW_DRAG_THRESHOLD;
        }
    } // namespace

    void Canvas::OnMouseDown(const ImVec2 &pos) {
        if (!m_screenshot) return;

        m_mouseDown = true;
        m_dragStart = pos;
        m_justDeselected = false;

        // 文本输入点击检测（委托给 CanvasTextInput.cpp）
        if (HandleTextInputClick(pos)) return;

        // 编辑窗口固定全图选区，仅允许图层手柄交互。
        if (HasSelection() && !m_imageEditing) {
            m_activeHandle = HitTestResizeHandle(pos);
            if (m_activeHandle != ResizeHandle::None) {
                m_state = SelectionState::Resizing;
                m_selectionBeforeDrag = GetSelectionRect();
                ClearAllSelectedLayers();
                if (m_currentTool != ToolType::None) {
                    m_currentTool = ToolType::None;
                }
                return;
            }
        }

        if (m_selectedLayer) {
            // TextLayer 选中时点击空白处取消选中，不进入拖拽
            if (auto* textLayer = dynamic_cast<TextLayer*>(m_selectedLayer)) {
                ImRect bounds = textLayer->GetFrameBounds(GetTextFrameScale());
                if (!bounds.Contains(pos)) {
                    // 点击在 TextLayer 外 → 取消选中
                    ClearAllSelectedLayers();
                    m_justDeselected = true;
                    if (m_currentTool == ToolType::Text) {
                        m_skipTextLayerSelect = true;
                    }
                    // 继续往下处理其他图层检测
                } else {
                    // 点击在 TextLayer 内 → 进入拖拽
                    m_selectedLayer->StartDrag(pos);
                    m_layerDragging = true;
                    m_layerPreDragBounds = m_selectedLayer->GetDragBounds();
                    return;
                }
            } else {
                // 非 TextLayer：先检测手柄 → 缩放
                const ResizeHandle handle = m_selectedLayer->HitTestResizeHandle(pos);
                if (handle != ResizeHandle::None) {
                    m_selectedLayer->StartResize(handle);
                    m_dragStart = pos;
                    return;
                }

                // 再检测边框/区域 → 拖拽移动
                const float threshold = m_selectedLayer->GetThickness() * 0.5f + 5.0f;
                if (m_selectedLayer->IsPointNearBorder(pos, threshold)) {
                    // 马赛克只支持选中删除，点击或拖动均不改变笔迹。
                    if (dynamic_cast<MosaicLayer*>(m_selectedLayer)) return;
                    m_selectedLayer->StartDrag(pos);
                    m_layerDragging = true;
                    m_layerPreDragBounds = m_selectedLayer->GetDragBounds();
                    return;
                }

                // 不在手柄上也不在边框上 → 取消选中，继续检测其他图层
                ClearAllSelectedLayers();
                m_justDeselected = true;
            }
        }

        // 检查是否点击在未选中的图层上 → 选中 + 准备拖拽
        {
            auto& layers = m_layerManager.GetLayers();
            for (int i = static_cast<int>(layers.size()) - 1; i >= 0; i--) {
                Layer* layer = layers[i].get();
                // 绘图工具开启时允许覆盖马赛克标注，退出绘图工具后才点选已有笔迹。
                const bool mosaic = dynamic_cast<MosaicLayer*>(layer) != nullptr;
                if (mosaic && m_currentTool != ToolType::None) continue;
                if (dynamic_cast<HighlightLayer*>(layer) && m_currentTool != ToolType::None && m_currentTool != ToolType::Highlight) continue;

                // TextLayer 用扩大区域检测
                if (layer->SupportsFontSize()) {
                    // 取消选中 TextLayer 后的下次点击，跳过 TextLayer 选中检测
                    if (m_skipTextLayerSelect) continue;
                    auto* textLayer = static_cast<TextLayer*>(layer);
                    ImRect bounds = textLayer->GetFrameBounds(GetTextFrameScale());
                    if (bounds.Contains(pos)) {
                        ClearAllSelectedLayers();
                        m_selectedLayer = layer;
                        m_selectedLayer->SetSelected(true);
                        m_selectedLayer->StartDrag(pos);
                        m_layerDragging = true;
                        m_layerPreDragBounds = m_selectedLayer->GetDragBounds();
                        return;
                    }
                    continue;
                }

                const float threshold = layer->GetThickness() * 0.5f + 5.0f;
                if (layer->IsPointNearBorder(pos, threshold)) {
                    ClearAllSelectedLayers();
                    m_selectedLayer = layer;
                    m_selectedLayer->SetSelected(true);
                    if (mosaic) {
                        m_currentTool = ToolType::None;
                        return;
                    }
                    m_selectedLayer->StartDrag(pos);
                    m_layerDragging = true;
                    m_layerPreDragBounds = m_selectedLayer->GetDragBounds();
                    return;
                }
            }
        }

        // 数字点单击创建，独立于形状拖拽阈值和取消选中后的跳过规则。
        if (m_currentTool == ToolType::Number) {
            if (!HasSelection()) return;
            const ImRect selection = GetSelectionRect();
            const float diameter = GetNumberDiameter();
            if (!selection.Contains(pos) || selection.GetWidth() < diameter || selection.GetHeight() < diameter) return;
            const float radius = diameter * 0.5f;
            const ImVec2 center(std::clamp(pos.x, selection.Min.x + radius, selection.Max.x - radius),
                                std::clamp(pos.y, selection.Min.y + radius, selection.Max.y - radius));
            int number = 1;
            for (const auto& layer : m_layerManager.GetLayers()) {
                if (dynamic_cast<NumberLayer*>(layer.get())) ++number;
            }
            auto marker = std::make_unique<NumberLayer>(center, number, m_toolColor, diameter);
            marker->InitializeLabel(selection, m_textFontSize, GetTextFrameScale());
            m_currentLayer = std::move(marker);
            m_state = SelectionState::Drawing;
            return;
        }

        // 如果选择了绘图工具（水印工具不在画布上拖拽绘制，留给后续水印图层实现）
        if (m_currentTool != ToolType::None && m_currentTool != ToolType::Text &&
            m_currentTool != ToolType::Watermark && !m_justDeselected) {
            // 只有在有选区且点击在选区内时才允许绘制
            if (!HasSelection()) return;
            if (GetSelectionRect().Contains(pos) == false) return;
            m_state = SelectionState::Drawing;
            ClearAllSelectedLayers();

            const ImVec2 clampedPos = ClampToSelection(pos);
            m_drawStart = clampedPos;

            switch (m_currentTool) {
                case ToolType::Rectangle:
                case ToolType::Ellipse:
                case ToolType::Arrow:
                case ToolType::Highlight:
                    break;
                case ToolType::Brush:
                    m_currentLayer = std::make_unique<BrushLayer>(
                        std::vector{clampedPos}, m_toolColor, m_toolThickness);
                    break;
                case ToolType::Mosaic:
                    m_currentLayer = std::make_unique<MosaicLayer>(
                        std::vector{clampedPos}, m_mosaicBlockSize, m_mosaicBrushRadius);
                    // 延迟建立编辑缓冲（首次）：original/mosaic/mask 只覆盖选区区域
                    EnsureEditRegion(ComputeRefreshRegionPx());
                    EnsureMosaicBuffers();
                    PaintMosaicMask(clampedPos, m_mosaicBrushRadius);
                    {
                        const float scaleX = GetPixelScale().x;
                        const float scaleY = GetPixelScale().y;
                        const float radiusPx = m_mosaicBrushRadius * scaleX;
                        const float px = ToPixel(clampedPos).x;
                        const float py = ToPixel(clampedPos).y;
                        const ImRect compositeRegion(px - radiusPx, py - radiusPx, px + radiusPx, py + radiusPx);
                        CompositeMosaicRegion(compositeRegion);
                        UpdateTextureRegion(compositeRegion);
                    }
                    m_hasLastMosaicPoint = true;
                    m_lastMosaicPoint = clampedPos;
                    break;
                default:
                    break;
            }
            return;
        }

        // 文本工具
        if (m_currentTool == ToolType::Text) {
            // 已有选中 TextLayer 时，点击其他区域取消选中，需要再次点击才能创建新输入框
            if (m_selectedLayer && m_selectedLayer->SupportsFontSize()) {
                ClearAllSelectedLayers();
                return;
            }
            // 刚取消选中 TextLayer 时，本次点击不创建输入框，需要再次点击
            if (m_justDeselected) {
                return;
            }
            if (HasSelection()) {
                if (const ImRect sel = GetSelectionRect(); sel.Contains(pos)) {
                    // 输入框创建成功，消费 m_skipTextLayerSelect 标志
                    m_skipTextLayerSelect = false;
                    m_textInput.active = true;
                    m_textInput.position = pos;
                    m_textInput.text.clear();
                    m_textInput.cursorPos = 0;
                    m_textInput.focusRequested = true;
                    m_textInput.fontSize = m_textFontSize;  // 记录开始输入时的字号
                    m_textInput.wrapWidth = sel.Max.x - pos.x;  // 新建：从选区计算
                    m_state = SelectionState::Drawing;
                    return;
                }
            }
            return;
        }

        // 清除选中状态（点击在空白处）
        ClearAllSelectedLayers();

        if (m_imageEditing) return;

        // 如果已经有选区（手柄检测已在前面处理）
        if (HasSelection()) {
            if (const ImRect sel = GetSelectionRect(); sel.Contains(pos)) {
                m_state = SelectionState::Moving;
                m_selectionBeforeDrag = sel;
                return;
            }

            // 有选区但点击不在选区内，不允许重新框选
            return;
        }

        // 开始新选择
        // 如果点击在智能选区区域内，不立即启动框选（等待双击确认或拖拽覆盖）
        if (m_smartRegion && m_smartRegion->bounds.Contains(pos)) {
            Logger::Debug("[OnMouseDown] Click on smart region, waiting for drag or double-click");
            m_state = SelectionState::None;
            m_dragStart = pos;
            m_mouseDown = true;
            return;
        }

        Logger::Debug("[OnMouseDown] Start selection");
        m_state = SelectionState::Selecting;
        m_selectionStart = ClampToCanvas(pos);
        m_selectionEnd = m_selectionStart;
    }

    void Canvas::OnMouseMove(const ImVec2 &pos) {
        if (!m_mouseDown) return;

        // 文本选中拖拽
        if (m_textInput.selecting) {
            int charIdx = GetCharIndexFromPixelPos(pos);
            m_textInput.selectionEnd = charIdx;
            m_textInput.cursorPos = charIdx;
            return;
        }

        switch (m_state) {
            case SelectionState::None: {
                // 在智能选区内按下后拖拽：超过阈值则切换到手动框选
                float dx = pos.x - m_dragStart.x;
                float dy = pos.y - m_dragStart.y;
                if (dx * dx + dy * dy > 16.0f) {  // 4px 阈值
                    m_state = SelectionState::Selecting;
                    m_selectionStart = ClampToCanvas(m_dragStart);
                    m_selectionEnd = ClampToCanvas(pos);
                    Logger::Debug("[OnMouseMove] Drag exceeds threshold, start manual selection");
                }
                break;
            }
            case SelectionState::Drawing: {
                // 文本工具不需要鼠标移动处理
                if (m_currentTool == ToolType::Text) break;

                ImVec2 clampedPos = ClampToSelection(pos);

                if (!m_currentLayer && IsShapePreviewThresholdTool(m_currentTool)) {
                    if (!HasExceededShapePreviewThreshold(m_drawStart, clampedPos)) {
                        break;
                    }

                    switch (m_currentTool) {
                        case ToolType::Highlight:
                            m_currentLayer = std::make_unique<HighlightLayer>(m_drawStart, clampedPos, m_highlightEllipse);
                            break;
                        case ToolType::Rectangle: {
                            auto rect = std::make_unique<RectangleLayer>(
                                m_drawStart, clampedPos, m_toolColor, m_toolThickness);
                            rect->SetFilled(m_toolFill);
                            m_currentLayer = std::move(rect);
                            break;
                        }
                        case ToolType::Ellipse: {
                            auto ellipse = std::make_unique<EllipseLayer>(
                                m_drawStart, clampedPos, m_toolColor, m_toolThickness);
                            ellipse->SetFilled(m_toolFill);
                            m_currentLayer = std::move(ellipse);
                            break;
                        }
                        case ToolType::Arrow:
                            m_currentLayer = std::make_unique<ArrowLayer>(
                                m_drawStart, clampedPos, m_toolColor, m_toolThickness);
                            break;
                        default:
                            break;
                    }
                }

                if (!m_currentLayer) break;

                // 拖拽预览：只更新终点，不重建对象（避免每个 mouse move
                // 一次堆分配+销毁；SetEnd 是简单字段赋值）
                if (auto* rect = dynamic_cast<RectangleLayer*>(m_currentLayer.get())) {
                    rect->SetEnd(clampedPos);
                } else if (auto* circle = dynamic_cast<EllipseLayer*>(m_currentLayer.get())) {
                    circle->SetEnd(clampedPos);
                } else if (auto* arrow = dynamic_cast<ArrowLayer*>(m_currentLayer.get())) {
                    arrow->SetEnd(clampedPos);
                } else if (auto* brush = dynamic_cast<BrushLayer*>(m_currentLayer.get())) {
                    brush->AddPoint(clampedPos);
                } else if (auto* mosaic = dynamic_cast<MosaicLayer*>(m_currentLayer.get())) {
                    // 点间插值：保证笔迹连续
                    float scaleX = GetPixelScale().x;
                    float scaleY = GetPixelScale().y;
                    float brushRadiusPx = m_mosaicBrushRadius * scaleX;
                    ImRect compositeRegion;  // 本帧受影响区域
                    bool hasCompositeRegion = false;
                    auto expandCompositeRegion = [&](const ImRect& region) {
                        if (!hasCompositeRegion) {
                            compositeRegion = region;
                            hasCompositeRegion = true;
                            return;
                        }
                        compositeRegion.Min.x = std::min(compositeRegion.Min.x, region.Min.x);
                        compositeRegion.Min.y = std::min(compositeRegion.Min.y, region.Min.y);
                        compositeRegion.Max.x = std::max(compositeRegion.Max.x, region.Max.x);
                        compositeRegion.Max.y = std::max(compositeRegion.Max.y, region.Max.y);
                    };

                    if (m_hasLastMosaicPoint) {
                        float dist = std::sqrt(
                            (clampedPos.x - m_lastMosaicPoint.x) * (clampedPos.x - m_lastMosaicPoint.x) +
                            (clampedPos.y - m_lastMosaicPoint.y) * (clampedPos.y - m_lastMosaicPoint.y));

                        if (float stepLen = m_mosaicBrushRadius * 0.3f; dist > stepLen) {
                            int steps = static_cast<int>(std::ceil(dist / stepLen));
                            for (int i = 1; i <= steps; i++) {
                                float t = static_cast<float>(i) / steps;
                                ImVec2 interpPt(
                                    m_lastMosaicPoint.x + (clampedPos.x - m_lastMosaicPoint.x) * t,
                                    m_lastMosaicPoint.y + (clampedPos.y - m_lastMosaicPoint.y) * t);
                                mosaic->AddPoint(interpPt);
                                PaintMosaicMask(interpPt, m_mosaicBrushRadius);

                                float ipx = ToPixel(interpPt).x;
                                float ipy = ToPixel(interpPt).y;
                                ImRect ptRegion(ipx - brushRadiusPx, ipy - brushRadiusPx, ipx + brushRadiusPx, ipy + brushRadiusPx);
                                expandCompositeRegion(ptRegion);
                            }
                        }
                    }

                    mosaic->AddPoint(clampedPos);
                    PaintMosaicMask(clampedPos, m_mosaicBrushRadius);

                    float px = ToPixel(clampedPos).x;
                    float py = ToPixel(clampedPos).y;
                    ImRect ptRegion(px - brushRadiusPx, py - brushRadiusPx, px + brushRadiusPx, py + brushRadiusPx);
                    expandCompositeRegion(ptRegion);

                    if (hasCompositeRegion) {
                        CompositeMosaicRegion(compositeRegion);
                        UpdateTextureRegion(compositeRegion);
                    }
                    m_hasLastMosaicPoint = true;
                    m_lastMosaicPoint = clampedPos;
                }
                break;
            }

            case SelectionState::Selecting: {
                m_selectionEnd = ClampToCanvas(pos);
                break;
            }

            case SelectionState::Moving: {
                auto delta = ImVec2(pos.x - m_dragStart.x, pos.y - m_dragStart.y);
                ImRect before = m_selectionBeforeDrag;
                ImRect newSel(before.Min.x + delta.x, before.Min.y + delta.y,
                              before.Max.x + delta.x, before.Max.y + delta.y);
                newSel = ConstrainSelectionToBounds(newSel);
                m_selectionStart = newSel.Min;
                m_selectionEnd = newSel.Max;
                break;
            }

            case SelectionState::Resizing: {
                auto delta = ImVec2(pos.x - m_dragStart.x, pos.y - m_dragStart.y);
                ImRect r = m_selectionBeforeDrag;
                ImVec2 newMin = r.Min;
                ImVec2 newMax = r.Max;

                switch (m_activeHandle) {
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

                // 约束：如果选区边进入了绘制内容区域，限制该边不能越过
                if (m_hasDrawnContent) {
                    if (newMin.x > m_drawnContentBounds.Min.x)
                        newMin.x = std::min(newMin.x, m_drawnContentBounds.Min.x);
                    if (newMin.y > m_drawnContentBounds.Min.y)
                        newMin.y = std::min(newMin.y, m_drawnContentBounds.Min.y);
                    if (newMax.x < m_drawnContentBounds.Max.x)
                        newMax.x = std::max(newMax.x, m_drawnContentBounds.Max.x);
                    if (newMax.y < m_drawnContentBounds.Max.y)
                        newMax.y = std::max(newMax.y, m_drawnContentBounds.Max.y);
                }

                // 确保不超出画布范围
                newMin.x = std::max(newMin.x, 0.0f);
                newMin.y = std::max(newMin.y, 0.0f);
                newMax.x = std::min(newMax.x, m_displaySize.x);
                newMax.y = std::min(newMax.y, m_displaySize.y);

                m_selectionStart = newMin;
                m_selectionEnd = newMax;
                break;
            }

            default:
                break;
        }

        // 选中图层拖拽移动处理（独立于 m_state）
        if (m_selectedLayer && m_layerDragging) {
            ImRect sel = GetSelectionRect();

            // 计算原始总位移（从拖拽起点到当前鼠标位置）
            auto rawTotal = ImVec2(pos.x - m_dragStart.x, pos.y - m_dragStart.y);

            // 严格约束到选区内：预期新位置 = 拖拽前边界 + 原始总位移
            ImVec2 clampedTotal = rawTotal;
            if (m_layerPreDragBounds.Min.x + rawTotal.x < sel.Min.x)
                clampedTotal.x = sel.Min.x - m_layerPreDragBounds.Min.x;
            else if (m_layerPreDragBounds.Max.x + rawTotal.x > sel.Max.x)
                clampedTotal.x = sel.Max.x - m_layerPreDragBounds.Max.x;
            if (m_layerPreDragBounds.Min.y + rawTotal.y < sel.Min.y)
                clampedTotal.y = sel.Min.y - m_layerPreDragBounds.Min.y;
            else if (m_layerPreDragBounds.Max.y + rawTotal.y > sel.Max.y)
                clampedTotal.y = sel.Max.y - m_layerPreDragBounds.Max.y;

            if (auto* brushLayer = dynamic_cast<BrushLayer*>(m_selectedLayer)) {
                // BrushLayer 使用增量：增量 = 约束后的总量 - 已累积量
                ImRect cur = brushLayer->GetBounds();
                ImVec2 accumulated(cur.Min.x - m_layerPreDragBounds.Min.x,
                                   cur.Min.y - m_layerPreDragBounds.Min.y);
                ImVec2 increment(clampedTotal.x - accumulated.x, clampedTotal.y - accumulated.y);
                brushLayer->UpdateDrag(increment);
            } else {
                m_selectedLayer->UpdateDrag(clampedTotal);
            }
            return;
        }

        // 选中图层 resize 手柄处理（独立于 m_state）
        if (m_selectedLayer && m_selectedLayer->GetResizeHandle() != ResizeHandle::None) {
            auto delta = ImVec2(pos.x - m_dragStart.x, pos.y - m_dragStart.y);
            ImRect selection = GetSelectionRect();
            m_selectedLayer->UpdateResize(delta, selection);
        }
    }

    void Canvas::OnMouseUp(const ImVec2 &pos) {
        m_mouseDown = false;

        // 结束文本选中拖拽
        if (m_textInput.selecting) {
            m_textInput.selecting = false;
            if (m_textInput.selectionStart == m_textInput.selectionEnd) {
                m_textInput.selectionStart = -1;
                m_textInput.selectionEnd = -1;
            }
            return;
        }

        if (m_state == SelectionState::Drawing) {
            auto* marker = dynamic_cast<NumberLayer*>(m_currentLayer.get());
            if (m_currentLayer) {
                FinalizeCurrentLayer();
            }

            m_state = SelectionState::Selected;
            m_hasLastMosaicPoint = false;
            // 图层加入管理器并分配 ID 后再关联输入状态，避免悬空的编辑对象。
            if (marker) BeginNumberLabelInput(*marker);
            return;
        }

        if (m_state == SelectionState::Selecting) {
            const ImRect sel = GetSelectionRect();
            if (constexpr float minSize = 10.0f; sel.GetWidth() >= minSize && sel.GetHeight() >= minSize) {
                Logger::Debug("[OnMouseUp] Selection done: {:.0f}x{:.0f}", sel.GetWidth(), sel.GetHeight());
                m_state = SelectionState::Selected;
            } else {
                m_state = SelectionState::None;
            }
        } else if (m_state == SelectionState::Moving || m_state == SelectionState::Resizing) {
            m_state = SelectionState::Selected;
        }

        m_activeHandle = ResizeHandle::None;

        // 结束选中图层的拖拽/缩放操作
        if (m_selectedLayer) {
            if (m_layerDragging) {
                m_selectedLayer->EndDrag();
                m_layerDragging = false;
                if (HasSelectedNumberLayer()) RecalculateDrawnContentBounds();
                ConditionalRefreshTexture();
            }
            if (m_selectedLayer->GetResizeHandle() != ResizeHandle::None) {
                m_selectedLayer->EndResize();
                RefreshTexture();
            }
        }
    }
} // namespace shotlet
