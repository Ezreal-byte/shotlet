// Canvas 状态与基础配置管理：
// 负责画布初始化、截图数据接入、工具与选区状态、图层选择状态、
// 以及与绘制内容边界相关的基础辅助逻辑。
#include "Canvas.h"
#include "platform/FontProvider.h"
#include "utils/Logger.h"
#include "utils/StringUtils.h"
#include <algorithm>
#include <cmath>

namespace shotlet {
    Canvas::Canvas() : m_reedit(), m_textFontSize(GetTextLayerFontSize()) {}

    void Canvas::SetHighlightEllipse(bool ellipse) {
        m_highlightEllipse = ellipse;
        if (auto* layer = dynamic_cast<HighlightLayer*>(m_selectedLayer)) layer->SetEllipse(ellipse);
    }
    bool Canvas::GetHighlightEllipse() const {
        const auto* layer = dynamic_cast<const HighlightLayer*>(m_selectedLayer);
        return layer ? layer->IsEllipse() : m_highlightEllipse;
    }
    void Canvas::SetHighlightTransparency(int percent) {
        // 所有高亮共用一层遮罩，调整透明度不会让重叠区域重复变暗。
        m_highlightTransparency = std::clamp(percent, HighlightSettings::MinimumTransparency, HighlightSettings::MaximumTransparency);
    }

    void Canvas::SetWatermarkOpacity(int percent) {
        m_watermarkOpacity = std::clamp(percent, WatermarkSettings::MinimumOpacity, WatermarkSettings::MaximumOpacity);
    }

    void Canvas::SetWatermarkText(const std::string& text) {
        m_watermarkText = text.substr(0, StringUtils::Utf8PrefixBytes(text, WATERMARK_MAX_CHARS));
        if (m_watermarkFontSize == 0) m_watermarkFontSize = GetWatermarkFontSize();
    }

    float Canvas::GetWatermarkFontSize() const {
        if (m_watermarkFontSize > 0) return m_watermarkFontSize;
        // 首次输入时固定文档字号，历史恢复后不能重新取当前窗口字体大小。
        if (ImGui::GetCurrentContext()) {
            const auto& io = ImGui::GetIO();
            const auto* font = io.FontDefault ? io.FontDefault : (io.Fonts->Fonts.empty() ? nullptr : io.Fonts->Fonts[0]);
            if (font && font->LegacySize > 0) return font->LegacySize;
        }
        return GetTextLayerFontSize() * m_documentRenderScale;
    }

    void Canvas::SetUIScale(const float scale) {
        if (!std::isfinite(scale)) return;
        m_uiScale = std::clamp(scale, 1.0f, 3.0f);
        if (!m_documentScaleLocked) m_documentRenderScale = m_uiScale;
        // DPI 变化不能作为字号编辑操作，也不能改写选中的旧文字。
        m_textFontSize = GetScaledTextFontSizeForIndex(m_textFontSizeIndex);
    }

    void Canvas::SetTextFontSizeIndex(const int index) {
        m_textFontSizeIndex = index;
        m_textFontSize = GetScaledTextFontSizeForIndex(index);
        if (m_selectedLayer && m_selectedLayer->SupportsFontSize()) {
            m_selectedLayer->SetFontSize(m_textFontSize);
            ConditionalRefreshTexture();
        }
        if (m_textInput.active) { m_textInput.fontSize = m_textFontSize; }
    }

    float Canvas::GetScaledTextFontSizeForIndex(int index) const {
        index = std::clamp(index, 0, TEXT_FONT_SIZE_COUNT - 1);
        return GetTextLayerFontSize() * m_documentRenderScale * TEXT_FONT_SIZE_RATIOS[index];
    }

    void Canvas::SetNumberSizeIndex(int index) {
        index = std::clamp(index, 0, NUMBER_SIZE_COUNT - 1);
        if (auto* marker = dynamic_cast<NumberLayer*>(m_selectedLayer)) {
            const float diameter = NUMBER_DIAMETER_OPTIONS[index] * m_documentRenderScale;
            const ImRect selection = GetSelectionRect();
            if (selection.GetWidth() < diameter || selection.GetHeight() < diameter) return;
            const float radius = diameter * 0.5f;
            const ImVec2 center = marker->GetCenter();
            marker->SetDiameter(diameter);
            marker->SetCenter(ImVec2(std::clamp(center.x, selection.Min.x + radius, selection.Max.x - radius),
                                    std::clamp(center.y, selection.Min.y + radius, selection.Max.y - radius)));
            RecalculateDrawnContentBounds();
        }
        m_numberSizeIndex = index;
    }

