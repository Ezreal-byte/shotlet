#include "ScreenOverlay.h"

#if !defined(SHOTLET_PLATFORM_WINDOWS)
#error "ScreenOverlay_win.cpp must only be compiled on Windows"
#endif

#include "platform/PlatformWin32.h"
#include "utils/Logger.h"

#include <SDL3/SDL.h>
#include <dwmapi.h>

namespace shotlet {

namespace {

#ifndef DWMWA_TRANSITIONS_FORCEDISABLED
constexpr DWORD DWMWA_TRANSITIONS_FORCEDISABLED = 3;
#endif

HWND GetOverlayHwnd(SDL_Window* window) {
    if (!window) {
        return nullptr;
    }

    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    return reinterpret_cast<HWND>(SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

void ConfigureOverlayHwnd(HWND hwnd) {
    if (!hwnd) {
        return;
    }

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    exStyle |= WS_EX_TOOLWINDOW;
    // 游戏或高权限窗口仍在前台时，全局按键轮询可能全部返回零。
    // 保留工具窗口样式避免任务栏闪烁，但允许截图窗口正常激活并接收输入。
    exStyle &= ~(WS_EX_APPWINDOW | WS_EX_NOACTIVATE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

    BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(hwnd,
                          DWMWA_TRANSITIONS_FORCEDISABLED,
                          &disableTransitions,
                          sizeof(disableTransitions));

    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
}

} // namespace

bool ScreenOverlay::CreateNativeWindow() {
    return true;
}

void ScreenOverlay::DestroyNativeWindow() {}

void ScreenOverlay::UpdateNativeBackground(const unsigned char*, int, int, int) {}

void ScreenOverlay::ShowNativeWindow() {
    HWND hwnd = GetOverlayHwnd(m_sdlWindow);
    if (!hwnd) {
        Logger::Warning("[ScreenOverlay:win] Failed to get HWND for overlay show");
        return;
    }

    ConfigureOverlayHwnd(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd, HWND_TOPMOST,
                 m_config.windowX, m_config.windowY,
                 m_config.windowWidth, m_config.windowHeight,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}

void ScreenOverlay::HideNativeWindow() {
    HWND hwnd = GetOverlayHwnd(m_sdlWindow);
    if (hwnd) {
        ShowWindow(hwnd, SW_HIDE);
    }
}

void ScreenOverlay::ClearNativeBackground() {}

void ScreenOverlay::HideSDLWindow() {}
void ScreenOverlay::RevealSDLWindow() {}

} // namespace shotlet
