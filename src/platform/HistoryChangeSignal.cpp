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
#if defined(_WIN32) || defined(__APPLE__)
    auto namePath = m_directory;
#if defined(_WIN32)
    // Windows 路径不区分大小写，同一目录必须映射到同一个内核对象。
    auto nativePath = namePath.native();
    CharLowerBuffW(nativePath.data(), static_cast<DWORD>(nativePath.size()));
    namePath = nativePath;
#endif
    // 固定散列算法保证独立进程使用同一通知名，不依赖 std::hash 的实现。
    constexpr uint64_t FnvOffsetBasis = 14695981039346656037ull, FnvPrime = 1099511628211ull;
    uint64_t hash = FnvOffsetBasis;
    for (auto byte : namePath.u8string()) { hash ^= uint8_t(byte); hash *= FnvPrime; }
    m_name = "com.shotlet.history." + std::to_string(hash);
#endif
#if defined(_WIN32)
    // 目录事件会合并、延迟，且清理临时资产也会触发；只在提交时更新共享版本。
    // 页文件映射由系统清零，已存在时不得重置，最后一个观察者退出后由系统释放。
    const std::wstring name = L"Local\\" + std::wstring(m_name.begin(), m_name.end());
    HANDLE handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(LONG64), name.c_str());
    if (!handle) throw std::runtime_error("无法创建截图历史变更通知");
    m_sharedRevision = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG64));
    if (!m_sharedRevision) {
        CloseHandle(handle);
        throw std::runtime_error("无法映射截图历史变更通知");
    }
    m_handle = reinterpret_cast<intptr_t>(handle);
#elif defined(__APPLE__)
    int token = -1;
    if (notify_register_check(m_name.c_str(), &token) != NOTIFY_STATUS_OK)
        throw std::runtime_error("无法注册截图历史变更通知");
    m_handle = token;
    int changed = 0;
    notify_check(token, &changed); // 清除首次检查默认返回的变更标记。
#elif defined(__linux__)
    m_handle = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_handle < 0 || inotify_add_watch(int(m_handle), m_directory.c_str(), IN_ATTRIB | IN_MODIFY) < 0) {
        if (m_handle >= 0) close(int(m_handle));
        m_handle = -1;
        throw std::runtime_error("无法监听截图历史变更");
    }
#endif
}
HistoryChangeSignal::~HistoryChangeSignal() {
    if (m_handle == -1) return;
#if defined(_WIN32)
    UnmapViewOfFile(m_sharedRevision);
    CloseHandle(reinterpret_cast<HANDLE>(m_handle));
#elif defined(__APPLE__)
    notify_cancel(int(m_handle));
#elif defined(__linux__)
    close(int(m_handle));
#endif
}
void HistoryChangeSignal::Notify() {
#if defined(_WIN32)
    if (m_sharedRevision) InterlockedIncrement64(static_cast<volatile LONG64*>(m_sharedRevision));
#else
    ++m_revision;
#if defined(__APPLE__)
    notify_post(m_name.c_str());
#elif defined(__linux__)
    // 只在正式提交时修改目录属性，其他进程通过 inotify 收到通知。
    std::error_code error;
    std::filesystem::last_write_time(m_directory, std::filesystem::file_time_type::clock::now(), error);
#endif
#endif
}
uint64_t HistoryChangeSignal::Revision() {
#if defined(_WIN32)
    return m_sharedRevision ? static_cast<uint64_t>(InterlockedCompareExchange64(
        static_cast<volatile LONG64*>(m_sharedRevision), 0, 0)) : 0;
#else
    bool changed = false;
#if defined(__APPLE__)
    int posted = 0;
    changed = m_handle != -1 && notify_check(int(m_handle), &posted) == NOTIFY_STATUS_OK && posted;
#elif defined(__linux__)
    alignas(inotify_event) char events[4096];
    ssize_t bytes = 0;
    while (m_handle != -1 && (bytes = read(int(m_handle), events, sizeof(events))) > 0) {
        for (size_t offset = 0; offset + sizeof(inotify_event) <= size_t(bytes);) {
            const auto* event = reinterpret_cast<const inotify_event*>(events + offset);
            // Setting only directory mtime emits IN_MODIFY on Linux. Child
            // asset writes have names and must not count as committed changes.
            if ((!event->len && (event->mask & (IN_ATTRIB | IN_MODIFY))) ||
                (event->mask & IN_Q_OVERFLOW)) changed = true;
            offset += sizeof(inotify_event) + event->len;
        }
    }
#endif
    if (changed) ++m_revision;
    return m_revision.load();
#endif
}
} // namespace shotlet
