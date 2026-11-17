#include "CaptureDocument.h"
#include "utils/StringUtils.h"
#include <cmath>
#include <unordered_set>
#include <type_traits>

namespace shotlet {
namespace {
constexpr float MinRenderScale = 1;
constexpr float MaxRenderScale = 3;
}
std::optional<size_t> DocumentPixelBytes(int width, int height) {
    if (width <= 0 || height <= 0 || size_t(width) > CaptureDocument::MaxPixelBytes / 4 / size_t(height))
        return std::nullopt;
    return size_t(width) * size_t(height) * 4;
}

bool ValidateCaptureDocument(const CaptureDocument& d, std::string& error, bool validatePixels) {
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    if (d.source.size() > 64 || d.sourceRecordId.size() > 80) return fail("文档来源信息超限");
    const auto point = [](DocumentPoint p) {
        return std::isfinite(p.x) && std::isfinite(p.y) && std::abs(p.x) <= 10000000 && std::abs(p.y) <= 10000000;
    };
    const auto positive = [](float v) { return std::isfinite(v) && v > 0 && v <= 1000000; };
    if (d.formatVersion != CaptureDocument::FormatVersion || d.renderVersion != CaptureDocument::RenderVersion)
        return fail("不支持的文档或渲染版本");
    const auto bytes = DocumentPixelBytes(d.width, d.height);
    if (!bytes || (validatePixels && d.originalRgba.size() != *bytes)) return fail("原图尺寸或像素缓冲无效");
    if (!point(d.logicalSize) || !positive(d.logicalSize.x) || !positive(d.logicalSize.y) || !point(d.cropOrigin) ||
        !std::isfinite(d.documentRenderScale) || d.documentRenderScale < MinRenderScale || d.documentRenderScale > MaxRenderScale ||
        d.quarterTurns < 0 || d.quarterTurns > 3) return fail("文档坐标或缩放无效");
    const float sx = d.pixelScale.x, sy = d.pixelScale.y;
    if (!point(d.sampleOffset) || d.sampleOffset.x < -0.5f || d.sampleOffset.x >= 0.5f || d.sampleOffset.y < -0.5f || d.sampleOffset.y >= 0.5f ||
        !std::isfinite(sx) || !std::isfinite(sy)) return fail("像素采样偏移无效");
    if (sx < 1.0f / 64 || sx > 64 || sy < 1.0f / 64 || sy > 64)
        return fail("像素与逻辑坐标比例超出支持范围");
    if (int(std::ceil(d.logicalSize.x * sx - d.sampleOffset.x - .5f)) != d.width ||
        int(std::ceil(d.logicalSize.y * sy - d.sampleOffset.y - .5f)) != d.height)
        return fail("图像尺寸与采样网格不一致");
    if (d.mosaicBlockSize < 1 || d.mosaicBlockSize > 4096 ||
        d.mosaicGridOffsetX < 0 || d.mosaicGridOffsetX >= d.mosaicBlockSize ||
        d.mosaicGridOffsetY < 0 || d.mosaicGridOffsetY >= d.mosaicBlockSize)
        return fail("马赛克网格参数无效");
    if (d.mosaicCache) {
        const auto& c = *d.mosaicCache;
        const auto cacheBytes = DocumentPixelBytes(c.width, c.height);
        if (!cacheBytes || c.x < 0 || c.y < 0 || c.width > d.width || c.height > d.height ||
            c.x > d.width - c.width || c.y > d.height - c.height || (validatePixels && c.rgba.size() != *cacheBytes))
            return fail("马赛克缓存越界或损坏");
    }
    if (d.layers.size() > CaptureDocument::MaxLayers) return fail("文档图层数量超限");
    if (d.highlightTransparency < HighlightSettings::MinimumTransparency ||
        d.highlightTransparency > HighlightSettings::MaximumTransparency) return fail("高亮透明度无效");
    std::unordered_set<uint64_t> ids;
    size_t totalPoints = 0, totalText = 0;
    if (d.watermark) {
        const auto& watermark = *d.watermark;
        if (watermark.text.empty() || watermark.text.size() > WatermarkSettings::MaxTextBytes ||
            watermark.text.find('\0') != std::string::npos ||
            StringUtils::Utf8PrefixBytes(watermark.text, WatermarkSettings::MaxCharacters) != watermark.text.size() ||
            !positive(watermark.fontSize) || watermark.fontSize < WatermarkSettings::MinimumFontSize ||
            watermark.opacity < WatermarkSettings::MinimumOpacity || watermark.opacity > WatermarkSettings::MaximumOpacity ||
            !point(watermark.origin)) return fail("水印参数无效");
        totalText = watermark.text.size();
    }
    for (const auto& layer : d.layers) {
        const bool validThickness = positive(layer.thickness) ||
            (std::holds_alternative<DocumentNumber>(layer.data) && layer.thickness == 0);
        if (layer.id == 0 || layer.id == UINT64_MAX || !ids.insert(layer.id).second || !validThickness)
            return fail("图层标识或粗细无效");
        const bool valid = std::visit([&](const auto& data) {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, DocumentRectangle> || std::is_same_v<T, DocumentEllipse> ||
                          std::is_same_v<T, DocumentArrow>) {
                return point(data.start) && point(data.end);
            } else if constexpr (std::is_same_v<T, DocumentBrush> || std::is_same_v<T, DocumentMosaic>) {
                if (data.points.empty() || data.points.size() > CaptureDocument::MaxPoints - totalPoints) return false;
                totalPoints += data.points.size();
                for (auto p : data.points) if (!point(p)) return false;
                if constexpr (std::is_same_v<T, DocumentMosaic>) return positive(data.brushRadius) && data.brushRadius <= 1024;
                return true;
            } else if constexpr (std::is_same_v<T, DocumentText>) {
                if (data.text.size() > CaptureDocument::MaxTextBytes - totalText) return false;
                totalText += data.text.size();
                return point(data.position) && positive(data.fontSize) && std::isfinite(data.wrapWidth) &&
                    data.wrapWidth >= 0 && data.wrapWidth <= 1000000;
            } else if constexpr (std::is_same_v<T, DocumentHighlight>) {
                return point(data.start) && point(data.end) && data.start.x != data.end.x && data.start.y != data.end.y;
            } else {
                if (!point(data.center) || !positive(data.diameter) || data.number <= 0) return false;
                if (!data.label) return true;
                const auto& label = *data.label;
                if (label.text.size() > CaptureDocument::MaxTextBytes - totalText) return false;
                totalText += label.text.size();
                return point(label.position) && positive(label.fontSize) && positive(label.wrapWidth) &&
                    std::isfinite(data.labelFrameScale) && data.labelFrameScale >= MinRenderScale && data.labelFrameScale <= MaxRenderScale;
            }
        }, layer.data);
        if (!valid) return fail("图层参数无效或资源超限");
    }
    return true;
}
} // namespace shotlet
