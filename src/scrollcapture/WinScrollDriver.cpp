#include "scrollcapture/WinScrollDriver.h"

#if !defined(SHOTLET_PLATFORM_WINDOWS)
#error "WinScrollDriver.cpp 只能在 Windows 上编译"
#endif

#include "scrollcapture/ScrollPointerGuard.h"
#include "scrollcapture/WinScrollUnits.h"
#include "utils/Logger.h"

#include <algorithm>

namespace shotlet {
namespace {

constexpr ULONG_PTR kSyntheticScrollMarker = winwheel::kSyntheticScrollMarker;
std::atomic<WinScrollDriver*> g_activeDriver{nullptr};

LRESULT CALLBACK MouseHookProc(int code, WPARAM message, LPARAM data) {
    if (code >= 0) {
        if (auto* driver = g_activeDriver.load()) {
            const auto& event = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
            const LRESULT result = driver->HandleMouse(message, event);
            if (result != 0) return result;
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}

LRESULT CALLBACK KeyboardHookProc(int code, WPARAM message, LPARAM data) {
    if (code >= 0) {
        if (auto* driver = g_activeDriver.load()) {
            const auto& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
            const LRESULT result = driver->HandleKeyboard(message, event);
            if (result != 0) return result;
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}

bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top &&
           a.right == b.right && a.bottom == b.bottom;
}

}  // namespace

WinScrollDriver::~WinScrollDriver() {
    Stop();
}

bool WinScrollDriver::Start(const GlobalRect& selection, IntentCallback callback) {
    Stop();
    if (selection.w <= 0 || selection.h <= 0 || !callback) {
        m_lastError = "滚动输入驱动参数无效";
        return false;
    }
    WinScrollDriver* expected = nullptr;
    if (!g_activeDriver.compare_exchange_strong(expected, this)) {
        m_lastError = "已有滚动截图输入驱动正在运行";
        return false;
    }
    m_selection = selection;
    m_callback = std::move(callback);
    m_stopRequested.store(false);
    m_cancelRequested.store(false);
    m_ready.store(false);
    m_injectPointSet.store(false);
    m_swallowedButtons.store(0);
    m_passThroughInput.store(false);
    m_passThroughPaused.store(false);
    m_inputDirection.store(0);
    m_targetWindow = nullptr;
    m_targetPid = 0;
    m_lastError.clear();
    m_lastInsideWheelEventTick.store(0);
    m_thread = std::thread([this] { ThreadMain(); });

    std::unique_lock lock(m_readyMutex);
    m_readyCv.wait_for(lock, std::chrono::seconds(2), [this] { return m_ready.load(); });
    if (!m_ready.load() || !m_mouseHook || !m_keyboardHook) {
        if (m_lastError.empty()) m_lastError = "安装 Windows 输入 hook 超时";
        Stop();
        return false;
    }
    Logger::Info("[WinScrollCapture][input] hooks ready selection=({},{} {}x{})",
                 selection.x, selection.y, selection.w, selection.h);
    return true;
}

void WinScrollDriver::ThreadMain() {
    m_threadId.store(GetCurrentThreadId());
    // 提前创建消息队列，保证停止线程可以投递 WM_QUIT。
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandleW(nullptr), 0);
    m_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, GetModuleHandleW(nullptr), 0);
    if (!m_mouseHook || !m_keyboardHook) {
        m_lastError = "无法安装 Windows 低级输入 hook";
    }
    m_ready.store(true);
    m_readyCv.notify_all();
    while (!m_stopRequested.load() && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (m_keyboardHook) UnhookWindowsHookEx(static_cast<HHOOK>(m_keyboardHook));
    if (m_mouseHook) UnhookWindowsHookEx(static_cast<HHOOK>(m_mouseHook));
    m_keyboardHook = m_mouseHook = nullptr;
    m_threadId.store(0);
}

void WinScrollDriver::RequestStop() {
    m_stopRequested.store(true);
    const DWORD threadId = m_threadId.load();
    if (threadId) PostThreadMessageW(threadId, WM_QUIT, 0, 0);
}

void WinScrollDriver::Stop() {
    RequestStop();
    if (m_thread.joinable()) m_thread.join();
    WinScrollDriver* expected = this;
    g_activeDriver.compare_exchange_strong(expected, nullptr);
    m_callback = nullptr;
}

LRESULT WinScrollDriver::HandleKeyboard(WPARAM message, const KBDLLHOOKSTRUCT& event) {
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && event.vkCode == VK_ESCAPE) {
        m_cancelRequested.store(true);
        return 1;
    }
    return 0;
}

LRESULT WinScrollDriver::HandleMouse(WPARAM message, const MSLLHOOKSTRUCT& event) {
    const bool wheelEvent = message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL;
    const bool injected = winwheel::IsInjectedEvent(
        event.flags, event.dwExtraInfo, kSyntheticScrollMarker);
    const bool passThrough = winwheel::IsPassThroughEvent(
        event.flags, event.dwExtraInfo, kSyntheticScrollMarker);
    if (wheelEvent) {
        m_wheelEvents.fetch_add(1);
        m_lastWheelX.store(event.pt.x);
        m_lastWheelY.store(event.pt.y);
        m_lastWheelDelta.store(static_cast<short>(HIWORD(event.mouseData)));
        m_lastWheelFlags.store(event.flags);
        m_lastWheelExtraInfo.store(event.dwExtraInfo);
        if (injected) m_ignoredInjectedWheelEvents.fetch_add(1);
    }
    if (injected) return 0;
    const int x = event.pt.x;
    const int y = event.pt.y;
    const bool inside = Contains(m_selection, x, y);
    if (wheelEvent) {
        if (inside) m_insideWheelEvents.fetch_add(1);
        if (!inside) return 0;
        m_injectX.store(x);
        m_injectY.store(y);
        m_injectPointSet.store(true);
        if (message == WM_MOUSEWHEEL) {
            const auto delta = static_cast<short>(HIWORD(event.mouseData));
            const int64_t intent = winwheel::WheelDeltaToIntent(delta);
            if (intent != 0 && m_callback) {
                const int direction = intent > 0 ? 1 : -1;
                int expected = 0;
                m_inputDirection.compare_exchange_strong(expected, direction);
                // 先拦截反向事件，再进入透传分支，避免页面已经滚动才通知控制器。
                if (m_inputDirection.load() != direction) {
                    m_callback(direction * 120);
                    return 1;
                }
                if (passThrough) {
                    m_passThroughInput.store(true);
                    const uint64_t now = GetTickCount64();
                    const uint64_t previous = m_lastInsideWheelEventTick.exchange(now);
                    // 过滤远程滚轮的连续事件；首个事件放行时立即关闭入口，避免后台轮询期间滚过头。
                    if (m_passThroughPaused.load() ||
                        (previous != 0 && now - previous < 180)) return 1;
                    if (m_passThroughPaused.exchange(true)) return 1;
                    // 高精度滚轮首个事件可能不足一格，但已放行的事件必须触发采集。
                    m_callback(intent > 0 ? 120 : -120);
                    return 0;
                }
                // 输入模式跟随当前本机事件，不能沿用之前的外部注入事件所启用的透传状态。
                m_passThroughInput.store(false);
                m_callback(intent);
            }
        }
        return passThrough ? 0 : 1;
    }

    ScrollPointerAction action;
    unsigned button = 0;
    bool pointerEvent = true;
    switch (message) {
        case WM_LBUTTONDOWN: action = ScrollPointerAction::Down; button = 0; break;
        case WM_LBUTTONUP: action = ScrollPointerAction::Up; button = 0; break;
        case WM_RBUTTONDOWN: action = ScrollPointerAction::Down; button = 1; break;
        case WM_RBUTTONUP: action = ScrollPointerAction::Up; button = 1; break;
        case WM_MBUTTONDOWN: action = ScrollPointerAction::Down; button = 2; break;
        case WM_MBUTTONUP: action = ScrollPointerAction::Up; button = 2; break;
        case WM_MOUSEMOVE:
            action = ScrollPointerAction::Drag;
            button = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 0 :
                     (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 1 : 2;
            if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0 &&
                (GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0 &&
                (GetAsyncKeyState(VK_MBUTTON) & 0x8000) == 0) pointerEvent = false;
            break;
        default: pointerEvent = false; break;
    }
    if (!pointerEvent) return 0;
    unsigned swallowed = m_swallowedButtons.load();
    const bool suppress = SuppressScrollPointer(m_selection, {}, x, y, button, action, swallowed);
    m_swallowedButtons.store(swallowed);
    return suppress ? 1 : 0;
}

WinScrollDriver::WheelTelemetry WinScrollDriver::DrainWheelTelemetry() {
    WheelTelemetry telemetry;
    telemetry.events = m_wheelEvents.exchange(0);
    telemetry.ignoredInjected = m_ignoredInjectedWheelEvents.exchange(0);
    telemetry.inside = m_insideWheelEvents.exchange(0);
    telemetry.x = m_lastWheelX.load();
    telemetry.y = m_lastWheelY.load();
    telemetry.delta = m_lastWheelDelta.load();
    telemetry.flags = m_lastWheelFlags.load();
    telemetry.extraInfo = m_lastWheelExtraInfo.load();
    return telemetry;
}

void WinScrollDriver::Inject(int64_t pixelDelta) {
    if (pixelDelta == 0 || !m_injectPointSet.load() || m_stopRequested.load()) {
        Logger::Warning("[WinScrollCapture][inject] skipped pixelDelta={} pointSet={} stopping={}",
                        pixelDelta, m_injectPointSet.load(), m_stopRequested.load());
        return;
    }
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(winwheel::PixelIntentToWheelData(pixelDelta));
    input.mi.dwExtraInfo = kSyntheticScrollMarker;
    const UINT sent = SendInput(1, &input, sizeof(input));
    Logger::Debug("[WinScrollCapture][inject] pixelDelta={} wheelData={} point=({}, {}) sent={}",
                 pixelDelta, static_cast<int32_t>(input.mi.mouseData),
                 m_injectX.load(), m_injectY.load(), sent);
    if (sent != 1) {
        m_lastError = "SendInput 注入滚轮失败";
        Logger::Warning("[WinScrollCapture][inject] SendInput failed error={}", GetLastError());
    }
}

bool WinScrollDriver::ValidateTarget(std::string& error) {
    if (!m_mouseHook || !m_keyboardHook) {
        m_targetErrorId = TextId::ScrollHookLost;
        error = "滚轮拦截失效，请结束采集后重试";
        return false;
    }
    if (!m_injectPointSet.load()) return true;
    POINT point{m_injectX.load(), m_injectY.load()};
    HWND target = WindowFromPoint(point);
    if (target) target = GetAncestor(target, GA_ROOT);
    if (!target || !IsWindow(target)) {
        m_targetErrorId = TextId::ScrollTargetMissing;
        error = "无法确定滚动目标窗口";
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    RECT bounds{};
    GetWindowRect(target, &bounds);
    if (!m_targetWindow) {
        if (pid == GetCurrentProcessId()) {
            m_targetErrorId = TextId::ScrollTargetSelf;
            error = "滚动目标不能是 Shotlet 窗口";
            return false;
        }
        m_targetWindow = target;
        m_targetPid = pid;
        m_targetBounds = bounds;
        Logger::Info("[WinScrollCapture][target] locked hwnd={} pid={} bounds=({},{} {}x{}) point=({}, {})",
                     reinterpret_cast<uintptr_t>(target), pid, bounds.left, bounds.top,
                     bounds.right - bounds.left, bounds.bottom - bounds.top, point.x, point.y);
        return true;
    }
    if (target != static_cast<HWND>(m_targetWindow) || pid != m_targetPid ||
        !SameRect(bounds, m_targetBounds)) {
        Logger::Warning("[WinScrollCapture][target] changed actualHwnd={} actualPid={} bounds=({},{} {}x{}) lockedHwnd={} lockedPid={}",
                        reinterpret_cast<uintptr_t>(target), pid, bounds.left, bounds.top,
                        bounds.right - bounds.left, bounds.bottom - bounds.top,
                        reinterpret_cast<uintptr_t>(m_targetWindow), m_targetPid);
        m_targetErrorId = TextId::ScrollTargetChanged;
        error = "目标窗口已移动、关闭或被遮挡";
        return false;
    }
    return true;
}

}  // namespace shotlet
