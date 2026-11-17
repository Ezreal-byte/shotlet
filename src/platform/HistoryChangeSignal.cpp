#include "HistoryChangeSignal.h"
#include <stdexcept>
#if defined(_WIN32)
#include "platform/PlatformWin32.h"
#elif defined(__APPLE__)
#include <notify.h>
#elif defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace shotlet {
void HistoryChangeSignal::Start(const std::filesystem::path& directory) {
    m_directory = std::filesystem::canonical(directory);
#if defined(_WIN32)
    HANDLE handle = FindFirstChangeNotificationW(m_directory.c_str(), TRUE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("无法监听截图历史变更");
    m_handle = reinterpret_cast<intptr_t>(handle);
#elif defined(__APPLE__)
    // 固定散列算法保证独立进程使用同一通知名，不依赖 std::hash 的实现。
    uint64_t hash = 14695981039346656037ull;
    for (auto byte : m_directory.u8string()) { hash ^= uint8_t(byte); hash *= 1099511628211ull; }
    m_name = "com.shotlet.history." + std::to_string(hash);
    int token = -1;
    if (notify_register_check(m_name.c_str(), &token) != NOTIFY_STATUS_OK)
        throw std::runtime_error("无法注册截图历史变更通知");
    m_handle = token;
    int changed = 0;
    notify_check(token, &changed); // 清除首次检查默认返回的变更标记。
#elif defined(__linux__)
    m_handle = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_handle < 0 || inotify_add_watch(int(m_handle), m_directory.c_str(), IN_ATTRIB) < 0) {
        if (m_handle >= 0) close(int(m_handle));
        m_handle = -1;
        throw std::runtime_error("无法监听截图历史变更");
    }
#endif
}
HistoryChangeSignal::~HistoryChangeSignal() {
    if (m_handle == -1) return;
#if defined(_WIN32)
    FindCloseChangeNotification(reinterpret_cast<HANDLE>(m_handle));
#elif defined(__APPLE__)
    notify_cancel(int(m_handle));
#elif defined(__linux__)
    close(int(m_handle));
#endif
}
void HistoryChangeSignal::Notify() {
    ++m_revision;
#if defined(__APPLE__)
    notify_post(m_name.c_str());
#elif defined(__linux__)
    // 只在正式提交时修改目录属性，其他进程通过 inotify 收到通知。
    std::error_code error;
    std::filesystem::last_write_time(m_directory, std::filesystem::file_time_type::clock::now(), error);
#endif
}
uint64_t HistoryChangeSignal::Revision() {
    bool changed = false;
#if defined(_WIN32)
    if (m_handle != -1 && WaitForSingleObject(reinterpret_cast<HANDLE>(m_handle), 0) == WAIT_OBJECT_0) {
        FindNextChangeNotification(reinterpret_cast<HANDLE>(m_handle));
        changed = true;
    }
#elif defined(__APPLE__)
    int posted = 0;
    changed = m_handle != -1 && notify_check(int(m_handle), &posted) == NOTIFY_STATUS_OK && posted;
#elif defined(__linux__)
    alignas(inotify_event) char events[4096];
    while (m_handle != -1 && read(int(m_handle), events, sizeof(events)) > 0) changed = true;
#endif
    if (changed) ++m_revision;
    return m_revision.load();
}
} // namespace shotlet
