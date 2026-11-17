#include "Canvas.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <new>
#include <type_traits>

namespace shotlet {
namespace {
DocumentPoint Local(ImVec2 point, ImVec2 origin) { return {point.x - origin.x, point.y - origin.y}; }
ImVec2 Position(DocumentPoint point) { return ImVec2(point.x, point.y); }
DocumentColor Color(ImU32 color) {
    return {uint8_t(color >> IM_COL32_R_SHIFT), uint8_t(color >> IM_COL32_G_SHIFT),
            uint8_t(color >> IM_COL32_B_SHIFT), uint8_t(color >> IM_COL32_A_SHIFT)};
}
ImU32 Color(DocumentColor color) { return IM_COL32(color.r, color.g, color.b, color.a); }
std::vector<DocumentPoint> LocalPoints(const std::vector<ImVec2>& points, ImVec2 origin) {
    std::vector<DocumentPoint> result;
    result.reserve(points.size());
    for (auto point : points) result.push_back(Local(point, origin));
    return result;
}
std::vector<ImVec2> Positions(const std::vector<DocumentPoint>& points) {
    std::vector<ImVec2> result;
    result.reserve(points.size());
    for (auto point : points) result.push_back(Position(point));
    return result;
}
}

bool Canvas::GetDocumentCrop(int& x, int& y, int& width, int& height, std::string& error) const {
    error.clear();
    if (!HasSelection() || !m_imageData || !std::isfinite(m_displaySize.x) || !std::isfinite(m_displaySize.y) ||
        !std::isfinite(m_screenshotSize.x) || !std::isfinite(m_screenshotSize.y) ||
        m_screenshotSize.x <= 0 || m_screenshotSize.x > 10000000 ||
        m_screenshotSize.y <= 0 || m_screenshotSize.y > 10000000 ||
        m_displaySize.x <= 0 || m_displaySize.y <= 0 ||
        !DocumentPixelBytes(static_cast<int>(m_screenshotSize.x), static_cast<int>(m_screenshotSize.y)) ||
        m_imageRowPitch < static_cast<int>(m_screenshotSize.x) * 4) {
        error = "画布没有可导出的图像选区";
        return false;
    }
    if (m_currentLayer || m_textInput.active) {
        error = "请先提交尚未完成的绘制或文字输入";
        return false;
    }
    const ImRect selection = GetSelectionRect();
    const auto min = ToPixel(selection.Min), max = ToPixel(selection.Max);
    if (!std::isfinite(min.x) || !std::isfinite(min.y) || !std::isfinite(max.x) || !std::isfinite(max.y) ||
        selection.Min.x < 0 || selection.Min.y < 0 || selection.Max.x > m_displaySize.x || selection.Max.y > m_displaySize.y) {
        error = "文档选区超出已保存的图片";
        return false;
    }
    // 仅保留中心落在半开选区内的像素样本；边缘使用最近的区内样本延拓。
    // 不保存外扩像素，独立保留原像素比例和亚像素偏移，避免重采样整个文档。
    x = std::clamp(int(std::ceil(min.x - 0.5f)), 0, int(m_screenshotSize.x));
    y = std::clamp(int(std::ceil(min.y - 0.5f)), 0, int(m_screenshotSize.y));
    width = std::clamp(int(std::ceil(max.x - 0.5f)), x, int(m_screenshotSize.x)) - x;
    height = std::clamp(int(std::ceil(max.y - 0.5f)), y, int(m_screenshotSize.y)) - y;
    if (!DocumentPixelBytes(width, height) || x + width > m_screenshotSize.x || y + height > m_screenshotSize.y) {
        error = "文档选区尺寸无效或超出资源上限";
        return false;
    }
    return true;
}

std::optional<size_t> Canvas::EstimateDocumentSnapshotBytes(std::string& error) const {
    int x, y, width, height;
    if (!GetDocumentCrop(x, y, width, height, error)) return std::nullopt;
    const auto& layers = m_layerManager.GetLayers();
    if (layers.size() > CaptureDocument::MaxLayers) { error = "文档图层数量超限"; return std::nullopt; }
    size_t bytes = sizeof(CaptureDocument) + *DocumentPixelBytes(width, height) + layers.size() * sizeof(DocumentLayer);
    size_t points = 0, textBytes = m_watermarkText.size();
    for (const auto& layer : layers) {
        size_t count = 0;
        if (const auto* brush = dynamic_cast<const BrushLayer*>(layer.get())) count = brush->GetPoints().size();
        else if (const auto* mosaic = dynamic_cast<const MosaicLayer*>(layer.get())) count = mosaic->GetPoints().size();
        else if (const auto* text = dynamic_cast<const TextLayer*>(layer.get())) {
            if (text->GetText().size() > CaptureDocument::MaxTextBytes - textBytes) {
                error = "文档文字长度超限"; return std::nullopt;
            }
            textBytes += text->GetText().size();
        } else if (const auto* number = dynamic_cast<const NumberLayer*>(layer.get())) {
            if (number->GetLabel()) {
                const auto length = number->GetLabel()->GetText().size();
                if (length > CaptureDocument::MaxTextBytes - textBytes) { error = "文档文字长度超限"; return std::nullopt; }
                textBytes += length;
            }
        } else if (!dynamic_cast<const RectangleLayer*>(layer.get()) && !dynamic_cast<const EllipseLayer*>(layer.get()) &&
                   !dynamic_cast<const ArrowLayer*>(layer.get())) {
            error = "不支持的图层类型"; return std::nullopt;
        }
        if (count > CaptureDocument::MaxPoints - points) { error = "文档笔迹点数量超限"; return std::nullopt; }
        points += count;
    }
    bytes += points * sizeof(DocumentPoint) + textBytes;
    if (m_mosaicImageData) {
        const int w = std::min(x + width, m_editX + m_editW) - std::max(x, m_editX);
        const int h = std::min(y + height, m_editY + m_editH) - std::max(y, m_editY);
        if (w > 0 && h > 0) bytes += *DocumentPixelBytes(w, h);
    }
    return bytes;
}

std::optional<CaptureDocument> Canvas::ExportDocument(std::string& error) const {
    // 在任何像素、文字或点序列复制前检查资源上限，便于服务层提前领取预算。
    if (!EstimateDocumentSnapshotBytes(error)) return std::nullopt;
    int x, y, width, height;
    if (!GetDocumentCrop(x, y, width, height, error)) return std::nullopt;
    try {
        CaptureDocument d;
        d.width = width; d.height = height;
        const ImRect selection = GetSelectionRect();
        d.logicalSize = {selection.GetWidth(), selection.GetHeight()};
        const auto scale = GetPixelScale();
        const auto sample = ToPixel(selection.Min);
        d.pixelScale = {scale.x, scale.y};
        d.sampleOffset = {x - sample.x, y - sample.y};
        d.cropOrigin = {m_documentCropOrigin.x + selection.Min.x, m_documentCropOrigin.y + selection.Min.y};
        d.documentRenderScale = m_documentRenderScale;
        d.quarterTurns = m_imageQuarterTurns;
        d.createdAtUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        d.mosaicBlockSize = m_mosaicBlockSize;
        d.highlightTransparency = m_highlightTransparency;
        if (!m_watermarkText.empty()) {
            d.watermark = DocumentWatermark{m_watermarkText, Color(m_toolColor), GetWatermarkFontSize(),
                Local(m_watermarkOrigin, selection.Min), m_watermarkOpacity};
        }
        d.mosaicGridOffsetX = (m_mosaicGridOffsetX + x) % m_mosaicBlockSize;
        d.mosaicGridOffsetY = (m_mosaicGridOffsetY + y) % m_mosaicBlockSize;
        d.originalRgba.resize(*DocumentPixelBytes(width, height));
        for (int row = 0; row < height; ++row) {
            auto* dst = d.originalRgba.data() + size_t(row) * width * 4;
            std::memcpy(dst, m_imageData.get() + size_t(y + row) * m_imageRowPitch + size_t(x) * 4, size_t(width) * 4);
            // 马赛克已写进当前图像：从原始编辑缓冲覆盖相交部分，绝不修改活动 Canvas。
            const int left = std::max(x, m_editX), right = std::min(x + width, m_editX + m_editW);
            if (m_originalImageData && y + row >= m_editY && y + row < m_editY + m_editH && right > left)
                std::memcpy(dst + size_t(left - x) * 4,
                    m_originalImageData.get() + (size_t(y + row - m_editY) * m_editW + left - m_editX) * 4,
                    size_t(right - left) * 4);
        }
        if (m_mosaicImageData) {
            const int left = std::max(x, m_editX), top = std::max(y, m_editY);
            const int w = std::min(x + width, m_editX + m_editW) - left;
            const int h = std::min(y + height, m_editY + m_editH) - top;
            if (w > 0 && h > 0) {
                d.mosaicCache.emplace();
                auto& c = *d.mosaicCache;
                c.x = left - x; c.y = top - y; c.width = w; c.height = h;
                c.rgba.resize(*DocumentPixelBytes(w, h));
                for (int row = 0; row < h; ++row)
                    std::memcpy(c.rgba.data() + size_t(row) * w * 4,
                        m_mosaicImageData.get() + (size_t(top + row - m_editY) * m_editW + left - m_editX) * 4, size_t(w) * 4);
            }
        }
        d.layers.reserve(m_layerManager.GetLayers().size());
        for (const auto& layer : m_layerManager.GetLayers()) {
            DocumentLayer record;
            record.id = layer->GetId(); record.color = Color(layer->GetColor()); record.thickness = layer->GetThickness();
            if (const auto* p = dynamic_cast<const HighlightLayer*>(layer.get()))
                record.data = DocumentHighlight{Local(p->GetStart(), selection.Min), Local(p->GetEnd(), selection.Min), p->IsEllipse()};
            else if (const auto* p = dynamic_cast<const RectangleLayer*>(layer.get()))
                record.data = DocumentRectangle{Local(p->GetStart(), selection.Min), Local(p->GetEnd(), selection.Min), p->IsFilled()};
            else if (const auto* p = dynamic_cast<const EllipseLayer*>(layer.get()))
                record.data = DocumentEllipse{Local(p->GetStart(), selection.Min), Local(p->GetEnd(), selection.Min), p->IsFilled()};
            else if (const auto* p = dynamic_cast<const ArrowLayer*>(layer.get()))
                record.data = DocumentArrow{Local(p->GetStart(), selection.Min), Local(p->GetEnd(), selection.Min)};
            else if (const auto* p = dynamic_cast<const BrushLayer*>(layer.get()))
                record.data = DocumentBrush{LocalPoints(p->GetPoints(), selection.Min)};
            else if (const auto* p = dynamic_cast<const MosaicLayer*>(layer.get()))
                record.data = DocumentMosaic{LocalPoints(p->GetPoints(), selection.Min), p->GetBrushRadius()};
            else if (const auto* p = dynamic_cast<const TextLayer*>(layer.get()))
                record.data = DocumentText{Local(p->GetPosition(), selection.Min), p->GetText(), p->GetFontSize(), p->GetWrapWidth()};
            else if (const auto* p = dynamic_cast<const NumberLayer*>(layer.get())) {
                DocumentNumber number{Local(p->GetCenter(), selection.Min), p->GetDiameter(), p->GetNumber()};
                if (const auto* label = p->GetLabel()) {
                    number.label = DocumentText{Local(label->GetPosition(), selection.Min), label->GetText(),
                        label->GetFontSize(), label->GetWrapWidth()};
                    number.labelFrameScale = p->GetLabelFrameScale();
                }
                record.data = std::move(number);
            }
            d.layers.push_back(std::move(record));
        }
        if (!ValidateCaptureDocument(d, error)) return std::nullopt;
        return d;
    } catch (const std::bad_alloc&) {
        error = "可编辑快照内存分配失败";
        return std::nullopt;
    }
}

bool Canvas::RestoreDocument(const CaptureDocument& d, ImTextureID texture, std::string& error) {
    try {
        if (!ValidateCaptureDocument(d, error)) return false;
        if (!ImGui::GetCurrentContext() && std::any_of(d.layers.begin(), d.layers.end(), [](const DocumentLayer& layer) {
                const auto* number = std::get_if<DocumentNumber>(&layer.data);
                return std::holds_alternative<DocumentText>(layer.data) ||
                    (number && number->label && !number->label->text.empty());
            })) {
            error = "恢复文字图层需要已初始化的编辑器字体上下文";
            return false;
        }
        // 所有分配和重建先在临时画布上完成；失败不破坏当前正在编辑的内容。
        Canvas staged;
        if (d.watermark) staged.m_watermarkText = d.watermark->text;
        staged.m_screenshotSize = ImVec2(float(d.width), float(d.height));
        staged.m_displaySize = Position(d.logicalSize);
        staged.m_documentScaleLocked = true;
        staged.m_documentPixelScale = d.pixelScale;
        staged.m_sampleOffset = d.sampleOffset;
        staged.m_imageRowPitch = d.width * 4;
        staged.m_imageData = std::make_unique<unsigned char[]>(d.originalRgba.size());
        std::memcpy(staged.m_imageData.get(), d.originalRgba.data(), d.originalRgba.size());
        staged.m_selectionEnd = staged.m_displaySize;
        staged.m_state = SelectionState::Selected;
        staged.m_mosaicBlockSize = d.mosaicBlockSize;
        staged.m_mosaicGridOffsetX = d.mosaicGridOffsetX;
        staged.m_mosaicGridOffsetY = d.mosaicGridOffsetY;
        bool hasMosaic = false;
        for (const auto& record : d.layers) {
            auto layer = std::visit([&](const auto& data) -> std::unique_ptr<Layer> {
                using T = std::decay_t<decltype(data)>;
                if constexpr (std::is_same_v<T, DocumentRectangle> || std::is_same_v<T, DocumentEllipse>) {
                    using L = std::conditional_t<std::is_same_v<T, DocumentRectangle>, RectangleLayer, EllipseLayer>;
                    auto p = std::make_unique<L>(Position(data.start), Position(data.end), Color(record.color), record.thickness);
                    p->SetFilled(data.filled); p->SetShowBorder(false); return p;
                } else if constexpr (std::is_same_v<T, DocumentArrow>) {
                    auto p = std::make_unique<ArrowLayer>(Position(data.start), Position(data.end), Color(record.color), record.thickness);
                    p->SetShowHandles(false); return p;
                } else if constexpr (std::is_same_v<T, DocumentBrush>) {
                    return std::make_unique<BrushLayer>(Positions(data.points), Color(record.color), record.thickness);
                } else if constexpr (std::is_same_v<T, DocumentMosaic>) {
                    hasMosaic = true;
                    return std::make_unique<MosaicLayer>(Positions(data.points), d.mosaicBlockSize, data.brushRadius);
                } else if constexpr (std::is_same_v<T, DocumentText>) {
                    return std::make_unique<TextLayer>(Position(data.position), data.text, Color(record.color), data.fontSize, data.wrapWidth);
                } else if constexpr (std::is_same_v<T, DocumentHighlight>) {
                    return std::make_unique<HighlightLayer>(Position(data.start), Position(data.end), data.ellipse);
                } else {
                    auto number = std::make_unique<NumberLayer>(Position(data.center), data.number, Color(record.color), data.diameter);
                    if (data.label) number->SetLabel(Position(data.label->position), data.label->text,
                        data.label->fontSize, data.label->wrapWidth, data.labelFrameScale);
                    return number;
                }
            }, record.data);
            layer->m_id = record.id;
            layer->SetColor(Color(record.color)); layer->SetThickness(record.thickness);
            staged.m_layerManager.AddLayer(std::move(layer));
        }
        if (hasMosaic || d.mosaicCache) {
            const ImRect full(ImVec2(0, 0), staged.m_screenshotSize);
            staged.EnsureEditRegion(full);
            staged.EnsureMosaicBuffers();
            if (d.mosaicCache) {
                const auto& c = *d.mosaicCache;
                for (int row = 0; row < c.height; ++row)
                    std::memcpy(staged.m_mosaicImageData.get() + (size_t(c.y + row) * d.width + c.x) * 4,
                                c.rgba.data() + size_t(row) * c.width * 4, size_t(c.width) * 4);
            }
            staged.CompositeMosaicRegion(full);
        }
        // 文本边界依赖当前编辑器的字体；在提交前计算，内容渲染仍走现有 GPU 路径。
        staged.RecalculateDrawnContentBounds();
        Reset();
        ClearSmartRegion();
        m_screenshot = texture;
        m_screenshotSize = staged.m_screenshotSize; m_displaySize = staged.m_displaySize;
        m_imageData = std::move(staged.m_imageData); m_imageRowPitch = staged.m_imageRowPitch;
        m_originalImageData = std::move(staged.m_originalImageData);
        m_mosaicImageData = std::move(staged.m_mosaicImageData); m_maskData = std::move(staged.m_maskData);
        m_editX = staged.m_editX; m_editY = staged.m_editY; m_editW = staged.m_editW; m_editH = staged.m_editH;
        m_mosaicBlockSize = d.mosaicBlockSize;
        m_highlightTransparency = d.highlightTransparency;
        // 包含无水印旧文档的情况，必须覆盖当前画布残留的水印文字。
        m_watermarkText = std::move(staged.m_watermarkText);
        m_watermarkOpacity = d.watermark ? d.watermark->opacity : WatermarkSettings::DefaultOpacity;
        if (d.watermark) {
            m_toolColor = Color(d.watermark->color);
            m_watermarkFontSize = d.watermark->fontSize;
            m_watermarkOrigin = Position(d.watermark->origin);
        }
        m_mosaicGridOffsetX = d.mosaicGridOffsetX; m_mosaicGridOffsetY = d.mosaicGridOffsetY;
        m_layerManager = std::move(staged.m_layerManager);
        m_documentRenderScale = d.documentRenderScale; m_documentScaleLocked = true;
        m_documentCropOrigin = d.cropOrigin;
        m_documentPixelScale = d.pixelScale; m_sampleOffset = d.sampleOffset;
        m_textFontSize = GetScaledTextFontSizeForIndex(m_textFontSizeIndex);
        m_selectionEnd = m_displaySize; m_state = SelectionState::Selected;
        m_imageEditing = true; m_imageQuarterTurns = d.quarterTurns;
        m_hasDrawnContent = staged.m_hasDrawnContent; m_drawnContentBounds = staged.m_drawnContentBounds;
        // 恢复完成后只上传一次，避免把最终成图当底图再次打码。
        UpdateTexture();
        return true;
    } catch (const std::bad_alloc&) {
        error = "恢复文档内存分配失败";
        return false;
    }
}

std::optional<CaptureDocument> Canvas::ExportCroppedDocument(ImRect crop, std::string& error) {
    if (!m_imageEditing || crop.Min.x < 0 || crop.Min.y < 0 || crop.Max.x > m_displaySize.x || crop.Max.y > m_displaySize.y ||
        !std::isfinite(crop.Min.x) || !std::isfinite(crop.Min.y) || !std::isfinite(crop.Max.x) || !std::isfinite(crop.Max.y) ||
        crop.GetWidth() <= 0 || crop.GetHeight() <= 0) { error = "裁剪区域无效"; return {}; }
    FlushPendingInput();
    const auto start = m_selectionStart, end = m_selectionEnd;
    m_selectionStart = crop.Min; m_selectionEnd = crop.Max;
    try {
        auto document = ExportDocument(error);
        m_selectionStart = start; m_selectionEnd = end;
        return document;
    } catch (...) { m_selectionStart = start; m_selectionEnd = end; throw; }
}

bool Canvas::SelectLayerById(uint64_t id) {
    FlushPendingInput();
    for (const auto& layer : m_layerManager.GetLayers()) {
        if (layer->GetId() != id) continue;
        ClearAllSelectedLayers(); m_selectedLayer = layer.get(); m_selectedLayer->SetSelected(true);
        if (dynamic_cast<MosaicLayer*>(m_selectedLayer)) m_currentTool = ToolType::None;
        return true;
    }
    return false;
}

bool Canvas::MoveMosaicLayer(uint64_t id, const ImVec2& delta) {
    if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || std::abs(delta.x) > m_displaySize.x ||
        std::abs(delta.y) > m_displaySize.y) return false;
    for (const auto& layer : m_layerManager.GetLayers()) {
        if (layer->GetId() != id) continue;
        auto* mosaic = dynamic_cast<MosaicLayer*>(layer.get());
        if (!mosaic) return false;
        mosaic->OffsetAllPoints(delta);
        RefreshTexture(); RecalculateDrawnContentBounds(); return true;
    }
    return false;
}

