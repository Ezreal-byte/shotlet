#pragma once
#include "platform/PlatformWin32.h"
#include <algorithm>
#include <cstdint>
#include <format>
#include <string>

namespace shotlet::Win32Utils {
inline constexpr int64_t MediaTicksPerSecond = 10000000;

// WASAPI 的采样时间戳使用 QPC 时钟、100 ns 单位，不能混用其他时钟的纪元。
inline int64_t PerformanceCounter100ns() {
    LARGE_INTEGER value{}, frequency{};
    QueryPerformanceCounter(&value); QueryPerformanceFrequency(&frequency);
    // 先拆整秒，避免长时间开机后乘法溢出。
    return value.QuadPart / frequency.QuadPart * MediaTicksPerSecond +
        value.QuadPart % frequency.QuadPart * MediaTicksPerSecond / frequency.QuadPart;
}

inline bool CheckHResult(HRESULT result, const char* action, std::string& error) {
    if (SUCCEEDED(result)) return true;
    error = std::format("{} (0x{:08X})", action, static_cast<unsigned>(result));
    return false;
}

inline bool RegisterWindowClass(const wchar_t* name, WNDPROC procedure) {
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = name; type.lpfnWndProc = procedure;
    return RegisterClassW(&type) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

inline std::wstring WindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(size_t(length) + 1, L'\0');
    const int written = GetWindowTextW(window, text.data(), int(text.size()));
    text.resize(std::max(0, written));
    return text;
}

inline void SetWindowTextIfChanged(HWND window, const std::wstring& text) {
    // 播放时高频同步进度和按钮状态，未变化的文字不触发原生控件重绘，避免悬停闪烁。
    if (WindowText(window) != text) SetWindowTextW(window, text.c_str());
}
}
