#include "DocumentCodec.h"
#include "third_party/nlohmann/json.hpp"
#include <SDL3/SDL.h>
#include <atomic>
#include <chrono>
#include <random>
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <type_traits>
#include <climits>
#include <cstdio>
#include <algorithm>
#include <iterator>

namespace shotlet {
namespace {
using Json = nlohmann::json;
Json Point(DocumentPoint p) { return Json::array({p.x, p.y}); }
DocumentPoint Point(const Json& j) {
    if (!j.is_array() || j.size() != 2 || !j[0].is_number() || !j[1].is_number()) throw std::runtime_error("无效坐标");
    return {j.at(0).get<float>(), j.at(1).get<float>()};
}
int Integer(const Json& j) {
    if (!j.is_number_integer() || j.get<double>() < INT_MIN || j.get<double>() > INT_MAX) throw std::runtime_error("无效整数");
    return j.get<int>();
}
Json Color(DocumentColor color) {
    return {{"r", color.r}, {"g", color.g}, {"b", color.b}, {"a", color.a}};
}
DocumentColor Color(const Json& value) {
    if (!value.is_object()) throw std::runtime_error("颜色无效");
    uint8_t channels[4];
    constexpr const char* keys[] = {"r", "g", "b", "a"};
    for (size_t i = 0; i < std::size(keys); ++i) {
        const int channel = Integer(value.at(keys[i]));
        if (channel < 0 || channel > UINT8_MAX) throw std::runtime_error("颜色通道越界");
        channels[i] = static_cast<uint8_t>(channel);
    }
    return {channels[0], channels[1], channels[2], channels[3]};
}
Json Parse(std::string_view value) {
    if (value.size() > 64u * 1024u * 1024u) throw std::runtime_error("文档 JSON 超过 64 MiB");
    return Json::parse(value, [](int depth, Json::parse_event_t, Json&) {
        if (depth > 32) throw std::runtime_error("JSON 嵌套过深");
        return true;
    });
}
Json Points(const std::vector<DocumentPoint>& points) {
    auto j = Json::array();
    for (auto p : points) j.push_back(Point(p));
    return j;
}
std::vector<DocumentPoint> Points(const Json& j, size_t& total) {
    if (!j.is_array() || j.size() > CaptureDocument::MaxPoints - total) throw std::runtime_error("笔迹点数量超限");
    total += j.size();
    std::vector<DocumentPoint> points;
    points.reserve(j.size()); for (const auto& p : j) points.push_back(Point(p));
    return points;
}
}
std::string NewHistoryId() {
    static std::atomic<uint64_t> serial{0};
    std::random_device random;
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    char id[80];
    std::snprintf(id, sizeof(id), "%016llx-%08x%08x-%016llx", static_cast<unsigned long long>(now),
        random(), random(), static_cast<unsigned long long>(++serial));
    return id;
}
std::string EncodeDocument(const CaptureDocument& d) {
    Json j{{"formatVersion", d.formatVersion}, {"renderVersion", d.renderVersion}, {"width", d.width}, {"height", d.height},
        {"logicalSize", Point(d.logicalSize)}, {"pixelScale", Point(d.pixelScale)}, {"sampleOffset", Point(d.sampleOffset)},
        {"cropOrigin", Point(d.cropOrigin)}, {"documentRenderScale", d.documentRenderScale}, {"quarterTurns", d.quarterTurns},
        {"source", d.source}, {"sourceRecordId", d.sourceRecordId}, {"createdAtUnixMs", d.createdAtUnixMs},
        {"mosaicBlockSize", d.mosaicBlockSize}, {"mosaicGridOffsetX", d.mosaicGridOffsetX}, {"mosaicGridOffsetY", d.mosaicGridOffsetY}};
    j["layers"] = Json::array();
    j["highlightTransparency"] = d.highlightTransparency;
    if (d.watermark) {
        const auto& watermark = *d.watermark;
        j["watermark"] = {{"text", watermark.text}, {"color", Color(watermark.color)},
            {"fontSize", watermark.fontSize}, {"origin", Point(watermark.origin)}, {"opacity", watermark.opacity}};
    }
    for (const auto& layer : d.layers) {
        Json item{{"id", std::to_string(layer.id)}, {"color", Color(layer.color)}, {"thickness", layer.thickness}};
        std::visit([&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, DocumentRectangle> || std::is_same_v<T, DocumentEllipse> || std::is_same_v<T, DocumentArrow>) {
                item["type"] = std::is_same_v<T, DocumentRectangle> ? "rectangle" : std::is_same_v<T, DocumentEllipse> ? "ellipse" : "arrow";
                item["start"] = Point(p.start); item["end"] = Point(p.end);
                if constexpr (!std::is_same_v<T, DocumentArrow>) item["filled"] = p.filled;
            } else if constexpr (std::is_same_v<T, DocumentBrush> || std::is_same_v<T, DocumentMosaic>) {
                item["type"] = std::is_same_v<T, DocumentBrush> ? "brush" : "mosaic"; item["points"] = Points(p.points);
                if constexpr (std::is_same_v<T, DocumentMosaic>) item["brushRadius"] = p.brushRadius;
            } else if constexpr (std::is_same_v<T, DocumentText>) {
                item["type"] = "text"; item["position"] = Point(p.position); item["text"] = p.text;
                item["fontSize"] = p.fontSize; item["wrapWidth"] = p.wrapWidth;
            } else if constexpr (std::is_same_v<T, DocumentHighlight>) {
                item["type"] = "highlight"; item["start"] = Point(p.start); item["end"] = Point(p.end); item["ellipse"] = p.ellipse;
            } else {
                item["type"] = "number"; item["center"] = Point(p.center); item["diameter"] = p.diameter; item["number"] = p.number;
                if (p.label) item["label"] = {{"position", Point(p.label->position)}, {"text", p.label->text},
                    {"fontSize", p.label->fontSize}, {"wrapWidth", p.label->wrapWidth}, {"frameScale", p.labelFrameScale}};
            }
        }, layer.data);
        j["layers"].push_back(std::move(item));
    }
    if (d.mosaicCache) {
        const auto& c = *d.mosaicCache;
        j["mosaicCache"] = {{"x", c.x}, {"y", c.y}, {"width", c.width}, {"height", c.height}};
    }
    return j.dump();
}
CaptureDocument DecodeDocument(std::string_view text) {
    const auto j = Parse(text);
    CaptureDocument d;
    d.formatVersion = Integer(j.at("formatVersion")); d.renderVersion = Integer(j.at("renderVersion"));
    if (d.formatVersion != CaptureDocument::FormatVersion || d.renderVersion != CaptureDocument::RenderVersion)
        throw std::runtime_error("文档版本不受支持，只能浏览最终图片");
    d.width = Integer(j.at("width")); d.height = Integer(j.at("height"));
    if (!DocumentPixelBytes(d.width, d.height)) throw std::runtime_error("原图尺寸超限");
    d.logicalSize = Point(j.at("logicalSize")); d.pixelScale = Point(j.at("pixelScale")); d.sampleOffset = Point(j.at("sampleOffset"));
    d.cropOrigin = Point(j.at("cropOrigin")); d.documentRenderScale = j.at("documentRenderScale").get<float>();
    d.quarterTurns = Integer(j.at("quarterTurns")); d.source = j.at("source").get<std::string>();
    d.sourceRecordId = j.at("sourceRecordId").get<std::string>(); d.createdAtUnixMs = j.at("createdAtUnixMs").get<int64_t>();
    d.mosaicBlockSize = Integer(j.at("mosaicBlockSize")); d.mosaicGridOffsetX = Integer(j.at("mosaicGridOffsetX"));
    d.mosaicGridOffsetY = Integer(j.at("mosaicGridOffsetY"));
    if (j.contains("highlightTransparency")) d.highlightTransparency = Integer(j.at("highlightTransparency"));
    if (j.contains("watermark")) {
        const auto& watermark = j.at("watermark");
        d.watermark = DocumentWatermark{watermark.at("text").get<std::string>(), Color(watermark.at("color")),
            watermark.at("fontSize").get<float>(), Point(watermark.at("origin"))};
        // 缺少新字段的历史继续使用旧版固定透明度，不能套用新建水印的默认值。
        d.watermark->opacity = watermark.contains("opacity") ? Integer(watermark.at("opacity")) : WatermarkSettings::LegacyOpacity;
    }
    const auto& layers = j.at("layers");
    if (!layers.is_array() || layers.size() > CaptureDocument::MaxLayers) throw std::runtime_error("图层数量超限");
    size_t pointCount = 0, textBytes = d.watermark ? d.watermark->text.size() : 0;
    if (textBytes > WatermarkSettings::MaxTextBytes) throw std::runtime_error("水印文字超限");
    for (const auto& item : layers) {
        DocumentLayer layer;
        auto id = item.at("id").get<std::string>();
        if (id.empty() || id.size() > 20 || id.find_first_not_of("0123456789") != std::string::npos) throw std::runtime_error("图层 ID 无效");
        layer.id = std::stoull(id);
        layer.color = Color(item.at("color")); layer.thickness = item.at("thickness").get<float>();
        const auto type = item.at("type").get<std::string>();
        if (type == "rectangle") layer.data = DocumentRectangle{Point(item.at("start")), Point(item.at("end")), item.at("filled").get<bool>()};
        else if (type == "ellipse") layer.data = DocumentEllipse{Point(item.at("start")), Point(item.at("end")), item.at("filled").get<bool>()};
        else if (type == "arrow") layer.data = DocumentArrow{Point(item.at("start")), Point(item.at("end"))};
        else if (type == "brush") layer.data = DocumentBrush{Points(item.at("points"), pointCount)};
        else if (type == "mosaic") layer.data = DocumentMosaic{Points(item.at("points"), pointCount), item.at("brushRadius").get<float>()};
        else if (type == "highlight") layer.data = DocumentHighlight{Point(item.at("start")), Point(item.at("end")), item.at("ellipse").get<bool>()};
        else if (type == "text") {
            auto value = item.at("text").get<std::string>();
            if (value.size() > CaptureDocument::MaxTextBytes - textBytes) throw std::runtime_error("文字长度超限");
            textBytes += value.size();
            layer.data = DocumentText{Point(item.at("position")), std::move(value), item.at("fontSize").get<float>(), item.at("wrapWidth").get<float>()};
        } else if (type == "number") {
            DocumentNumber number{Point(item.at("center")), item.at("diameter").get<float>(), Integer(item.at("number"))};
            if (item.contains("label")) {
                const auto& label = item.at("label");
                auto value = label.at("text").get<std::string>();
                if (value.size() > CaptureDocument::MaxTextBytes - textBytes) throw std::runtime_error("文字长度超限");
                textBytes += value.size();
                number.label = DocumentText{Point(label.at("position")), std::move(value),
                    label.at("fontSize").get<float>(), label.at("wrapWidth").get<float>()};
                number.labelFrameScale = label.at("frameScale").get<float>();
            }
            layer.data = std::move(number);
        }
        else throw std::runtime_error("存在未知图层，禁止恢复编辑");
        d.layers.push_back(std::move(layer));
    }
    if (j.contains("mosaicCache")) {
        const auto& c = j.at("mosaicCache"); d.mosaicCache.emplace();
        d.mosaicCache->x = Integer(c.at("x")); d.mosaicCache->y = Integer(c.at("y"));
        d.mosaicCache->width = Integer(c.at("width")); d.mosaicCache->height = Integer(c.at("height"));
    }
    std::string error;
    if (!ValidateCaptureDocument(d, error, false)) throw std::runtime_error(error);
    return d;
}
std::string EncodeHistoryMetadata(const HistoryMetadata& m) {
    return Json{{"id", m.id}, {"action", m.action}, {"sourceRecordId", m.sourceRecordId}, {"createdAtUnixMs", m.createdAtUnixMs},
        {"width", m.width}, {"height", m.height}, {"bytes", m.bytes}, {"formatVersion", m.formatVersion}, {"renderVersion", m.renderVersion},
        {"title", m.details.title}, {"captureType", m.details.captureType}, {"sourceApplication", m.details.sourceApplication},
        {"sourceWindow", m.details.sourceWindow}, {"tags", m.details.tags}, {"favorite", m.details.favorite},
        {"ocrIndexed", m.details.ocrIndexed}, {"ocrText", m.details.ocrText}, {"ocrError", m.details.ocrError},
        {"deletedAtUnixMs", m.details.deletedAtUnixMs}}.dump();
}
HistoryMetadata DecodeHistoryMetadata(std::string_view text) {
    auto j = Parse(text); HistoryMetadata m;
    m.id = j.at("id").get<std::string>(); m.action = j.at("action").get<std::string>(); m.sourceRecordId = j.at("sourceRecordId").get<std::string>();
    m.createdAtUnixMs = j.at("createdAtUnixMs").get<int64_t>(); m.width = Integer(j.at("width")); m.height = Integer(j.at("height"));
    if (!DocumentPixelBytes(m.width, m.height)) throw std::runtime_error("成图尺寸无效");
    m.bytes = j.at("bytes").get<uint64_t>(); m.formatVersion = Integer(j.at("formatVersion")); m.renderVersion = Integer(j.at("renderVersion"));
    auto& d = m.details;
    d.title = j.value("title", std::string()); d.captureType = j.value("captureType", std::string("unknown"));
    d.sourceApplication = j.value("sourceApplication", std::string()); d.sourceWindow = j.value("sourceWindow", std::string());
    d.tags = j.value("tags", std::vector<std::string>()); d.favorite = j.value("favorite", false);
    d.ocrIndexed = j.value("ocrIndexed", false); d.ocrText = j.value("ocrText", std::string()); d.ocrError = j.value("ocrError", std::string());
    d.deletedAtUnixMs = j.value("deletedAtUnixMs", int64_t(0));
    if (d.title.size() > 512 || d.sourceApplication.size() > 1024 || d.sourceWindow.size() > 4096 ||
        d.ocrText.size() > HistoryDetails::MaxOcrTextBytes || d.ocrError.size() > 2048 || d.tags.size() > 32 || d.deletedAtUnixMs < 0)
        throw std::runtime_error("历史属性超过限制");
    if (d.captureType != "unknown" && d.captureType != "region" && d.captureType != "window" &&
        d.captureType != "fullscreen" && d.captureType != "scroll") throw std::runtime_error("截图类型无效");
    std::vector<std::string> tags;
    for (const auto& tag : d.tags) {
        if (tag.empty() || tag.size() > 128) throw std::runtime_error("标签长度无效");
        if (std::find(tags.begin(), tags.end(), tag) == tags.end()) tags.push_back(tag);
    }
    d.tags = std::move(tags);
    return m;
}
std::string EncodeHistoryConfig(const HistoryConfig& c) { return Json{{"enabled", c.enabled}, {"generation", c.generation}}.dump(); }
HistoryConfig DecodeHistoryConfig(std::string_view text) {
    auto j = Parse(text); HistoryConfig c{j.at("enabled").get<bool>(), j.at("generation").get<std::string>()};
    if (c.generation.empty() || c.generation.size() > 128) throw std::runtime_error("历史配置代次无效");
    return c;
}

