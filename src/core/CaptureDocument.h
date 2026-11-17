#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace shotlet {

// 文档只包含数据，不持有窗口、字体、纹理或运行时图层对象。
struct DocumentPoint { float x = 0, y = 0; };
struct DocumentColor { uint8_t r = 0, g = 0, b = 0, a = 255; };
namespace WatermarkSettings {
inline constexpr int MaxCharacters = 10;
inline constexpr size_t MaxTextBytes = MaxCharacters * 4; // UTF-8 单字符最多 4 字节。
inline constexpr int MinimumOpacity = 0;
inline constexpr int MaximumOpacity = 100;
inline constexpr int DefaultOpacity = 30;
inline constexpr int LegacyOpacity = 38; // 旧版固定 alpha=96，38*255/100 仍为 96。
inline constexpr float MinimumFontSize = 1;
}
struct DocumentWatermark {
    std::string text;
    DocumentColor color;
    float fontSize = 0; // 文档逻辑坐标中的字号，不随恢复窗口的 DPI 变化。
    DocumentPoint origin; // 平铺网格原点；裁剪只平移原点，避免水印重新排布。
    int opacity = WatermarkSettings::DefaultOpacity; // 显示强度百分比，100 表示完全不透明。
};
struct DocumentRectangle { DocumentPoint start, end; bool filled = false; };
struct DocumentEllipse { DocumentPoint start, end; bool filled = false; };
struct DocumentArrow { DocumentPoint start, end; };
struct DocumentBrush { std::vector<DocumentPoint> points; };
struct DocumentMosaic { std::vector<DocumentPoint> points; float brushRadius = 10; };
struct DocumentText { DocumentPoint position; std::string text; float fontSize = 16, wrapWidth = 0; };
struct DocumentHighlight { DocumentPoint start, end; bool ellipse = false; };
namespace HighlightSettings {
inline constexpr int MinimumTransparency = 0;
inline constexpr int MaximumTransparency = 100;
inline constexpr int DefaultTransparency = 50;
}
struct DocumentNumber {
    DocumentPoint center;
    float diameter = 32;
    int number = 1;
    std::optional<DocumentText> label; // 旧历史没有此字段，恢复时保留原有纯数字圆点。
    float labelFrameScale = 1;
};
using DocumentLayerData = std::variant<DocumentRectangle, DocumentEllipse, DocumentArrow,
    DocumentBrush, DocumentMosaic, DocumentText, DocumentNumber, DocumentHighlight>;

struct DocumentLayer {
    uint64_t id = 0; // 文档内稳定标识；顺序由数组定义
    DocumentColor color;
    float thickness = 1;
    DocumentLayerData data;
};

struct DocumentMosaicCache {
    int x = 0, y = 0, width = 0, height = 0;
    std::vector<uint8_t> rgba; // 仅选区内的派生色块，不含选区外原图
};

struct CaptureDocument {
    static constexpr int FormatVersion = 1;
    static constexpr int RenderVersion = 2;
    static constexpr size_t MaxPixelBytes = 256u * 1024u * 1024u;
    static constexpr size_t MaxLayers = 10000;
    static constexpr size_t MaxPoints = 1000000;
    static constexpr size_t MaxTextBytes = 1024u * 1024u;

    int formatVersion = FormatVersion;
    int renderVersion = RenderVersion;
    int width = 0, height = 0; // 未旋转的选区像素尺寸
    DocumentPoint logicalSize;
    DocumentPoint pixelScale{1, 1};
    DocumentPoint sampleOffset; // 首个像素格原点相对选区原点的偏移（像素单位）
    DocumentPoint cropOrigin; // 原画布中的逻辑原点，仅用于追踪坐标变换
    float documentRenderScale = 1;
    int quarterTurns = 0;
    int highlightTransparency = HighlightSettings::DefaultTransparency;
    std::optional<DocumentWatermark> watermark; // 旧历史缺省为无水印。
    std::string source = "capture";
    std::string sourceRecordId;
    int64_t createdAtUnixMs = 0;
    std::vector<uint8_t> originalRgba;
    std::vector<DocumentLayer> layers;
    int mosaicBlockSize = 18; // 原始像素单位，全体马赛克共用
    int mosaicGridOffsetX = 0, mosaicGridOffsetY = 0; // 网格坐标 = 局部像素坐标 + 偏移
    std::optional<DocumentMosaicCache> mosaicCache;
};

// 所有尺寸乘法先验证；超出上限时不分配像素内存。
std::optional<size_t> DocumentPixelBytes(int width, int height);
bool ValidateCaptureDocument(const CaptureDocument& document, std::string& error, bool validatePixels = true);

} // namespace shotlet