    int Canvas::GetNumberSizeIndex() const {
        const auto* marker = dynamic_cast<const NumberLayer*>(m_selectedLayer);
        if (!marker) return m_numberSizeIndex;
        int nearest = 0;
        for (int i = 1; i < NUMBER_SIZE_COUNT; ++i) {
            if (std::fabs(marker->GetDiameter() - NUMBER_DIAMETER_OPTIONS[i] * m_documentRenderScale) <
                std::fabs(marker->GetDiameter() - NUMBER_DIAMETER_OPTIONS[nearest] * m_documentRenderScale)) nearest = i;
        }
        return nearest;
    }

    void Canvas::SetScreenshot(const ImTextureID texture, const ImVec2 &pixelSize,
        const ImVec2 &displaySize, const unsigned char *imageData) {
        m_screenshot = texture;
        m_screenshotSize = pixelSize;
        m_displaySize = displaySize;

        if (imageData) {
            const int rowPitch = static_cast<int>(pixelSize.x) * 4;  // RGBA

            // 当前图像数据初始化为原始数据副本
            m_imageData = std::make_unique<unsigned char[]>(rowPitch * static_cast<int>(pixelSize.y));
            memcpy(m_imageData.get(), imageData, rowPitch * static_cast<int>(pixelSize.y));
            m_imageRowPitch = rowPitch;
        }

        // 窗口覆盖单个屏幕，无需居中偏移，offset = (0,0)
    }

    void Canvas::SetScreenshotOwned(const ImTextureID texture, const ImVec2 &pixelSize, const ImVec2 &displaySize,
                                    std::unique_ptr<unsigned char[]> imageData, const int rowPitch) {
        m_screenshot = texture;
        m_screenshotSize = pixelSize;
        m_displaySize = displaySize;

        if (imageData) {
            m_imageData = std::move(imageData);
            m_imageRowPitch = rowPitch;
        }

        // 窗口覆盖单个屏幕，无需居中偏移，offset = (0,0)
    }

    void Canvas::ReplaceScreenshotAndSelectAll(ImTextureID texture, const ImVec2& pixelSize,
        const ImVec2& displaySize, std::unique_ptr<unsigned char[]> data, int rowPitch) {
        Reset();
        ClearSmartRegion();
        SetScreenshotOwned(texture, pixelSize, displaySize, std::move(data), rowPitch);
        m_selectionStart = ImVec2(0, 0);
        m_selectionEnd = displaySize;
        m_state = SelectionState::Selected;
    }

    void Canvas::RotateImageCounterClockwise() {
        if (!m_imageEditing || !HasSelection()) return;
        FlushPendingInput();
        m_imageQuarterTurns = (m_imageQuarterTurns + 1) % 4;
    }

    void Canvas::Reset() {
        m_captureWindowLabel.clear();
        // 先清理借用的选中指针，再释放图层。
        ClearAllSelectedLayers();
        m_mouseDown = false;
        m_imageEditing = false;
        m_imageQuarterTurns = 0;
        m_documentScaleLocked = false;
        m_documentCropOrigin = {};
        m_sampleOffset = {};
        m_documentRenderScale = m_uiScale;
        m_mosaicGridOffsetX = m_mosaicGridOffsetY = 0;
        m_mosaicBlockSize = 18;
        m_state = SelectionState::None;
        m_selectionStart = ImVec2(0, 0);
        m_selectionEnd = ImVec2(0, 0);
        m_screenshot = 0;
        m_screenshotSize = ImVec2(0, 0);
        m_displaySize = ImVec2(0, 0);
        m_originalImageData.reset();
        m_imageData.reset();
        m_imageRowPitch = 0;
        m_mosaicImageData.reset();
        m_maskData.reset();
        m_editX = 0;
        m_editY = 0;
        m_editW = 0;
        m_editH = 0;
        m_hasLastMosaicPoint = false;
        m_activeHandle = ResizeHandle::None;
        m_layerManager.Clear();
        m_currentLayer.reset();
        m_currentTool = ToolType::None;
        m_highlightEllipse = false;
        m_highlightTransparency = HighlightSettings::DefaultTransparency;
        m_watermarkFontSize = 0;
        m_watermarkOrigin = {};
        m_textInput = {};
        m_reedit = {};
        ClearAllSelectedLayers();
        m_hasDrawnContent = false;
    }

