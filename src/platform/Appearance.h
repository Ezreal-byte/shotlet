#pragma once
#include "ui/Theme.h"
struct SDL_Window;
namespace shotlet {
// 主线程查询；Windows 直接读应用配色偏好，覆盖无 SDL 窗口时的缓存缺口。
std::optional<Theme> QuerySystemTheme();
std::optional<Theme> QuerySystemTrayTheme();
void ApplyWindowTheme(SDL_Window* window, Theme theme);
// 仅供轻量弹层调用；半径采用 SDL 窗口坐标，Retina 下不再乘像素密度。
void ApplyPopupWindowCorners(SDL_Window* window, float radiusWindowUnits);
#if defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)
// 仅设置目标原生窗口的外观，不修改宿主应用的全局主题。
void ApplyNativeWindowTheme(void* window, Theme theme);
#endif
} // namespace shotlet
