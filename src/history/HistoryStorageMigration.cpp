#include "HistoryStore.h"
#include "platform/HistoryDirectoryLock.h"
#include <fstream>
#include <algorithm>
#include <stdexcept>

namespace shotlet {
namespace fs = std::filesystem;
namespace {
bool Contains(const fs::path& parent, const fs::path& child) {
    auto a = parent.begin(), b = child.begin();
    for (; a != parent.end(); ++a, ++b) if (b == child.end() || *a != *b) return false;
    return true;
}
void CopyAsset(const fs::path& from, const fs::path& to) {
    if (!fs::is_regular_file(fs::symlink_status(from))) throw std::runtime_error("历史资产不是普通文件");
    fs::copy_file(from, to);
    // 逐块比对，确保跨磁盘复制的内容完整后才切换配置。
    std::ifstream source(from, std::ios::binary), destination(to, std::ios::binary);
    char a[65536], b[65536];
    do {
        source.read(a, sizeof(a)); destination.read(b, sizeof(b));
        if (source.bad() || destination.bad() || source.gcount() != destination.gcount() ||
            !std::equal(a, a + source.gcount(), b)) throw std::runtime_error("历史资产复制校验失败");
    } while (source.gcount() != 0);
}
}
bool HistoryStore::MigrateTo(const fs::path& destination, const std::function<bool(std::string&)>& commit, std::string& error) {
    fs::path target, staging;
    bool published = false, sourceDisabled = false;
    HistoryConfig original;
    std::unique_ptr<HistoryDirectoryLock> sourceLock;
    try {
        const auto source = fs::canonical(m_root);
        target = fs::weakly_canonical(destination);
        if (source == target) return commit(error);
        if (Contains(source, target) || Contains(target, source)) throw std::runtime_error("新目录不能与当前历史目录互相包含");
        fs::create_directories(target);
        sourceLock = std::make_unique<HistoryDirectoryLock>(m_root / "history.lock", false);
        if (!*sourceLock) throw std::runtime_error(sourceLock->Error());
        auto targetLock = std::make_unique<HistoryDirectoryLock>(target / "history.lock", false);
        if (!*targetLock) throw std::runtime_error(targetLock->Error());
        if (fs::exists(target / "history.conf") || (fs::exists(target / "history") &&
            (!fs::is_directory(fs::symlink_status(target / "history")) || !fs::is_empty(target / "history"))))
            throw std::runtime_error("目标目录已有截图历史，请选择其他目录");
        original = ReadConfig();
        staging = target / (".shotlet-migration-" + NewHistoryId()); fs::create_directory(staging);
#if !defined(_WIN32)
        fs::permissions(staging, fs::perms::owner_all);
#endif
        const auto records = ListLocked(true);
        for (const auto& record : records) {
            const auto sourceRecord = m_root / "history" / record.id;
            const auto targetRecord = staging / record.id; fs::create_directory(targetRecord);
            for (const auto& asset : fs::directory_iterator(sourceRecord)) CopyAsset(asset.path(), targetRecord / asset.path().filename());
        }
        if (fs::exists(target / "history")) fs::remove(target / "history");
        fs::rename(staging, target / "history"); published = true;
        const auto config = target / (".history-conf-" + NewHistoryId());
        {
            std::ofstream output(config, std::ios::binary);
            output << EncodeHistoryConfig({original.enabled, NewHistoryId()}); output.close();
            if (!output) throw std::runtime_error("无法写入目标历史配置");
        }
        if (!ReplaceHistoryFile(config, target / "history.conf", error)) { fs::remove(config); throw std::runtime_error(error); }
        // 原目录换代并停用，其他已启动进程的旧候选也不能在迁移后重新写入。
        WriteConfig({false, NewHistoryId()}); sourceDisabled = true;
        targetLock.reset();
        if (!commit(error)) throw std::runtime_error(error);
        // 偏好提交成功后才清理源记录，清理失败留下副本不影响新目录使用。
        for (const auto& record : records) { std::error_code ignored; fs::remove_all(m_root / "history" / record.id, ignored); }
        m_changes.Notify();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        if (sourceDisabled) {
            try { WriteConfig(original); } catch (const std::exception& restore) { error += std::string("; ") + restore.what(); }
        }
        std::error_code ignored;
        if (published) { fs::remove_all(target / "history", ignored); fs::remove(target / "history.conf", ignored); }
        if (!staging.empty()) fs::remove_all(staging, ignored);
        return false;
    }
}
} // namespace shotlet
