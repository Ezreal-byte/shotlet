#include "SmartRegionDetector.h"

#if !defined(SHOTLET_PLATFORM_WINDOWS)
#error "SmartRegionDetector_win.cpp must only be compiled on Windows"
#endif

#include "platform/PlatformWin32.h"
#include <dwmapi.h>

namespace shotlet {

// 辅助：宽字符串转 UTF-8
static std::string WideToUtf8(const wchar_t* wide) {
    if (!wide || !*wide) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    // len 包含 null 终止符，使用 vector 避免写入 std::string 尾部之外
    std::vector<char> buffer(len);
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, buffer.data(), len, nullptr, nullptr);
    // 排除 null 终止符
    return std::string(buffer.data(), len - 1);
}

// 辅助：判断窗口类名是否为需要特殊处理的 Shell 窗口
// Shell_TrayWnd: 主任务栏（应保留，即使有 WS_EX_TOOLWINDOW）
// Shell_SecondaryTrayWnd: 多显示器副任务栏（应保留）
// Progman: 桌面背景窗口（应排除，覆盖全屏）
// WorkerW: 桌面渲染窗口（应排除，覆盖全屏）
static bool IsShellTrayWindow(HWND hwnd) {
    wchar_t className[256] = {};
    GetClassNameW(hwnd, className, 256);
    return wcscmp(className, L"Shell_TrayWnd") == 0 ||
           wcscmp(className, L"Shell_SecondaryTrayWnd") == 0;
}

static bool IsDesktopBackgroundWindow(HWND hwnd) {
    wchar_t className[256] = {};
    GetClassNameW(hwnd, className, 256);
    return wcscmp(className, L"Progman") == 0 ||
           wcscmp(className, L"WorkerW") == 0;
}

// 辅助：判断窗口是否为系统覆盖层（不应作为智能选区）
// "Windows 输入体验"是 IME 输入面板的透明覆盖层，覆盖全屏但用户不可见，
// z-order 高于任务栏，导致任务栏区域被它抢占匹配
static bool IsSystemOverlayWindow(HWND hwnd) {
    wchar_t className[256] = {};
    GetClassNameW(hwnd, className, 256);
    if (wcscmp(className, L"Windows.UI.Core.CoreWindow") == 0) {
        wchar_t title[256] = {};
        GetWindowTextW(hwnd, title, 256);
        return wcscmp(title, L"Windows 输入体验") == 0 ||
               wcscmp(title, L"Windows Input Experience") == 0;
    }
    return false;
}

// EnumWindows 回调需要访问的上下文
struct EnumContext {
    DWORD selfPid;
    std::vector<RawWindow> windows;
};

static BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<EnumContext*>(lParam);

    // 排除不可见窗口
    if (!IsWindowVisible(hwnd)) return TRUE;

    // 排除最小化窗口
    if (IsIconic(hwnd)) return TRUE;

    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return TRUE;
    }

    // 获取窗口类名和扩展样式（用于过滤和任务栏识别）
    wchar_t classNameW[256] = {};
    GetClassNameW(hwnd, classNameW, 256);
    std::string className = WideToUtf8(classNameW);
    wchar_t titleBuf[256] = {};
    GetWindowTextW(hwnd, titleBuf, 256);
    std::string label = WideToUtf8(titleBuf);
    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);

    // 排除桌面背景窗口（Progman/WorkerW 覆盖全屏，会导致智能选区变为整个屏幕）
    if (IsDesktopBackgroundWindow(hwnd)) {
        return TRUE;
    }

    // 排除系统覆盖层（IME 输入面板等，覆盖全屏但用户不可见）
    if (IsSystemOverlayWindow(hwnd)) {
        return TRUE;
    }

    // 排除系统工具窗口（浮动工具栏、通知等），但保留 Shell 窗口（任务栏）
    if ((exStyle & WS_EX_TOOLWINDOW) && !IsShellTrayWindow(hwnd)) {
        return TRUE;
    }

    // 排除自身进程窗口
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->selfPid) return TRUE;

    // 获取窗口边界（优先 DWM 扩展边界，包含阴影）
    RECT rect = {};
    HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                        &rect, sizeof(rect));
    if (FAILED(hr)) {
        GetWindowRect(hwnd, &rect);
    }

    float width = static_cast<float>(rect.right - rect.left);
    float height = static_cast<float>(rect.bottom - rect.top);

    // 排除极小窗口
    if (width < 10.0f || height < 10.0f) return TRUE;

    ImRect globalBounds(
        ImVec2(static_cast<float>(rect.left), static_cast<float>(rect.top)),
        ImVec2(static_cast<float>(rect.right), static_cast<float>(rect.bottom)));

    ctx->windows.push_back({
        globalBounds,
        label,
        className
    });
    return TRUE;
}

void SmartRegionDetector::BuildCache(float screenOffsetX, float screenOffsetY,
                                     float screenW, float screenH) {
    m_cache.clear();

    EnumContext ctx;
    ctx.selfPid = GetCurrentProcessId();

    // 第一轮：枚举所有顶层可见窗口（EnumWindows 按 z-order 从上到下调用）
    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&ctx));

    BuildCacheFromWindows(ctx.windows, screenOffsetX, screenOffsetY, screenW, screenH);
}

} // namespace shotlet
