#include "HistoryStore.h"
#include "platform/HistoryDirectoryLock.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <chrono>
#include <cstring>
#include <limits>

namespace shotlet {
namespace fs = std::filesystem;
namespace {
void RequireLock(const HistoryDirectoryLock& lock) { if (!lock) throw std::runtime_error(lock.Error()); }
std::string ReadText(const fs::path& path, uint64_t limit) {
    if (!fs::is_regular_file(fs::symlink_status(path)) || fs::file_size(path) > limit) throw std::runtime_error("历史文本资产缺失或超限");
    std::ifstream input(path, std::ios::binary);
    std::string text(size_t(fs::file_size(path)), '\0');
    input.read(text.data(), std::streamsize(text.size()));
    if (!input) throw std::runtime_error("读取历史资产失败");
    return text;
}
void WriteText(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), std::streamsize(text.size())); output.flush(); output.close();
    if (!output) throw std::runtime_error("写入历史资产失败");
}
bool ValidId(const std::string& id) {
    return !id.empty() && id.size() <= 80 && id.find_first_not_of("0123456789abcdef-") == std::string::npos;
}
uint64_t DirectoryBytes(const fs::path& path) {
    uint64_t bytes = 0;
    for (const auto& file : fs::directory_iterator(path)) {
        if (!fs::is_regular_file(file.symlink_status())) throw std::runtime_error("历史目录存在不支持的资产类型");
        bytes += file.file_size();
    }
    return bytes;
}
}
HistoryStore::HistoryStore(fs::path root, size_t maxRecords, uint64_t maxBytes)
    : m_root(std::move(root)), m_maxRecords(maxRecords), m_maxBytes(maxBytes) {
    if (maxRecords == 0 || maxBytes == 0) throw std::runtime_error("历史容量限制无效");
    fs::create_directories(m_root / "history");
    if (!fs::is_directory(fs::symlink_status(m_root / "history"))) throw std::runtime_error("历史路径不能是符号链接");
#if !defined(_WIN32)
    fs::permissions(m_root / "history", fs::perms::owner_all);
#endif
    m_changes.Start(m_root / "history");
    HistoryDirectoryLock lock(m_root / "history.lock"); RequireLock(lock);
    if (!fs::exists(m_root / "history.conf")) WriteConfig({true, NewHistoryId()});
    ReadConfig();
}
HistoryConfig HistoryStore::ReadConfig() { return DecodeHistoryConfig(ReadText(m_root / "history.conf", 4096)); }
void HistoryStore::WriteConfig(const HistoryConfig& config) {
    const auto temporary = m_root / (".history-conf-" + NewHistoryId());
    try {
        WriteText(temporary, EncodeHistoryConfig(config));
        std::string error;
        if (!ReplaceHistoryFile(temporary, m_root / "history.conf", error)) throw std::runtime_error(error);
        m_changes.Notify();
    } catch (...) { std::error_code ec; fs::remove(temporary, ec); throw; }
}
HistoryConfig HistoryStore::GetConfig() { HistoryDirectoryLock lock(m_root / "history.lock"); RequireLock(lock); return ReadConfig(); }
bool HistoryStore::SetEnabled(bool enabled, std::string& error) {
    try {
        HistoryDirectoryLock lock(m_root / "history.lock"); RequireLock(lock);
        auto config = ReadConfig(); config.enabled = enabled; WriteConfig(config); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
std::optional<std::string> HistoryStore::RegisterCandidate(std::string& error) {
    try { const auto config = GetConfig(); return config.enabled ? std::optional(config.generation) : std::nullopt; }
    catch (const std::exception& e) { error = e.what(); return {}; }
}
bool HistoryStore::IsCurrent(const std::string& generation) {
    try { return GetConfig().generation == generation; } catch (...) { return false; }
}
fs::path HistoryStore::RecordPath(const std::string& id) const {
    if (!ValidId(id)) throw std::runtime_error("历史记录 ID 无效");
    const auto path = m_root / "history" / id;
    if (!fs::is_directory(fs::symlink_status(path))) throw std::runtime_error("历史记录不存在");
    return path;
}
std::vector<HistoryMetadata> HistoryStore::ListLocked(bool includeDeleted) {
    std::vector<HistoryMetadata> list;
    for (const auto& entry : fs::directory_iterator(m_root / "history")) {
        const auto id = entry.path().filename().string();
        if (!ValidId(id) || !fs::is_directory(entry.symlink_status())) continue;
        try {
            auto metadata = DecodeHistoryMetadata(ReadText(entry.path() / "meta.json", HistoryMetadata::MaxJsonBytes));
            if (metadata.id != id || !fs::is_regular_file(fs::symlink_status(entry.path() / "result.png"))) continue;
            if (!includeDeleted && metadata.details.deletedAtUnixMs) continue;
            metadata.bytes = DirectoryBytes(entry.path()); list.push_back(std::move(metadata));
        } catch (...) { /* 单条损坏不能影响其他历史。 */ }
    }
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
        return a.createdAtUnixMs != b.createdAtUnixMs ? a.createdAtUnixMs > b.createdAtUnixMs : a.id > b.id;
    });
    return list;
}
std::vector<HistoryMetadata> HistoryStore::List(bool includeDeleted) {
    HistoryDirectoryLock lock(m_root / "history.lock"); RequireLock(lock);
    auto records=ListLocked(true);
    const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::erase_if(records,[&](const auto& r){
        if(r.details.deletedAtUnixMs && now-r.details.deletedAtUnixMs>TrashRetentionMs){RemoveLocked(RecordPath(r.id));return true;}
        return false;
    });
    if(!includeDeleted)std::erase_if(records,[](const auto& r){return r.details.deletedAtUnixMs!=0;});
    return records;
}
std::optional<HistoryMetadata> HistoryStore::Get(const std::string& id, std::string& error) {
    try {
        HistoryDirectoryLock lock(m_root / "history.lock"); RequireLock(lock);
        auto metadata = DecodeHistoryMetadata(ReadText(RecordPath(id) / "meta.json", HistoryMetadata::MaxJsonBytes));
        if (metadata.id != id) throw std::runtime_error("历史记录 ID 不一致");
        return metadata;
    } catch (const std::exception& e) { error = e.what(); return {}; }
}
bool HistoryStore::UpdateDetails(const std::string& id, const std::function<void(HistoryDetails&)>& update, std::string& error) {
    fs::path temporary;
    try {
        HistoryDirectoryLock lock(m_root / "history.lock"); RequireLock(lock);
        const auto path = RecordPath(id);
        auto metadata = DecodeHistoryMetadata(ReadText(path / "meta.json", HistoryMetadata::MaxJsonBytes));
        if (metadata.id != id) throw std::runtime_error("历史记录 ID 不一致");
        update(metadata.details);
        auto text = EncodeHistoryMetadata(metadata);
        metadata = DecodeHistoryMetadata(text); // 属性修改必须满足和磁盘读取相同的校验，并统一去重标签。
        text = EncodeHistoryMetadata(metadata);
        const uint64_t assets = DirectoryBytes(path) - fs::file_size(path / "meta.json");
        for (int pass = 0; pass < 4; ++pass) {
            metadata.bytes = assets + text.size(); text = EncodeHistoryMetadata(metadata);
        }
        if (assets + text.size() > m_maxBytes) throw std::runtime_error("修改后的记录超过历史容量上限");
        temporary = path / (".meta-" + NewHistoryId()); WriteText(temporary, text);
        if (!ReplaceHistoryFile(temporary, path / "meta.json", error)) throw std::runtime_error(error);
        temporary.clear(); m_changes.Notify(); EnforceQuotaLocked(); return true;
    } catch (const std::exception& e) {
        if (!temporary.empty()) { std::error_code ec; fs::remove(temporary, ec); }
        error = e.what(); return false;
    }
}
bool HistoryStore::Trash(const std::string& id, std::string& error) {
    return UpdateDetails(id, [](HistoryDetails& d) {
        if (!d.deletedAtUnixMs) d.deletedAtUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }, error);
}
bool HistoryStore::Restore(const std::string& id, std::string& error) {
    return UpdateDetails(id, [](HistoryDetails& d) { d.deletedAtUnixMs = 0; }, error);
}
bool HistoryStore::EmptyTrash(std::string& error) {
    try {
        HistoryDirectoryLock lock(m_root / "history.lock"); RequireLock(lock);
        for (const auto& metadata : ListLocked(true)) if (metadata.details.deletedAtUnixMs) RemoveLocked(RecordPath(metadata.id));
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
void HistoryStore::RemoveLocked(const fs::path& path) {
    const auto hidden = path.parent_path() / (".deleted-" + NewHistoryId());
    fs::rename(path, hidden);
    m_changes.Notify();
    std::error_code ec; fs::remove_all(hidden, ec);
}
void HistoryStore::EnforceQuotaLocked() {
    auto records = ListLocked(true);
    // 最近删除仍计入总配额，优先清理回收站，再按原有规则淘汰最旧截图。
    std::stable_sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
        return bool(a.details.deletedAtUnixMs) < bool(b.details.deletedAtUnixMs);
    });
    uint64_t total = 0;
    for (const auto& record : records) total += record.bytes;
    while (records.size() > m_maxRecords || total > m_maxBytes) {
        const auto oldest = records.back();
        RemoveLocked(RecordPath(oldest.id)); total -= oldest.bytes; records.pop_back();
    }
}
bool HistoryStore::Write(const std::string& generation, const CaptureDocument& document, const HistoryImage& result,
                         const std::string& action, std::string& error, const HistoryDetails& details, std::string* writtenId) {
    fs::path temporary;
    std::unique_ptr<HistoryDirectoryLock> lease;
    try {
        if (!ValidateCaptureDocument(document, error)) return false;
        if (!result) { error = "最终图片无效"; return false; }
        const auto id = NewHistoryId();
        temporary = m_root / "history" / (".tmp-" + id);
        {
            HistoryDirectoryLock lock(m_root / "history.lock"); RequireLock(lock);
            if (ReadConfig().generation != generation) { error = "候选代次已失效"; return false; }
            fs::create_directory(temporary);
#if !defined(_WIN32)
            fs::permissions(temporary, fs::perms::owner_all);
#endif
            lease = std::make_unique<HistoryDirectoryLock>(temporary / ".lease"); RequireLock(*lease);
        }
        // 编码和缩略图生成期间只持有临时目录租约，其他进程仍可登记候选或清空历史。
        if (!WriteHistoryPng(temporary / "original.png", document.originalRgba.data(), document.width, document.height, error) ||
            !WriteHistoryPng(temporary / "result.png", result.pixels.get(), result.width, result.height, error)) throw std::runtime_error(error);
        auto thumbnail = MakeHistoryThumbnail(result);
        if (!WriteHistoryPng(temporary / "thumbnail.png", thumbnail.pixels.get(), thumbnail.width, thumbnail.height, error)) throw std::runtime_error(error);
        if (document.mosaicCache) {
            const auto& c = *document.mosaicCache;
            if (!WriteHistoryPng(temporary / "mosaic.png", c.rgba.data(), c.width, c.height, error)) throw std::runtime_error(error);
        }
        WriteText(temporary / "document.json", EncodeDocument(document));
        HistoryMetadata metadata{id, action, document.sourceRecordId, document.createdAtUnixMs, result.width, result.height};
        metadata.details = details;
        if (metadata.details.captureType == "unknown") metadata.details.captureType = document.source == "scroll" ? "scroll" : "region";
        DecodeHistoryMetadata(EncodeHistoryMetadata(metadata));
        WriteText(temporary / "meta.json", EncodeHistoryMetadata(metadata));
        for (int pass = 0; pass < 4; ++pass) {
            metadata.bytes = DirectoryBytes(temporary);
            WriteText(temporary / "meta.json", EncodeHistoryMetadata(metadata));
            if (DirectoryBytes(temporary) == metadata.bytes) break;
        }
        const auto bytes = DirectoryBytes(temporary);
        if (bytes > m_maxBytes) throw std::runtime_error("本条截图历史超过磁盘保留上限，旧历史已保留");
        {
            HistoryDirectoryLock lock(m_root / "history.lock"); RequireLock(lock);
            if (ReadConfig().generation != generation) throw std::runtime_error("候选代次已失效");
            lease.reset(); fs::remove(temporary / ".lease");
            fs::rename(temporary, m_root / "history" / id);
            m_changes.Notify();
            temporary.clear();
            EnforceQuotaLocked();
            if (writtenId) *writtenId = id;
        }
        return true;
    } catch (const std::exception& e) {
        error = e.what(); lease.reset();
        if (!temporary.empty()) { std::error_code ec; fs::remove_all(temporary, ec); }
        return false;
    }
}
HistoryImage HistoryStore::LoadResult(const std::string& id, bool thumbnail, std::string& error, bool requireActive) {
    try {
        HistoryDirectoryLock lock(m_root / "history.lock"); RequireLock(lock);
        const auto path = RecordPath(id);
        if (thumbnail && !requireActive) return ReadHistoryPng(path / "thumbnail.png", error, 0, 0, 192u * 128u * 4u);
        const auto metadata = DecodeHistoryMetadata(ReadText(path / "meta.json", HistoryMetadata::MaxJsonBytes));
        // 快速复制必须在持有目录锁时检查删除状态；完整图库仍可预览回收站中的图片。
        if (requireActive && metadata.details.deletedAtUnixMs) { error = "截图已被删除"; return {}; }
        if (thumbnail) return ReadHistoryPng(path / "thumbnail.png", error, 0, 0, 192u * 128u * 4u);
        return ReadHistoryPng(path / "result.png", error, metadata.width, metadata.height);
    } catch (const std::exception& e) { error = e.what(); return {}; }
}
std::shared_ptr<CaptureDocument> HistoryStore::LoadDocument(const std::string& id, std::string& error) {
    try {
        HistoryDirectoryLock lock(m_root / "history.lock"); RequireLock(lock);
        const auto path = RecordPath(id);
        auto d = std::make_shared<CaptureDocument>(DecodeDocument(ReadText(path / "document.json", 64u * 1024 * 1024)));
        auto original = ReadHistoryPng(path / "original.png", error);
        if (!original || original.width != d->width || original.height != d->height) throw std::runtime_error("原图缺失或尺寸不一致");
        d->originalRgba.assign(original.pixels.get(), original.pixels.get() + original.ByteSize());
        if (d->mosaicCache) {
            const auto cache = ReadHistoryPng(path / "mosaic.png", error);
            if (!cache || cache.width != d->mosaicCache->width || cache.height != d->mosaicCache->height)
                throw std::runtime_error("马赛克缓存缺失或尺寸不一致");
            d->mosaicCache->rgba.assign(cache.pixels.get(), cache.pixels.get() + cache.ByteSize());
        }
        if (!ValidateCaptureDocument(*d, error)) return {};
        d->sourceRecordId = id; d->source = "history";
        return d;
    } catch (const std::exception& e) { error = e.what(); return {}; }
}
bool HistoryStore::Delete(const std::string& id, std::string& error) {
    try { HistoryDirectoryLock lock(m_root / "history.lock"); RequireLock(lock); RemoveLocked(RecordPath(id)); return true; }
    catch (const std::exception& e) { error = e.what(); return false; }
}
bool HistoryStore::Clear(std::string& error) {
    try {
        HistoryDirectoryLock lock(m_root / "history.lock"); RequireLock(lock);
        auto config = ReadConfig(); config.generation = NewHistoryId();
        WriteConfig(config); // 必须先持久化代次；失败时不先删除记录。
        for (const auto& entry : fs::directory_iterator(m_root / "history"))
            if (ValidId(entry.path().filename().string()) && fs::is_directory(entry.symlink_status())) RemoveLocked(entry.path());
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
void HistoryStore::Recover() {
    HistoryDirectoryLock lock(m_root / "history.lock"); RequireLock(lock);
    std::vector<fs::path> paths;
    for (const auto& entry : fs::directory_iterator(m_root / "history")) paths.push_back(entry.path());
    for (const auto& path : paths) {
        const auto id = path.filename().string();
        if (!fs::is_directory(fs::symlink_status(path))) continue;
        if (id.starts_with(".tmp-") || id.starts_with(".deleted-")) {
            bool inactive = false;
            { HistoryDirectoryLock lease(path / ".lease", false); inactive = bool(lease); }
            if (inactive) { std::error_code ec; fs::remove_all(path, ec); }
        } else if (ValidId(id)) {
            try {
                for(const auto& asset:fs::directory_iterator(path))
                    if(asset.path().filename().string().starts_with(".meta-") && fs::is_regular_file(asset.symlink_status())) fs::remove(asset.path());
                auto metadata = DecodeHistoryMetadata(ReadText(path / "meta.json", HistoryMetadata::MaxJsonBytes));
                if (metadata.id != id || !fs::is_regular_file(fs::symlink_status(path / "result.png")))
                    throw std::runtime_error("不完整的历史记录");
                DirectoryBytes(path);
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                if (metadata.details.deletedAtUnixMs && now - metadata.details.deletedAtUnixMs > TrashRetentionMs)
                    RemoveLocked(path);
            } catch (...) { RemoveLocked(path); }
        }
    }
    for (const auto& entry : fs::directory_iterator(m_root)) {
        if (entry.path().filename().string().starts_with(".history-conf-") && fs::is_regular_file(entry.symlink_status())) {
            std::error_code ec; fs::remove(entry.path(), ec);
        }
    }
    // 覆盖上次进程在正式重命名后、容量清理前被强制终止的情况。
    EnforceQuotaLocked();
}
} // namespace shotlet
