#include "HotkeyService.h"
#include "utils/Logger.h"

#if !defined(SHOTLET_PLATFORM_WINDOWS)
#error "HotkeyService_win.cpp must only be compiled on Windows"
#endif

#include "platform/PlatformWin32.h"

#include <array>

namespace shotlet {

// 全局状态
static HWND s_hotkeyWindow = nullptr;
static ATOM s_windowClassAtom = 0;
struct Registration {
    int id;
    bool registered = false, triggered = false;
    int modifiers = 0, key = 0;
};
constexpr int kHotkeyIdsPerAction = 2;
static auto s_registrations = [] {
    std::array<Registration, size_t(HotkeyService::Action::Count)> result{};
    for (size_t i = 0; i < result.size(); ++i) result[i].id = int(i) * kHotkeyIdsPerAction + 1;
    return result;
}();
static bool s_suspended = false;

// 修饰符描述辅助函数
static std::string DescribeModifiers(int modifiers) {
    std::string desc;
    if (modifiers & HOTKEY_MOD_SUPER) desc += "Win+";
    if (modifiers & HOTKEY_MOD_CTRL)  desc += "Ctrl+";
    if (modifiers & HOTKEY_MOD_ALT)   desc += "Alt+";
    if (modifiers & HOTKEY_MOD_SHIFT) desc += "Shift+";
    return desc;
}

// 键码描述辅助函数
static std::string DescribeKey(int key) {
    if (key >= 'A' && key <= 'Z') {
        return std::string(1, static_cast<char>(key));
    }
    return std::to_string(key);
}

// 将跨平台修饰符转换为 Win32 修饰符
static UINT ToWin32Modifiers(int modifiers) {
    UINT winMods = 0;
    if (modifiers & HOTKEY_MOD_SUPER) winMods |= MOD_WIN;
    if (modifiers & HOTKEY_MOD_CTRL)  winMods |= MOD_CONTROL;
    if (modifiers & HOTKEY_MOD_ALT)   winMods |= MOD_ALT;
    if (modifiers & HOTKEY_MOD_SHIFT) winMods |= MOD_SHIFT;
    // MOD_NOREPEAT: 防止按住时连续触发
    winMods |= MOD_NOREPEAT;
    return winMods;
}

// 隐藏窗口的消息处理
static LRESULT CALLBACK HotkeyWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_HOTKEY) {
        for (auto& registration : s_registrations)
            if (!s_suspended && registration.registered && wParam == registration.id) registration.triggered = true;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool HotkeyService::Register(int modifiers, int key, std::string& conflictMsg, Language language, Action action) {
    conflictMsg.clear();
    auto& registration = s_registrations[size_t(action)];
    if (registration.registered && registration.modifiers == modifiers && registration.key == key) return true;

    // 创建隐藏的 message-only 窗口用于接收 WM_HOTKEY
    if (!s_hotkeyWindow) {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = HotkeyWindowProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = "ShotletHotkeyWindow";
        if (!s_windowClassAtom) s_windowClassAtom = RegisterClassA(&wc);

        if (!s_windowClassAtom) {
            Logger::Error("[HotkeyService] RegisterClass failed: {}", GetLastError());
            conflictMsg = Tr(language, TextId::HotkeyClassFailed);
            return false;
        }

        s_hotkeyWindow = CreateWindowExA(0, "ShotletHotkeyWindow", "",
                                          0, 0, 0, 0, 0,
                                          HWND_MESSAGE, nullptr, nullptr, nullptr);
        if (!s_hotkeyWindow) {
            Logger::Error("[HotkeyService] CreateWindow failed: {}", GetLastError());
            conflictMsg = Tr(language, TextId::HotkeyWindowFailed);
            return false;
        }
    }

    // 注册全局快捷键
    UINT winMods = ToWin32Modifiers(modifiers);
    const int firstId = int(action) * kHotkeyIdsPerAction + 1;
    const int newId = registration.id == firstId ? firstId + 1 : firstId;
    BOOL result = RegisterHotKey(s_hotkeyWindow, newId, winMods, key);

    if (!result) {
        DWORD err = GetLastError();
        std::string keyDesc = DescribeModifiers(modifiers) + DescribeKey(key);
        if (err == ERROR_HOTKEY_ALREADY_REGISTERED) {
            conflictMsg = FormatText(language, TextId::HotkeyInUse, keyDesc);
        } else {
            conflictMsg = FormatText(language, TextId::HotkeyRegisterFailed, keyDesc, err);
        }
        Logger::Error("[HotkeyService] RegisterHotKey failed: {} (key={}, error={})",
                       result, keyDesc, err);
        return false;
    }

    if (registration.registered) UnregisterHotKey(s_hotkeyWindow, registration.id);
    if (s_suspended) UnregisterHotKey(s_hotkeyWindow, newId);
    registration = {newId, true, false, modifiers, key};
    Logger::Info("[HotkeyService] Registered: {}{}",
                 DescribeModifiers(modifiers), DescribeKey(key));
    return true;
}

void HotkeyService::Unregister(Action action) {
    auto& registration = s_registrations[size_t(action)];
    if (registration.registered) UnregisterHotKey(s_hotkeyWindow, registration.id);
    registration.registered = registration.triggered = false;
}

void HotkeyService::SetSuspended(bool suspended) {
    if (s_suspended == suspended) return;
    for (auto& registration : s_registrations) {
        registration.triggered = false;
        if (!registration.registered) continue;
        // 录入时释放系统注册，否则 Windows 会吞掉已经注册的组合键。
        if (suspended) UnregisterHotKey(s_hotkeyWindow, registration.id);
        else if (!RegisterHotKey(s_hotkeyWindow, registration.id, ToWin32Modifiers(registration.modifiers), registration.key)) {
            registration.registered = false;
            Logger::Error("[HotkeyService] Cannot restore shortcut: {}", GetLastError());
        }
    }
    s_suspended = suspended;
}

bool HotkeyService::Poll(Action action) {
    auto& registration = s_registrations[size_t(action)];
    const bool triggered = registration.triggered;
    registration.triggered = false;
    return triggered;
}

} // namespace shotlet
