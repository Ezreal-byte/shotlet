#include "HistoryDirectoryLock.h"
#if defined(_WIN32)
#include "platform/PlatformWin32.h"
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

namespace shotlet {
HistoryDirectoryLock::HistoryDirectoryLock(const std::filesystem::path& file, bool wait) {
#if defined(_WIN32)
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { m_error = "无法打开历史锁文件"; return; }
    m_handle = reinterpret_cast<intptr_t>(handle);
    OVERLAPPED overlap{};
    m_locked = LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY), 0, 1, 0, &overlap);
    if (!m_locked) m_error = "无法取得历史目录锁";
#else
    m_handle = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (m_handle < 0) { m_error = std::strerror(errno); return; }
    int status;
    do { status = flock(int(m_handle), LOCK_EX | (wait ? 0 : LOCK_NB)); } while (status < 0 && errno == EINTR);
    m_locked = status == 0;
    if (!m_locked) m_error = std::strerror(errno);
#endif
}
HistoryDirectoryLock::~HistoryDirectoryLock() {
    if (m_handle == -1) return;
#if defined(_WIN32)
    auto handle = reinterpret_cast<HANDLE>(m_handle);
    if (m_locked) { OVERLAPPED overlap{}; UnlockFileEx(handle, 0, 1, 0, &overlap); }
    CloseHandle(handle);
#else
    if (m_locked) flock(int(m_handle), LOCK_UN);
    close(int(m_handle));
#endif
}
bool ReplaceHistoryFile(const std::filesystem::path& from, const std::filesystem::path& to, std::string& error) {
#if defined(_WIN32)
    HANDLE file = CreateFileW(from.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = "无法刷新历史配置"; return false; }
    const bool flushed = FlushFileBuffers(file); CloseHandle(file);
    if (!flushed) { error = "无法持久化历史配置"; return false; }
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    error = "历史配置原子替换失败"; return false;
#else
    const int file = open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (file < 0) { error = std::strerror(errno); return false; }
    const bool flushed = fsync(file) == 0; close(file);
    if (!flushed) { error = "无法持久化历史配置"; return false; }
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (!ec) {
        const int directory = open(to.parent_path().c_str(), O_RDONLY | O_CLOEXEC);
        if (directory >= 0) { fsync(directory); close(directory); }
        return true;
    }
    error = ec.message(); return false;
#endif
}
} // namespace shotlet