bool Canvas::SetMosaicLayerBrushRadius(uint64_t id, float radius) {
    if (!std::isfinite(radius) || radius <= 0 || radius > 1024) return false;
    for (const auto& layer : m_layerManager.GetLayers()) {
        if (layer->GetId() != id) continue;
        auto* mosaic = dynamic_cast<MosaicLayer*>(layer.get());
        if (!mosaic) return false;
        mosaic->SetBrushRadius(radius);
        RefreshTexture(); RecalculateDrawnContentBounds(); return true;
    }
    return false;
}

bool Canvas::DeleteLayerById(uint64_t id) {
    if (m_reedit.numberLabel && m_reedit.layerId == id) CommitTextInput();
    for (const auto& layer : m_layerManager.GetLayers()) {
        if (layer->GetId() != id) continue;
        ClearAllSelectedLayers();
        m_layerManager.RemoveLayer(layer.get());
        RefreshTexture(); RecalculateDrawnContentBounds(); return true;
    }
    return false;
}

void Canvas::SetMosaicBlockSize(int pixels) {
    if (pixels < 1 || pixels > 4096 || pixels == m_mosaicBlockSize || !m_imageData) return;
    FlushPendingInput();
    EnsureEditRegion(ComputeRefreshRegionPx());
    m_mosaicBlockSize = pixels;
    m_mosaicGridOffsetX %= pixels; m_mosaicGridOffsetY %= pixels;
    m_mosaicImageData.reset(); m_maskData.reset();
    EnsureMosaicBuffers();
    RefreshTexture();
}
} // namespace shotlet
