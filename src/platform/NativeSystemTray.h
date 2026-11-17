#pragma once
#include "i18n/Localization.h"
#include <SDL3/SDL_rect.h>

struct SDL_Surface;
struct SDL_Window;

namespace shotlet {
enum class TrayAction { Recent, CopyLatest, History, Settings, Quit };
using TrayActionCallback = void (*)(void*, TrayAction);
struct NativeSystemTray;

// 原生托盘仅转发动作；历史读取、窗口创建和退出由应用主循环处理。
NativeSystemTray* CreateNativeSystemTray(SDL_Surface* icon, Language language, TrayActionCallback callback, void* data);
void DestroyNativeSystemTray(NativeSystemTray* tray);
void UpdateNativeSystemTrayIcon(NativeSystemTray* tray, SDL_Surface* icon);
SDL_Rect NativeSystemTrayAnchor(NativeSystemTray* tray);

// 弹层获取键盘焦点前保存前台应用，复制完成后仅在仍由弹层持有焦点时恢复。
struct PopupFocusToken;
PopupFocusToken* RememberPopupFocus();
void ReleasePopupFocus(PopupFocusToken* token, SDL_Window* popup, bool restore);
} // namespace shotlet