    void Canvas::SetTool(const ToolType tool, const bool clearSelection) {
        if (tool != m_currentTool && dynamic_cast<NumberLayer*>(m_currentLayer.get())) {
            m_currentLayer.reset();
            m_mouseDown = false;
            m_state = SelectionState::Selected;
        }
        // 数字说明属于已创建的标注，切换工具时提交，不能留下隐藏文字的编辑状态。
        if (m_textInput.active && m_reedit.numberLabel && tool != m_currentTool) {
            CommitTextInput();
        } else if (m_textInput.active && !m_reedit.numberLabel && tool != ToolType::Text) {
            m_textInput.active = false;
            m_textInput.text.clear();
            if (m_state == SelectionState::Drawing) {
                m_state = SelectionState::Selected;
            }
        }
        // 切换到其他工具时清除选中状态，toggle off 当前工具时不清除
        if (clearSelection) {
            ClearAllSelectedLayers();
        }
        m_currentTool = tool;
        m_skipTextLayerSelect = false;
    }

    ToolType Canvas::GetPropertyTool() const {
        if (!m_selectedLayer) return m_currentTool;
        if (dynamic_cast<HighlightLayer*>(m_selectedLayer)) return ToolType::Highlight;
        if (dynamic_cast<RectangleLayer*>(m_selectedLayer)) return ToolType::Rectangle;
        if (dynamic_cast<EllipseLayer*>(m_selectedLayer)) return ToolType::Ellipse;
        if (dynamic_cast<ArrowLayer*>(m_selectedLayer)) return ToolType::Arrow;
        if (dynamic_cast<BrushLayer*>(m_selectedLayer)) return ToolType::Brush;
        if (dynamic_cast<TextLayer*>(m_selectedLayer)) return ToolType::Text;
        if (dynamic_cast<NumberLayer*>(m_selectedLayer)) return ToolType::Number;
        return ToolType::None;
    }

    ImU32 Canvas::GetSelectedLayerColor() const {
        if (const auto* marker = GetEditingNumberLayer()) return marker->GetColor();
        if (m_selectedLayer) return m_selectedLayer->GetColor();
        return m_toolColor;
    }

    void Canvas::SetSelectedLayerColor(const ImU32 color) {
        m_toolColor = color;
        // 说明框输入期间没有选中指针，调色仍应作用于正在编辑的数字标注。
        Layer* target = GetEditingNumberLayer();
        if (!target) target = m_selectedLayer;
        if (target) {
            target->SetColor(color);
            ConditionalRefreshTexture();
        }
    }

    float Canvas::GetSelectedLayerThickness() const {
        if (m_selectedLayer) return m_selectedLayer->GetThickness();
        return m_toolThickness;
    }

    void Canvas::SetSelectedLayerThickness(const float thickness) {
        m_toolThickness = thickness;
        if (m_selectedLayer) {
            m_selectedLayer->SetThickness(thickness);
            ConditionalRefreshTexture();
        }
    }

    bool Canvas::GetSelectedLayerFilled() const {
        if (m_selectedLayer && m_selectedLayer->SupportsFill()) return m_selectedLayer->IsFilled();
        return m_toolFill;
    }

    void Canvas::SetSelectedLayerFilled(const bool filled) {
        m_toolFill = filled;
        if (m_selectedLayer && m_selectedLayer->SupportsFill()) {
            m_selectedLayer->SetFilled(filled);
            ConditionalRefreshTexture();
        }
    }

    float Canvas::GetSelectedTextLayerFontSize() const {
        if (m_selectedLayer && m_selectedLayer->SupportsFontSize()) return m_selectedLayer->GetFontSize();
        return m_textFontSize;
    }

    void Canvas::ClearAllSelectedLayers() {
        if (m_selectedLayer) {
            m_selectedLayer->SetSelected(false);
            m_selectedLayer->EndDrag();
            m_selectedLayer->EndResize();
        }
        m_selectedLayer = nullptr;
        m_layerDragging = false;
    }