bool WriteHistoryPng(const std::filesystem::path& file, const unsigned char* data, int w, int h, std::string& error) {
    if (!data || !DocumentPixelBytes(w, h) || w > 65535 || h > 65535) { error = "PNG 像素或尺寸无效"; return false; }
    auto* surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, const_cast<unsigned char*>(data), w * 4);
    if (!surface) { error = SDL_GetError(); return false; }
    const auto name = file.u8string();
    bool ok = SDL_SavePNG(surface, reinterpret_cast<const char*>(name.c_str()));
    SDL_DestroySurface(surface);
    // SDL 的编码函数可能把短写误判成成功，再检查结尾 IEND 完整性。
    std::ifstream input(file, std::ios::binary); char tail[12]{};
    if (input) { input.seekg(-12, std::ios::end); input.read(tail, 12); }
    ok = ok && input && std::memcmp(tail + 4, "IEND", 4) == 0;
    if (!ok) error = "PNG 写入失败或文件不完整";
    return ok;
}
HistoryImage ReadHistoryPng(const std::filesystem::path& file, std::string& error, int expectedWidth, int expectedHeight, size_t maxBytes) {
    try {
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(file))) throw std::runtime_error("图片资产缺失");
        std::ifstream input(file, std::ios::binary); unsigned char header[24]{}; input.read(reinterpret_cast<char*>(header), sizeof(header));
        const unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
        if (!input || std::memcmp(header, signature, 8) || std::memcmp(header + 12, "IHDR", 4)) throw std::runtime_error("PNG 文件头损坏");
        auto number = [](const unsigned char* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; };
        const uint32_t w = number(header + 16), h = number(header + 20);
        const auto bytes = w <= INT_MAX && h <= INT_MAX ? DocumentPixelBytes(int(w),int(h)) : std::nullopt;
        if (!bytes || *bytes > maxBytes || (expectedWidth && w != uint32_t(expectedWidth)) ||
            (expectedHeight && h != uint32_t(expectedHeight))) throw std::runtime_error("PNG 尺寸超限或与记录不一致");
        const auto name = file.u8string();
        std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> source(SDL_LoadPNG(reinterpret_cast<const char*>(name.c_str())), SDL_DestroySurface);
        if (!source) throw std::runtime_error(SDL_GetError());
        std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> converted(SDL_ConvertSurface(source.get(), SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
        if (!converted || converted->w != int(w) || converted->h != int(h)) throw std::runtime_error("PNG 解码尺寸无效");
        auto pixels = std::shared_ptr<unsigned char>(new unsigned char[size_t(w) * h * 4], std::default_delete<unsigned char[]>());
        for (uint32_t y = 0; y < h; ++y) std::memcpy(pixels.get() + size_t(y) * w * 4,
            static_cast<const unsigned char*>(converted->pixels) + size_t(y) * converted->pitch, size_t(w) * 4);
        return {std::move(pixels), int(w), int(h)};
    } catch (const std::exception& e) { error = e.what(); return {}; }
}
HistoryImage MakeHistoryThumbnail(const HistoryImage& image, int maxWidth, int maxHeight) {
    if (!image || maxWidth <= 0 || maxHeight <= 0) return {};
    const double scale = std::min({1.0, double(maxWidth) / image.width, double(maxHeight) / image.height});
    const int w = std::max(1, int(image.width * scale)), h = std::max(1, int(image.height * scale));
    auto pixels = std::shared_ptr<unsigned char>(new unsigned char[size_t(w) * h * 4], std::default_delete<unsigned char[]>());
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
        std::memcpy(pixels.get() + (size_t(y) * w + x) * 4,
            image.pixels.get() + (size_t(int64_t(y) * image.height / h) * image.width + int64_t(x) * image.width / w) * 4, 4);
    return {std::move(pixels), w, h};
}
} // namespace shotlet
