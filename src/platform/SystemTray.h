#pragma once
#include "i18n/Localization.h"
#include "ui/Theme.h"
#include <SDL3/SDL_rect.h>

struct SDL_Tray;
struct SDL_TrayEntry;

namespace shotlet {
struct NativeSystemTray;

// 托盘及菜单由主线程创建和销毁，退出请求由应用主循环处理。
class SystemTray {
public:
    SystemTray() = default;
    ~SystemTray();
    SystemTray(const SystemTray&) = delete;
    SystemTray& operator=(const SystemTray&) = delete;

    bool Create(Language language = Language::English, Theme systemTheme = Theme::Dark);
    void Destroy();
    bool IsQuitRequested() const { return m_quitRequested; }
    bool ConsumeSettingsRequest();
    bool ConsumeHistoryRequest();
    bool ConsumeRecentRequest();
    bool ConsumeCopyLatestRequest();
    SDL_Rect GetAnchor() const;
    void UpdateIcon(Theme systemTheme);

private:
    SDL_Tray* m_tray = nullptr;
    NativeSystemTray* m_native = nullptr;
    bool m_quitRequested = false;
    bool m_settingsRequested = false;
    bool m_historyRequested = false;
    bool m_recentRequested = false;
    bool m_copyLatestRequested = false;
};

} // namespace shotlet
