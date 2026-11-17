#include "TransparentWindow.h"
#include "utils/Logger.h"

#if !defined(SHOTLET_PLATFORM_WINDOWS)
#error "TransparentWindow_win.cpp must only be compiled on Windows"
#endif

#include <SDL3/SDL.h>
#include "platform/PlatformWin32.h"
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

namespace shotlet {

void ConfigureWindowTransparent(SDL_Window* window) {
    if (!window) return;

    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    HWND hwnd = (HWND)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!hwnd) {
        Logger::Warning("Failed to get HWND from SDL window");
        return;
    }

    // 确保不设置 WS_EX_TRANSPARENT，防止鼠标穿透
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    exStyle &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

    // 加固置顶
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // DWM blur-behind 确保合成器正确处理透明
    DWM_BLURBEHIND bb = {};
    bb.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
    bb.fEnable = TRUE;
    bb.hRgnBlur = CreateRectRgn(0, 0, -1, -1);
    DwmEnableBlurBehindWindow(hwnd, &bb);
    if (bb.hRgnBlur) DeleteObject(bb.hRgnBlur);

    Logger::Info("Window transparency configured on Windows");
}

void SetWindowTransparent(SDL_Window* window) {
    if (!window) return;

    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    HWND hwnd = (HWND)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!hwnd) {
        Logger::Warning("Failed to get HWND from SDL window");
        return;
    }

    // 确保不设置 WS_EX_TRANSPARENT，防止鼠标穿透
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    exStyle &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

    // 加固置顶
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // DWM blur-behind 确保合成器正确处理透明
    DWM_BLURBEHIND bb = {};
    bb.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
    bb.fEnable = TRUE;
    bb.hRgnBlur = CreateRectRgn(0, 0, -1, -1);
    DwmEnableBlurBehindWindow(hwnd, &bb);
    if (bb.hRgnBlur) DeleteObject(bb.hRgnBlur);

    // 激活并获取焦点
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    Logger::Info("Window transparency enabled on Windows");
}

void SetWindowOpaque(SDL_Window* window) {
    if (!window) return;

    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    HWND hwnd = (HWND)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!hwnd) return;

    DWM_BLURBEHIND bb = {};
    bb.dwFlags = DWM_BB_ENABLE;
    bb.fEnable = FALSE;
    DwmEnableBlurBehindWindow(hwnd, &bb);

    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void ForceWindowFocus(SDL_Window* window) {
    if (!window) return;

    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    HWND hwnd = (HWND)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!hwnd) return;

    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
}

bool RestoreActivationPolicy() {
    // Windows 无需恢复激活策略
    return true;
}

} // namespace shotlet