    void Canvas::SetSmartRegion(const SmartRegion* region) {
        if (HasSelection()) {
            m_smartRegion = nullptr;
            return;
        }
        m_smartRegion = region;
    }

    void Canvas::ClearSmartRegion() {
        m_smartRegion = nullptr;
    }

    void Canvas::Undo() {
        if (m_reedit.numberLabel) CommitTextInput();
        // 先清除选中状态再删除图层，避免访问已释放的图层指针
        ClearAllSelectedLayers();
        m_layerManager.Undo();
        RefreshTexture();
        RecalculateDrawnContentBounds();
    }

    void Canvas::FlushPendingInput() {
        // 提交未完成的文字输入
        if (m_textInput.active) {
            CommitTextInput();
        }
        // 提交正在绘制的图形
        if (m_currentLayer) {
            FinalizeCurrentLayer();
            m_state = SelectionState::Selected;
            m_hasLastMosaicPoint = false;
        }
    }

    void Canvas::FinalizeCurrentLayer() {
        if (!m_currentLayer) return;
        if (const auto* highlight = dynamic_cast<const HighlightLayer*>(m_currentLayer.get())) {
            const auto bounds = highlight->GetBounds();
            if (bounds.GetWidth() <= 0 || bounds.GetHeight() <= 0) { m_currentLayer.reset(); return; }
        }

        if (auto* circle = dynamic_cast<EllipseLayer*>(m_currentLayer.get())) {
            circle->SetShowBorder(false);
        } else if (auto* rect = dynamic_cast<RectangleLayer*>(m_currentLayer.get())) {
            rect->SetShowBorder(false);
        } else if (auto* arrow = dynamic_cast<ArrowLayer*>(m_currentLayer.get())) {
            arrow->SetShowHandles(false);
        }

        UpdateDrawnContentBounds(m_currentLayer.get());
        m_layerManager.AddLayer(std::move(m_currentLayer));
        m_currentLayer.reset();
    }

    ImRect Canvas::GetSelectionRect() const {
        const ImVec2 min(
            std::min(m_selectionStart.x, m_selectionEnd.x),
            std::min(m_selectionStart.y, m_selectionEnd.y)
        );
        const ImVec2 max(
            std::max(m_selectionStart.x, m_selectionEnd.x),
            std::max(m_selectionStart.y, m_selectionEnd.y)
        );
        return ImRect(min, max);
    }

    // 手柄点命中检测

    ResizeHandle Canvas::HitTestResizeHandle(const ImVec2 &pos) const {
        const ImRect sel = GetSelectionRect();
        const float h = GetSelectionHandleHitSize();

        // 角点
        if (ImRect(ImVec2(sel.Min.x - h, sel.Min.y - h), ImVec2(sel.Min.x + h, sel.Min.y + h)).Contains(pos))
            return ResizeHandle::TopLeft;
        if (ImRect(ImVec2(sel.Max.x - h, sel.Min.y - h), ImVec2(sel.Max.x + h, sel.Min.y + h)).Contains(pos))
            return ResizeHandle::TopRight;
        if (ImRect(ImVec2(sel.Min.x - h, sel.Max.y - h), ImVec2(sel.Min.x + h, sel.Max.y + h)).Contains(pos))
            return ResizeHandle::BottomLeft;
        if (ImRect(ImVec2(sel.Max.x - h, sel.Max.y - h), ImVec2(sel.Max.x + h, sel.Max.y + h)).Contains(pos))
            return ResizeHandle::BottomRight;

        // 边中点
        const float midX = (sel.Min.x + sel.Max.x) * 0.5f;
        const float midY = (sel.Min.y + sel.Max.y) * 0.5f;

        if (ImRect(ImVec2(midX - h, sel.Min.y - h), ImVec2(midX + h, sel.Min.y + h)).Contains(pos))
            return ResizeHandle::Top;
        if (ImRect(ImVec2(midX - h, sel.Max.y - h), ImVec2(midX + h, sel.Max.y + h)).Contains(pos))
            return ResizeHandle::Bottom;
        if (ImRect(ImVec2(sel.Min.x - h, midY - h), ImVec2(sel.Min.x + h, midY + h)).Contains(pos))
            return ResizeHandle::Left;
        if (ImRect(ImVec2(sel.Max.x - h, midY - h), ImVec2(sel.Max.x + h, midY + h)).Contains(pos))
            return ResizeHandle::Right;

        return ResizeHandle::None;
    }

