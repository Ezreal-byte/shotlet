#include "HistoryLibrary.h"
#include "HistoryStore.h"
#include "platform/HistoryDirectoryLock.h"
#include "utils/StringUtils.h"
#include <SDL3/SDL_stdinc.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace shotlet {
namespace {
std::tm LocalTime(int64_t milliseconds) {
    std::time_t time = std::time_t(milliseconds / 1000); std::tm value{};
#if defined(_WIN32)
    localtime_s(&value, &time);
#else
    localtime_r(&time, &value);
#endif
    return value;
}
std::string ExportName(const HistoryMetadata& record) {
    auto name = HistoryName(record);
    for (char& c : name) if (static_cast<unsigned char>(c) < 32 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) c = '_';
    if (name.size() > 120) {
        size_t end = 120;
        while (end && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80) --end;
        name.resize(end);
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    while (!name.empty() && (name.front() == ' ' || name.front() == '.')) name.erase(name.begin());
    return (name.empty() ? "Screenshot" : name) + "-" + record.id.substr(record.id.size() > 8 ? record.id.size() - 8 : 0);
}
}
int64_t HistoryNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
int64_t HistoryDayStart(int64_t milliseconds, int offset) {
    auto value = LocalTime(milliseconds); value.tm_hour = value.tm_min = value.tm_sec = 0;
    value.tm_mday += offset; value.tm_isdst = -1;
    return int64_t(std::mktime(&value)) * 1000;
}
std::string HistoryTime(int64_t milliseconds, const char* format) {
    auto value = LocalTime(milliseconds); char buffer[96]{}; std::strftime(buffer, sizeof(buffer), format, &value); return buffer;
}
std::string HistoryName(const HistoryMetadata& record, Language language) {
    return record.details.title.empty() ? std::string(Tr(language,TextId::HistoryScreenshot))+" "+HistoryTime(record.createdAtUnixMs,"%Y-%m-%d %H.%M.%S") : record.details.title;
}
bool HistoryMatches(const HistoryMetadata& r, const HistoryQuery& q, int64_t now) {
    const bool trash = r.details.deletedAtUnixMs != 0;
    if (trash != (q.scope == HistoryScope::Trash)) return false;
    switch (q.scope) {
        case HistoryScope::Favorites: if (!r.details.favorite) return false; break;
        case HistoryScope::Today: if (HistoryDayStart(r.createdAtUnixMs) != HistoryDayStart(now)) return false; break;
        case HistoryScope::Yesterday: if (HistoryDayStart(r.createdAtUnixMs) != HistoryDayStart(now, -1)) return false; break;
        case HistoryScope::Week: if (r.createdAtUnixMs < HistoryDayStart(now, -6) || r.createdAtUnixMs >= HistoryDayStart(now, 1)) return false; break;
        case HistoryScope::Type: if (r.details.captureType != q.value) return false; break;
        case HistoryScope::Tag: if (std::find(r.details.tags.begin(), r.details.tags.end(), q.value) == r.details.tags.end()) return false; break;
        default: break;
    }
    const auto search = StringUtils::Trim(q.search);
    if (search.empty()) return true;
    std::string haystack = HistoryName(r) + " " + r.details.sourceApplication + " " + r.details.sourceWindow + " " + r.details.ocrText;
    if(r.details.title.empty())haystack+=" "+HistoryName(r,Language::ChineseSimplified);
    for (const auto& tag : r.details.tags) haystack += " " + tag;
    std::istringstream terms(search); std::string term;
    // 保留多关键词同时命中的规则，使用 SDL 的 UTF-8 大小写折叠，避免破坏中文或受系统 locale 影响。
    while (terms >> term) if (!SDL_strcasestr(haystack.c_str(), term.c_str())) return false;
    return true;
}
std::vector<size_t> QueryHistory(const std::vector<HistoryMetadata>& records, const HistoryQuery& query, int64_t now) {
    std::vector<size_t> result;
    for (size_t i = 0; i < records.size(); ++i) if (HistoryMatches(records[i], query, now)) result.push_back(i);
    std::stable_sort(result.begin(), result.end(), [&](size_t a, size_t b) {
        const auto& x = records[a]; const auto& y = records[b];
        if (query.sort == HistorySort::Name && HistoryName(x) != HistoryName(y)) return HistoryName(x) < HistoryName(y);
        if (query.sort == HistorySort::Largest && x.bytes != y.bytes) return x.bytes > y.bytes;
        const auto xTime=query.scope==HistoryScope::Trash?x.details.deletedAtUnixMs:x.createdAtUnixMs;
        const auto yTime=query.scope==HistoryScope::Trash?y.details.deletedAtUnixMs:y.createdAtUnixMs;
        if (xTime != yTime) return query.sort == HistorySort::Oldest ? xTime < yTime : xTime > yTime;
        return x.id < y.id;
    });
    return result;
}
std::filesystem::path HistoryStore::ResultPath(const std::string& id, std::string& error) {
    try {
        HistoryDirectoryLock lock(m_root / "history.lock");
        if (!lock) throw std::runtime_error(lock.Error());
        auto path = RecordPath(id) / "result.png";
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path))) throw std::runtime_error("图片文件不存在");
        return path;
    } catch (const std::exception& e) { error = e.what(); return {}; }
}
bool HistoryStore::ExportResults(const std::vector<std::string>& ids, const std::filesystem::path& folder, std::string& error) {
    namespace fs = std::filesystem;
    fs::path temporary;
    try {
        if (ids.empty() || !fs::is_directory(folder)) throw std::runtime_error("请选择导出目录和图片");
        HistoryDirectoryLock lock(m_root / "history.lock");
        if (!lock) throw std::runtime_error(lock.Error());
        for (const auto& id : ids) {
            const auto recordPath = RecordPath(id);
            if (!fs::is_regular_file(fs::symlink_status(recordPath / "meta.json")) || fs::file_size(recordPath / "meta.json") > HistoryMetadata::MaxJsonBytes ||
                !fs::is_regular_file(fs::symlink_status(recordPath / "result.png"))) throw std::runtime_error("历史资产缺失或无效");
            std::ifstream stream(recordPath / "meta.json");
            std::string json((std::istreambuf_iterator<char>(stream)), {});
            auto metadata = DecodeHistoryMetadata(json);
            if (metadata.id != id || metadata.details.deletedAtUnixMs) throw std::runtime_error("待导出记录已被删除");
            const auto utf8 = [](const std::string& name) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(name.data()),name.size())); };
            auto name = ExportName(metadata); auto target = folder / utf8(name + ".png");
            for (int suffix = 1; fs::exists(target); ++suffix) target = folder / utf8(name + "-" + std::to_string(suffix) + ".png");
            temporary = folder / (".shotlet-export-" + NewHistoryId());
            fs::copy_file(recordPath / "result.png", temporary);
            fs::copy_file(temporary, target); fs::remove(temporary); temporary.clear();
        }
        return true;
    } catch (const std::exception& e) {
        if (!temporary.empty()) { std::error_code ec; fs::remove(temporary, ec); }
        error = e.what(); return false;
    }
}
HistoryImage HistoryStore::Stitch(const std::vector<std::string>& ids, bool horizontal, std::string& error) {
    try {
        if (ids.size() < 2 || ids.size() > 100) throw std::runtime_error("请选择 2 到 100 张截图");
        int64_t width = 0, height = 0;
        std::vector<HistoryMetadata> records;
        for (const auto& id : ids) {
            auto record = Get(id, error);
            if (!record || record->details.deletedAtUnixMs) throw std::runtime_error("拼接记录已被删除");
            width = horizontal ? width + record->width : std::max<int64_t>(width, record->width);
            height = horizontal ? std::max<int64_t>(height, record->height) : height + record->height;
            if (width > 65535 || height > 65535 || !DocumentPixelBytes(int(width), int(height))) throw std::runtime_error("拼接图片尺寸超过上限，请减少图片数量");
            records.push_back(*record);
        }
        auto pixels = std::shared_ptr<unsigned char>(new unsigned char[size_t(width) * height * 4], std::default_delete<unsigned char[]>());
        std::memset(pixels.get(), 255, size_t(width) * height * 4);
        int offset = 0;
        for (const auto& record : records) {
            auto image = LoadResult(record.id, false, error);
            if (!image || image.width != record.width || image.height != record.height) throw std::runtime_error("拼接图片读取失败");
            const int x = horizontal ? offset : (int(width) - image.width) / 2;
            const int y = horizontal ? (int(height) - image.height) / 2 : offset;
            for (int row = 0; row < image.height; ++row)
                std::memcpy(pixels.get() + (size_t(row + y) * width + x) * 4, image.pixels.get() + size_t(row) * image.width * 4, size_t(image.width) * 4);
            offset += horizontal ? image.width : image.height;
        }
        return {std::move(pixels), int(width), int(height)};
    } catch (const std::exception& e) { error = e.what(); return {}; }
}
} // namespace shotlet
