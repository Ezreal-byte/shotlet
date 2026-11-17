#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace shotlet {
// 历史存储提交后的跨进程变更通知；检查版本不扫描目录或读取 JSON。
class HistoryChangeSignal {
public:
    HistoryChangeSignal() = default;
    ~HistoryChangeSignal();
    HistoryChangeSignal(const HistoryChangeSignal&) = delete;
    HistoryChangeSignal& operator=(const HistoryChangeSignal&) = delete;
    void Start(const std::filesystem::path& directory);
    void Notify();
    uint64_t Revision();
private:
    std::filesystem::path m_directory;
    std::string m_name;
    intptr_t m_handle = -1;
#if defined(_WIN32)
    void* m_sharedRevision = nullptr;
#endif
    std::atomic<uint64_t> m_revision{0};
};
} // namespace shotlet
