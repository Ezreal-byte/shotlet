#pragma once
#include "DocumentCodec.h"
#include "platform/HistoryChangeSignal.h"
#include <optional>
#include <functional>

namespace shotlet {
class HistoryStore {
public:
    static constexpr size_t DefaultMaxRecords = 100;
    static constexpr uint64_t DefaultMaxBytes = 512ull * 1024 * 1024;
    static constexpr int64_t TrashRetentionMs = 30ll * 24 * 60 * 60 * 1000;
    explicit HistoryStore(std::filesystem::path root, size_t maxRecords = DefaultMaxRecords, uint64_t maxBytes = DefaultMaxBytes);
    const std::filesystem::path& Root() const { return m_root; }
    uint64_t Revision() { return m_changes.Revision(); }
    HistoryConfig GetConfig();
    bool SetEnabled(bool enabled, std::string& error);
    std::optional<std::string> RegisterCandidate(std::string& error);
    bool IsCurrent(const std::string& generation);
    bool Write(const std::string& generation, const CaptureDocument& document, const HistoryImage& result,
               const std::string& action, std::string& error, const HistoryDetails& details = {}, std::string* writtenId = nullptr);
    std::vector<HistoryMetadata> List(bool includeDeleted = false);
    std::optional<HistoryMetadata> Get(const std::string& id, std::string& error);
    bool UpdateDetails(const std::string& id, const std::function<void(HistoryDetails&)>& update, std::string& error);
    bool Trash(const std::string& id, std::string& error);
    bool Restore(const std::string& id, std::string& error);
    bool EmptyTrash(std::string& error);
    std::filesystem::path ResultPath(const std::string& id, std::string& error);
    bool ExportResults(const std::vector<std::string>& ids, const std::filesystem::path& folder, std::string& error);
    HistoryImage Stitch(const std::vector<std::string>& ids, bool horizontal, std::string& error);
    HistoryImage LoadResult(const std::string& id, bool thumbnail, std::string& error, bool requireActive = false);
    std::shared_ptr<CaptureDocument> LoadDocument(const std::string& id, std::string& error);
    bool Delete(const std::string& id, std::string& error);
    bool Clear(std::string& error);
    // 调用前排空本进程写入；复制完成后提交偏好，失败保留原目录及原代次。
    bool MigrateTo(const std::filesystem::path& destination, const std::function<bool(std::string&)>& commit,
                   std::string& error);
    void Recover();
private:
    HistoryConfig ReadConfig();
    void WriteConfig(const HistoryConfig& config);
    std::vector<HistoryMetadata> ListLocked(bool includeDeleted = false);
    void RemoveLocked(const std::filesystem::path& path);
    void EnforceQuotaLocked();
    std::filesystem::path RecordPath(const std::string& id) const;
    std::filesystem::path m_root;
    size_t m_maxRecords;
    uint64_t m_maxBytes;
    HistoryChangeSignal m_changes;
};
} // namespace shotlet
