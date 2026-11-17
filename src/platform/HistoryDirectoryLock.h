#pragma once
#include <filesystem>
#include <cstdint>
#include <string>

namespace shotlet {
// 锁文件固定存在；进程退出后由操作系统释放锁，不用删除锁文件抢锁。
class HistoryDirectoryLock {
public:
    explicit HistoryDirectoryLock(const std::filesystem::path& file, bool wait = true);
    ~HistoryDirectoryLock();
    HistoryDirectoryLock(const HistoryDirectoryLock&) = delete;
    HistoryDirectoryLock& operator=(const HistoryDirectoryLock&) = delete;
    explicit operator bool() const { return m_locked; }
    const std::string& Error() const { return m_error; }
private:
    intptr_t m_handle = -1;
    bool m_locked = false;
    std::string m_error;
};
// 同目录替换，失败不破坏旧配置。路径全程使用平台原生 Unicode 类型。
bool ReplaceHistoryFile(const std::filesystem::path& from, const std::filesystem::path& to, std::string& error);
} // namespace shotlet
