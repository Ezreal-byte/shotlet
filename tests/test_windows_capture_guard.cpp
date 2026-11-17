#include "platform/WindowsCaptureGuard.h"
#include <gtest/gtest.h>

namespace shotlet {
namespace {
constexpr wchar_t TestWindowClass[] = L"ShotletCaptureGuardTest";
constexpr int TestWindowSizePixels = 120;
constexpr auto ObserveInterval = std::chrono::milliseconds(10);
constexpr auto RecoveryMargin = std::chrono::seconds(2);

class WindowsCaptureGuardTest : public testing::Test {
protected:
    void SetUp() override {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = TestWindowClass;
        if (!RegisterClassW(&windowClass)) ASSERT_EQ(GetLastError(), ERROR_CLASS_ALREADY_EXISTS);
        for (int index = 0; index < WindowCount; ++index) {
            HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TestWindowClass, L"",
                WS_POPUP, index * TestWindowSizePixels, 0, TestWindowSizePixels, TestWindowSizePixels,
                nullptr, nullptr, windowClass.hInstance, nullptr);
            ASSERT_NE(window, nullptr);
            m_windows.push_back(window);
            ShowWindow(window, SW_SHOWNOACTIVATE);
        }
        SetForegroundWindow(m_windows.front());
        DWORD foregroundPid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
        if (foregroundPid != GetCurrentProcessId()) GTEST_SKIP() << "测试需要前台桌面权限";
    }

    void TearDown() override {
        m_guard.Stop();
        for (HWND window : m_windows) DestroyWindow(window);
        UnregisterClassW(TestWindowClass, GetModuleHandleW(nullptr));
    }

    static constexpr int WindowCount = 2;
    WindowsCaptureGuard m_guard;
    std::vector<HWND> m_windows;
};

TEST_F(WindowsCaptureGuardTest, StalledWindowThreadIsMinimizedWithoutPumpingItsMessages) {
    ASSERT_TRUE(m_guard.Start(m_windows));
    const auto deadline = CaptureSafetyState::Clock::now() + CaptureSafetyState::HeartbeatTimeout + RecoveryMargin;
    // 故意不处理任何窗口消息，模拟 GPU/游戏交互导致 UI 线程停滞。
    bool allMinimized = false;
    while (CaptureSafetyState::Clock::now() < deadline) {
        allMinimized = true;
        for (HWND window : m_windows) allMinimized &= IsIconic(window) != FALSE;
        if (allMinimized) break;
        std::this_thread::sleep_for(ObserveInterval);
    }
    EXPECT_EQ(m_guard.GetReason(), WindowsCaptureGuard::Reason::Unresponsive);
    EXPECT_TRUE(allMinimized);
    m_guard.Stop();
    // daemon 会复用同一组 HWND；保护最小化不能让下一次截图一直处于最小化状态。
    for (HWND window : m_windows) {
        ShowWindow(window, SW_SHOWNOACTIVATE);
        EXPECT_FALSE(IsIconic(window));
    }
    ASSERT_TRUE(m_guard.Start(m_windows));
    EXPECT_EQ(m_guard.GetReason(), WindowsCaptureGuard::Reason::None);
}

TEST_F(WindowsCaptureGuardTest, StopJoinsWorkerBeforeWindowReuseAndResetsNextSession) {
    ASSERT_TRUE(m_guard.Start(m_windows));
    m_guard.Stop();
    for (HWND window : m_windows) EXPECT_FALSE(IsIconic(window));
    ASSERT_TRUE(m_guard.Start(m_windows));
    m_guard.Heartbeat();
    EXPECT_EQ(m_guard.GetReason(), WindowsCaptureGuard::Reason::None);
}
} // namespace
} // namespace shotlet