    ImVec2 Canvas::ClampToCanvas(const ImVec2 &pos) const {
        ImVec2 result = pos;
        result.x = std::max(0.0f, std::min(result.x, m_displaySize.x));
        result.y = std::max(0.0f, std::min(result.y, m_displaySize.y));
        return result;
    }

    ImVec2 Canvas::ClampToSelection(const ImVec2 &pos) const {
        const ImRect sel = GetSelectionRect();
        ImVec2 result = pos;
        result.x = std::max(sel.Min.x, std::min(result.x, sel.Max.x));
        result.y = std::max(sel.Min.y, std::min(result.y, sel.Max.y));
        return result;
    }

    bool Canvas::ValidateSelectedLayer() {
        if (!m_selectedLayer) return false;

        // 检查指针是否仍在图层列表中
        bool stillExists = false;
        for (const auto& layer : m_layerManager.GetLayers()) {
            if (layer.get() == m_selectedLayer) {
                stillExists = true;
                break;
            }
        }
        if (!stillExists) {
            ClearAllSelectedLayers();
            return false;
        }
        return true;
    }

    void Canvas::UpdateDrawnContentBounds(const Layer* layer) {
        if (!layer) return;

        const ImRect bounds = layer->GetBounds();
        float expand = layer->GetThickness() * 0.5f;
        if (auto* mosaic = dynamic_cast<const MosaicLayer*>(layer)) {
            expand += mosaic->GetBounds().GetWidth() - bounds.GetWidth();
            expand += 15.0f;
        }
        if (dynamic_cast<const TextLayer*>(layer)) {
            expand += padding * GetTextFrameScale();
        }

        const ImVec2 screenMin(bounds.Min.x - expand, bounds.Min.y - expand);
        const ImVec2 screenMax(bounds.Max.x + expand, bounds.Max.y + expand);

        if (!m_hasDrawnContent) {
            m_drawnContentBounds = ImRect(screenMin, screenMax);
            m_hasDrawnContent = true;
        } else {
            m_drawnContentBounds.Min.x = std::min(m_drawnContentBounds.Min.x, screenMin.x);
            m_drawnContentBounds.Min.y = std::min(m_drawnContentBounds.Min.y, screenMin.y);
            m_drawnContentBounds.Max.x = std::max(m_drawnContentBounds.Max.x, screenMax.x);
            m_drawnContentBounds.Max.y = std::max(m_drawnContentBounds.Max.y, screenMax.y);
        }
    }

    void Canvas::RecalculateDrawnContentBounds() {
        m_hasDrawnContent = false;
        for (const auto& layer : m_layerManager.GetLayers()) {
            UpdateDrawnContentBounds(layer.get());
        }
    }

    ImRect Canvas::ConstrainSelectionToBounds(const ImRect& sel) const {
        if (!m_hasDrawnContent) return sel;

        ImRect result = sel;

        if (result.Min.x > m_drawnContentBounds.Min.x) {
            const float shift = result.Min.x - m_drawnContentBounds.Min.x;
            result.Min.x -= shift;
            result.Max.x -= shift;
        }
        if (result.Min.y > m_drawnContentBounds.Min.y) {
            const float shift = result.Min.y - m_drawnContentBounds.Min.y;
            result.Min.y -= shift;
            result.Max.y -= shift;
        }
        if (result.Max.x < m_drawnContentBounds.Max.x) {
            const float shift = m_drawnContentBounds.Max.x - result.Max.x;
            result.Min.x += shift;
            result.Max.x += shift;
        }
        if (result.Max.y < m_drawnContentBounds.Max.y) {
            const float shift = m_drawnContentBounds.Max.y - result.Max.y;
            result.Min.y += shift;
            result.Max.y += shift;
        }

        // 确保不超出画布范围
        result.Min.x = std::max(result.Min.x, 0.0f);
        result.Min.y = std::max(result.Min.y, 0.0f);
        result.Max.x = std::min(result.Max.x, m_displaySize.x);
        result.Max.y = std::min(result.Max.y, m_displaySize.y);

        return result;
    }
} // namespace shotlet
