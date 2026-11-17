#pragma once

#include "CaptureSafetyState.h"
#include "PlatformWin32.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace shotlet {

// 仅持有会话内有效的 HWND，不接触 SDL、GPU 或画布。
// Start/Stop 仅由 UI 线程调用，调用方必须在销毁窗口前 Stop。
class WindowsCaptureGuard {
public:
    using Reason = CaptureSafetyState::Reason;
    ~WindowsCaptureGuard();
    bool Start(std::vector<HWND> windows);
    void Stop();
    void Heartbeat();
    void SetForegroundRequired(bool required) { m_foregroundRequired = required; }
    Reason GetReason() const { return m_reason.load(); }

private:
    void Monitor();
    void MinimizeOverlays();

    std::vector<HWND> m_windows;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wakeup;
    bool m_stopping = false;
    std::atomic<CaptureSafetyState::Clock::duration::rep> m_heartbeat{0};
    std::atomic<Reason> m_reason{Reason::None};
    std::atomic<bool> m_foregroundRequired{true};
};

} // namespace shotlet
