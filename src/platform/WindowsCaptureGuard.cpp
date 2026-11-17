#include "WindowsCaptureGuard.h"

#include <array>
#include <system_error>
#include <utility>

namespace shotlet {
namespace {
constexpr auto MonitorInterval = std::chrono::milliseconds(50);
constexpr size_t DesktopNameCapacity = 256;
using DesktopName = std::array<wchar_t, DesktopNameCapacity>;

bool ReadDesktopName(HDESK desktop, DesktopName& name) {
    DWORD requiredBytes = 0;
    return desktop && GetUserObjectInformationW(desktop, UOI_NAME, name.data(),
                                                static_cast<DWORD>(sizeof(name)), &requiredBytes);
}

bool IsInputDesktopActive(const DesktopName& ownerDesktopName) {
    // Ctrl+Alt+Delete、锁屏和 UAC 会切换输入桌面；不能等主线程恢复后才撤下遮罩。
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop) return false;
    DesktopName inputName{};
    const bool matches = ReadDesktopName(desktop, inputName) && inputName == ownerDesktopName;
    CloseDesktop(desktop);
    return matches;
}
} // namespace

WindowsCaptureGuard::~WindowsCaptureGuard() { Stop(); }

bool WindowsCaptureGuard::Start(std::vector<HWND> windows) {
    Stop();
    if (windows.empty()) return false;
    m_windows = std::move(windows);
    m_stopping = false;
    m_reason = Reason::None;
    m_foregroundRequired = true;
    Heartbeat();
    try {
        m_thread = std::thread(&WindowsCaptureGuard::Monitor, this);
        return true;
    } catch (const std::system_error&) {
        m_windows.clear();
        return false;
    }
}

void WindowsCaptureGuard::Stop() {
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_wakeup.notify_one();
    if (m_thread.joinable()) m_thread.join();
    m_windows.clear();
}

void WindowsCaptureGuard::Heartbeat() {
    m_heartbeat = CaptureSafetyState::Clock::now().time_since_epoch().count();
}

void WindowsCaptureGuard::Monitor() {
    using Clock = CaptureSafetyState::Clock;
    DesktopName ownerDesktopName{};
    ReadDesktopName(GetThreadDesktop(GetCurrentThreadId()), ownerDesktopName);
    CaptureSafetyState state;
    std::unique_lock lock(m_mutex);
    while (!m_wakeup.wait_for(lock, MonitorInterval, [this] { return m_stopping; })) {
        lock.unlock();
        CaptureSafetyState::Sample sample;
        sample.foregroundRequired = m_foregroundRequired.load();
        for (HWND window : m_windows) {
            if (IsWindowVisible(window) && !IsIconic(window)) sample.anyWindowVisible = true;
        }
        if (sample.anyWindowVisible) {
            sample.inputDesktopActive = IsInputDesktopActive(ownerDesktopName);
            sample.escapeDown = GetAsyncKeyState(VK_ESCAPE) < 0;
            DWORD foregroundPid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
            sample.foregroundOwned = foregroundPid == GetCurrentProcessId();
        }
        const auto heartbeat = Clock::time_point(Clock::duration(m_heartbeat.load()));
        const Reason reason = state.Evaluate(Clock::now(), heartbeat, sample);
        if (reason != Reason::None) {
            m_reason = reason;
            // 保护线程不写日志：主线程可能正持有日志锁，救援路径不能再等它。
            MinimizeOverlays();
        }
        lock.lock();
    }
}

void WindowsCaptureGuard::MinimizeOverlays() {
    for (HWND window : m_windows) {
        if (!IsWindowVisible(window) || IsIconic(window)) continue;
        // 异步隐藏/降级仍要等窗口线程处理消息；FORCEMINIMIZE 可在该线程卡住时生效。
        // 无最小化样式的弹窗可能被隐藏而非置为 iconic；两种结果都已让出桌面。
        // 持续检查直到主线程接管取消，防止迟到的 ShowWindow 再次覆盖桌面。
        ShowWindow(window, SW_FORCEMINIMIZE);
    }
}

} // namespace shotlet
