#include "Appearance.h"
#include "PlatformWin32.h"
#include <SDL3/SDL.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
namespace shotlet {
namespace {
std::optional<Theme> ReadTheme(const wchar_t* property) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        property, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return {};
    return value ? Theme::Light : Theme::Dark;
}
}
std::optional<Theme> QuerySystemTheme() { return ReadTheme(L"AppsUseLightTheme"); }
std::optional<Theme> QuerySystemTrayTheme() { return ReadTheme(L"SystemUsesLightTheme"); }

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD DWMWA_WINDOW_CORNER_PREFERENCE = 33;
#endif

void ApplyWindowCornerPreference(HWND hwnd) {
    if (!hwnd) return;
    // DWMWCP_ROUND 为 Windows 11 标准圆角；旧系统上调用失败仅保持直角，不视为错误。
    const int cornerPreference = 2 /* DWMWCP_ROUND */;
    DwmSetWindowAttribute(hwnd, static_cast<DWMWINDOWATTRIBUTE>(DWMWA_WINDOW_CORNER_PREFERENCE),
                          &cornerPreference, sizeof(cornerPreference));
}

void ApplyNativeWindowTheme(void* window, Theme theme) {
    HWND native = static_cast<HWND>(window);
    if (!native) return;
    const auto& p = GetThemePalette(theme);
    BOOL dark = theme == Theme::Dark;
    COLORREF background = Colors::ToNativeRgb(p.editorToolbarBackground), text = Colors::ToNativeRgb(p.text), border = Colors::ToNativeRgb(p.border);
    DwmSetWindowAttribute(native, static_cast<DWMWINDOWATTRIBUTE>(20), &dark, sizeof(dark));
    DwmSetWindowAttribute(native, static_cast<DWMWINDOWATTRIBUTE>(35), &background, sizeof(background));
    DwmSetWindowAttribute(native, static_cast<DWMWINDOWATTRIBUTE>(36), &text, sizeof(text));
    DwmSetWindowAttribute(native, static_cast<DWMWINDOWATTRIBUTE>(34), &border, sizeof(border));
    ApplyWindowCornerPreference(native);
}
void ApplyWindowTheme(SDL_Window* window, Theme theme) {
    if (!window) return;
    ApplyNativeWindowTheme(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr), theme);
}
void ApplyPopupWindowCorners(SDL_Window* window, float radiusWindowUnits) {
    if (!window) return;
    HWND native = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    RECT bounds{};
    if (!native || !GetWindowRect(native, &bounds)) return;
    constexpr int minimumDiameterPixels = 1;
    const int diameterPixels = std::max(minimumDiameterPixels, int(std::lround(radiusWindowUnits * 2)));
    // 窗口区域使用实际像素；+1 保留右侧和底部边缘，Windows 10 也能得到相同的圆角。
    HRGN region = CreateRoundRectRgn(0, 0, bounds.right - bounds.left + 1, bounds.bottom - bounds.top + 1,
        diameterPixels, diameterPixels);
    if (!region) return;
    // 设置成功后区域归系统所有，失败时仍由当前函数释放。
    if (!SetWindowRgn(native, region, TRUE)) DeleteObject(region);
}
} // namespace shotlet
