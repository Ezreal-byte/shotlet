#include "SystemTray.h"
#include "NativeSystemTray.h"

#include <SDL3/SDL.h>
#include "utils/ApplicationIcon.h"

namespace shotlet {

SystemTray::~SystemTray() {
    Destroy();
}

namespace {
#if defined(SHOTLET_PLATFORM_WINDOWS)
constexpr int TrayIconPaddingPixels = 0;
#else
// macOS 保留原有透明留白，避免图形在菜单栏中比其他应用偏大。
constexpr int TrayIconPaddingPixels = 3;
#endif
constexpr int TrayIconCanvasSizePixels = ApplicationIcon::ArtworkSizePixels + TrayIconPaddingPixels * 2;

ApplicationIcon::Surface MakeTrayIcon(Theme theme) {
    const ImU32 color = theme == Theme::Dark ? Colors::DarkTrayIcon : Colors::LightTrayIcon;
    return ApplicationIcon::CreateSurface(color, TrayIconCanvasSizePixels);
}
} // namespace

bool SystemTray::Create(Language language, Theme systemTheme) {
    if (m_tray || m_native) return true;
    m_quitRequested = false;

    auto icon = MakeTrayIcon(systemTheme);
    if (!icon) return false;

#if (defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)) && !defined(SHOTLET_TRAY_SDL_BACKEND)
    m_native = CreateNativeSystemTray(icon.get(), language, [](void* data, TrayAction action) {
        auto& tray = *static_cast<SystemTray*>(data);
        switch (action) {
            case TrayAction::Recent: tray.m_recentRequested = true; break;
            case TrayAction::CopyLatest: tray.m_copyLatestRequested = true; break;
            case TrayAction::History: tray.m_historyRequested = true; break;
            case TrayAction::Settings: tray.m_settingsRequested = true; break;
            case TrayAction::Quit: tray.m_quitRequested = true; break;
        }
    }, this);
    return m_native != nullptr;
#else
    m_tray = SDL_CreateTray(icon.get(), Tr(language, TextId::TrayTooltip));
    if (!m_tray) return false;

    SDL_TrayMenu* menu = SDL_CreateTrayMenu(m_tray);
    if (!menu) { Destroy(); return false; }
    SDL_TrayEntry* recent = SDL_InsertTrayEntryAt(menu, -1, Tr(language, TextId::RecentCapturesTitle), SDL_TRAYENTRY_BUTTON);
    if (!recent) { Destroy(); return false; }
    SDL_SetTrayEntryCallback(recent, [](void* data, SDL_TrayEntry*) {
        static_cast<SystemTray*>(data)->m_recentRequested = true;
    }, this);
    SDL_TrayEntry* latest = SDL_InsertTrayEntryAt(menu, -1, Tr(language, TextId::CopyLatestCapture), SDL_TRAYENTRY_BUTTON);
    if (!latest) { Destroy(); return false; }
    SDL_SetTrayEntryCallback(latest, [](void* data, SDL_TrayEntry*) {
        static_cast<SystemTray*>(data)->m_copyLatestRequested = true;
    }, this);
    SDL_TrayEntry* settings = SDL_InsertTrayEntryAt(menu, -1, Tr(language, TextId::SettingsTitle), SDL_TRAYENTRY_BUTTON);
    if (!settings) { Destroy(); return false; }
    SDL_SetTrayEntryCallback(settings, [](void* data, SDL_TrayEntry*) {
        static_cast<SystemTray*>(data)->m_settingsRequested = true;
    }, this);
    SDL_TrayEntry* historyEntry = SDL_InsertTrayEntryAt(menu, -1, Tr(language, TextId::HistoryTitle), SDL_TRAYENTRY_BUTTON);
    if (!historyEntry) { Destroy(); return false; }
    SDL_SetTrayEntryCallback(historyEntry, [](void* data, SDL_TrayEntry*) { static_cast<SystemTray*>(data)->m_historyRequested = true; }, this);
    SDL_TrayEntry* quitEntry = menu
        ? SDL_InsertTrayEntryAt(menu, -1, Tr(language, TextId::TrayQuit), SDL_TRAYENTRY_BUTTON)
        : nullptr;
    if (!quitEntry) {
        Destroy();
        return false;
    }
    SDL_SetTrayEntryCallback(quitEntry, [](void* userdata, SDL_TrayEntry*) {
        // 回调中只记录请求，避免在系统菜单处理期间拆除窗口和 GPU。
        static_cast<SystemTray*>(userdata)->m_quitRequested = true;
    }, this);
    return true;
#endif
}

bool SystemTray::ConsumeHistoryRequest() { bool value = m_historyRequested; m_historyRequested = false; return value; }
bool SystemTray::ConsumeRecentRequest() { const bool value = m_recentRequested; m_recentRequested = false; return value; }
bool SystemTray::ConsumeCopyLatestRequest() { const bool value = m_copyLatestRequested; m_copyLatestRequested = false; return value; }

SDL_Rect SystemTray::GetAnchor() const {
#if (defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)) && !defined(SHOTLET_TRAY_SDL_BACKEND)
    if (m_native) return NativeSystemTrayAnchor(m_native);
#endif
    float x = 0, y = 0;
    SDL_GetGlobalMouseState(&x, &y);
    return {static_cast<int>(x), static_cast<int>(y), 1, 1};
}

bool SystemTray::ConsumeSettingsRequest() {
    const bool requested = m_settingsRequested;
    m_settingsRequested = false;
    return requested;
}

void SystemTray::UpdateIcon(Theme systemTheme) {
    if (!m_tray && !m_native) return;
    auto icon = MakeTrayIcon(systemTheme);
#if (defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)) && !defined(SHOTLET_TRAY_SDL_BACKEND)
    if (icon && m_native) UpdateNativeSystemTrayIcon(m_native, icon.get());
#endif
    if (icon && m_tray) SDL_SetTrayIcon(m_tray, icon.get());
}

void SystemTray::Destroy() {
#if (defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)) && !defined(SHOTLET_TRAY_SDL_BACKEND)
    DestroyNativeSystemTray(m_native);
    m_native = nullptr;
#endif
    if (m_tray) {
        SDL_DestroyTray(m_tray);
        m_tray = nullptr;
    }
    m_quitRequested = false;
    m_settingsRequested = false;
    m_historyRequested = false;
    m_recentRequested = m_copyLatestRequested = false;
}

} // namespace shotlet
