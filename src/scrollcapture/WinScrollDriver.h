#pragma once
#include "i18n/Localization.h"

#include "platform/PlatformWin32.h"
#include "scrollcapture/ScrollCaptureTypes.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace shotlet {

class WinScrollDriver {
public:
    using IntentCallback = std::function<void(int64_t)>;

    struct WheelTelemetry {
        uint32_t events = 0;
        uint32_t ignoredInjected = 0;
        uint32_t inside = 0;
        int x = 0;
        int y = 0;
        int delta = 0;
        unsigned long flags = 0;
        uintptr_t extraInfo = 0;
    };

    WinScrollDriver() = default;
    ~WinScrollDriver();
    WinScrollDriver(const WinScrollDriver&) = delete;
    WinScrollDriver& operator=(const WinScrollDriver&) = delete;

    bool Start(const GlobalRect& selection, IntentCallback callback);
    void Stop();
    void RequestStop();
    void Inject(int64_t pixelDelta);
    bool ValidateTarget(std::string& error);
    TextId GetTargetErrorId() const { return m_targetErrorId; }
    WheelTelemetry DrainWheelTelemetry();
    [[nodiscard]] bool PassThroughInput() const { return m_passThroughInput.load(); }
    void SetPassThroughPaused(bool paused) { m_passThroughPaused.store(paused); }

    [[nodiscard]] bool CancelRequested() const { return m_cancelRequested.load(); }
    [[nodiscard]] const std::string& GetLastErrorMessage() const { return m_lastError; }

    LRESULT HandleMouse(WPARAM message, const MSLLHOOKSTRUCT& event);
    LRESULT HandleKeyboard(WPARAM message, const KBDLLHOOKSTRUCT& event);

private:
    void ThreadMain();

    GlobalRect m_selection{};
    IntentCallback m_callback;
    std::thread m_thread;
    std::atomic<DWORD> m_threadId{0};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_cancelRequested{false};
    std::atomic<bool> m_ready{false};
    std::atomic<int> m_injectX{0};
    std::atomic<int> m_injectY{0};
    std::atomic<bool> m_injectPointSet{false};
    std::atomic<unsigned> m_swallowedButtons{0};
    std::atomic<bool> m_passThroughInput{false};
    std::atomic<bool> m_passThroughPaused{false};
    std::atomic<int> m_inputDirection{0};
    std::atomic<uint32_t> m_wheelEvents{0};
    std::atomic<uint32_t> m_ignoredInjectedWheelEvents{0};
    std::atomic<uint32_t> m_insideWheelEvents{0};
    std::atomic<uint64_t> m_lastInsideWheelEventTick{0};
    std::atomic<int> m_lastWheelX{0};
    std::atomic<int> m_lastWheelY{0};
    std::atomic<int> m_lastWheelDelta{0};
    std::atomic<unsigned long> m_lastWheelFlags{0};
    std::atomic<uintptr_t> m_lastWheelExtraInfo{0};
    void* m_mouseHook = nullptr;
    void* m_keyboardHook = nullptr;
    void* m_targetWindow = nullptr;
    DWORD m_targetPid = 0;
    RECT m_targetBounds{};
    std::mutex m_readyMutex;
    std::condition_variable m_readyCv;
    TextId m_targetErrorId = TextId::ScrollHookLost;
    std::string m_lastError;
};

}  // namespace shotlet
