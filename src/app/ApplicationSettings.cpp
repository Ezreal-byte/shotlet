#include "Application.h"
#include "platform/Appearance.h"
#include "platform/FileService.h"
#include "platform/OcrWindow.h"
#include "platform/PinWindow.h"
#include "ui/OverlayManager.h"
#include "utils/Logger.h"

namespace shotlet {
namespace {
constexpr auto StorageCheckInterval = std::chrono::seconds(1);
constexpr auto ThemeCheckInterval = std::chrono::seconds(1);
}

void Application::ShowSettings() {
    if (m_capturing) { m_settingsPending = true; return; }
    m_settingsPending = false;
    if (!m_settingsWindow) {
        SettingsWindow::Callbacks callbacks;
        callbacks.save = [this](const Preferences& preferences, std::string& error) { return SaveSettings(preferences,error); };
        callbacks.chooseDirectory = [this] { return ChooseExportDirectory(m_language,TextId::SettingsStorageLocation); };
        callbacks.chooseLogDirectory = [this] { return ChooseExportDirectory(m_language, TextId::SettingsLogLocation); };
        callbacks.saveLogDirectory = [this](const std::filesystem::path& directory, std::string& error) {
            return SaveLogDirectory(directory, error);
        };
        callbacks.viewLog = [this](std::string& error) {
            // 打开前刷新缓冲，系统查看器可以立即读到最新日志。
            Logger::Flush();
            const auto file = Logger::GetLogPath();
            std::error_code ec;
            if (std::filesystem::is_regular_file(file, ec) && OpenFileWithDefaultApplication(file)) return true;
            error = Tr(m_language, TextId::SettingsLogOpenFailed);
            return false;
        };
        callbacks.enableHistory = [this](bool enabled,std::string& error) {
            if (!m_history) { error=Tr(m_language,TextId::HistoryUnavailable); return false; }
            const bool saved=m_history->Store().SetEnabled(enabled,error);
            if (saved) m_nextStorageCheck={};
            return saved;
        };
        callbacks.clearHistory = [this](std::string& error) {
            if (!m_history) { error=Tr(m_language,TextId::HistoryUnavailable); return false; }
            if (!m_history->Store().Clear(error)) return false;
            if (m_historyWindow) {
                const bool visible=m_historyWindow->IsVisible();
                m_historyWindow->Close(); m_historyPending=visible;
            }
            m_nextStorageCheck={}; return true;
        };
        m_settingsWindow=std::make_unique<SettingsWindow>(m_gpuDevice,m_preferences,m_language,m_theme,std::move(callbacks));
    }
    RefreshStorageInfo();
    m_settingsWindow->SetLogPath(Logger::GetLogPath());
    if (!m_settingsWindow->Show()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,Tr(m_language,TextId::SettingsTitle),Tr(m_language,TextId::HotkeyWindowFailed),nullptr);
    }
}
void Application::ProcessSettings() {
    if (m_systemTray.ConsumeSettingsRequest() || (m_settingsPending && !m_capturing)) ShowSettings();
    if (m_restoreSettingsWindow && !m_capturing) { m_restoreSettingsWindow=false; ShowSettings(); }
    if (!m_settingsWindow) return;
    m_settingsWindow->SetPreferences(m_preferences,m_language);
    m_settingsWindow->SetLogPath(Logger::GetLogPath());
    if (m_settingsWindow->IsVisible() && std::chrono::steady_clock::now()>=m_nextStorageCheck) RefreshStorageInfo();
    m_settingsWindow->RenderFrame();
}
void Application::RefreshStorageInfo() {
    m_nextStorageCheck=std::chrono::steady_clock::now()+StorageCheckInterval;
    if (!m_settingsWindow) return;
    SettingsWindow::StorageInfo info;
    try {
        if (m_history) {
            info.directory=m_history->Store().Root();
            info.enabled=m_history->Store().GetConfig().enabled;
            const auto records=m_history->Store().List(true); info.count=records.size();
            for (const auto& record:records) info.used+=record.bytes;
            const auto space=std::filesystem::space(info.directory); info.available=space.available; info.capacity=space.capacity;
            info.ready=true;
        }
    } catch (const std::exception& e) { m_settingsWindow->SetError(e.what()); }
    m_settingsWindow->SetStorageInfo(std::move(info));
}
bool Application::SaveLogDirectory(const std::filesystem::path& directory, std::string& error) {
    auto preferences = m_preferences;
    preferences.logDirectory = directory;
    const auto persist = [&] {
        if (SavePreferences(PreferencesPath(), preferences)) return true;
        error = Tr(m_language, TextId::SettingsSaveFailed);
        return false;
    };
    // 即使重选已保存的目录也执行切换，允许用户覆盖本次启动的 --log-dir。
    if (!Logger::ChangeDirectory(directory, persist)) {
        if (error.empty()) error = Tr(m_language, TextId::SettingsLogLocationFailed);
        return false;
    }
    m_preferences = std::move(preferences);
    return true;
}
bool Application::SaveSettings(const Preferences& preferences,std::string& error) {
    if (!preferences.HasValidHotkeys()) {
        error=Tr(m_language,TextId::HotkeySettingsInvalid); return false;
    }
    if (preferences.HasDuplicateHotkeys()) {
        error=Tr(m_language,TextId::SettingsHotkeyDuplicate); return false;
    }
    const bool migrating=preferences.storageDirectory!=m_preferences.storageDirectory;
    if (migrating) {
        if (m_capturing || m_ocrFuture.valid() || m_historyOperation.valid() || m_recentCopy.valid() || ClipboardCopiesBusy()) {
            error=Tr(m_language,TextId::SettingsBusy); return false;
        }
        if (!m_history) { error=Tr(m_language,TextId::HistoryUnavailable); return false; }
        m_recentCaptures.reset();
        m_history->Drain();
        std::unique_ptr<HistoryService> replacement;
        const bool moved=m_history->Store().MigrateTo(preferences.storageDirectory,[&](std::string& message) {
            replacement=std::make_unique<HistoryService>(preferences.storageDirectory);
            if (SavePreferences(PreferencesPath(),preferences)) return true;
            replacement.reset(); message=Tr(m_language,TextId::SettingsSaveFailed); return false;
        },error);
        if (!moved) {
            replacement.reset(); m_history->ResumeAfterDrain();
            if (error=="新目录不能与当前历史目录互相包含") error=Tr(m_language,TextId::SettingsStorageNested);
            else if (error=="目标目录已有截图历史，请选择其他目录") error=Tr(m_language,TextId::SettingsStorageOccupied);
            return false;
        }
        const bool visible=m_historyWindow && m_historyWindow->IsVisible();
        m_historyWindow.reset(); m_history=std::move(replacement); m_historyPending=visible;
        m_historyCopyTextPending.clear(); m_nextStorageCheck={};
    } else {
        const auto oldBindings=m_preferences.Hotkeys();
        const auto newBindings=preferences.Hotkeys();
        bool* registered[]={&m_hotkeyRegistered,&m_historyHotkeyRegistered,&m_copyLatestHotkeyRegistered};
        const bool wasRegistered[]={m_hotkeyRegistered,m_historyHotkeyRegistered,m_copyLatestHotkeyRegistered};
        auto rollback=[&](size_t count) {
            for (size_t i=0;i<count;++i) {
                if (oldBindings[i]==newBindings[i]) continue;
                if (wasRegistered[i]) {
                    std::string message;
                    *registered[i]=HotkeyService::Register(oldBindings[i].modifiers,oldBindings[i].key,message,m_language,HotkeyService::Action(i));
                    if (!*registered[i]) Logger::Error("Hotkey rollback failed: {}",message);
                } else { HotkeyService::Unregister(HotkeyService::Action(i)); *registered[i]=false; }
            }
        };
        for (size_t i=0;i<newBindings.size();++i) {
            if (oldBindings[i]==newBindings[i]) continue;
            if (newBindings[i].IsEnabled() && !HotkeyService::Register(newBindings[i].modifiers,newBindings[i].key,error,m_language,HotkeyService::Action(i))) {
                rollback(i); return false;
            }
            if (!newBindings[i].IsEnabled()) HotkeyService::Unregister(HotkeyService::Action(i));
            *registered[i]=newBindings[i].IsEnabled();
        }
        if (!SavePreferences(PreferencesPath(),preferences)) { rollback(newBindings.size()); error=Tr(m_language,TextId::SettingsSaveFailed); return false; }
    }
    m_preferences=preferences; m_themePreference=preferences.theme;
    const auto language=ResolveLanguage(preferences.language);
    if (language!=m_language) {
        m_language=language;
        if (m_historyWindow) m_historyWindow->SetLanguage(language);
        if (m_recentCaptures) m_recentCaptures->SetLanguage(language);
        m_systemTray.Destroy();
        if (!m_systemTray.Create(language,m_trayTheme.value_or(Theme::Dark))) Logger::Error("Cannot update system tray language: {}",SDL_GetError());
    }
    m_nextThemeCheck={};
    return true;
}
void Application::PollHotkeys() {
    if (!m_running || !m_daemonMode || m_capturing || m_systemTray.IsQuitRequested()) return;
    HotkeyService::SetSuspended(m_settingsWindow && m_settingsWindow->IsRecording());
    if (HotkeyService::Poll(HotkeyService::Action::History)) m_historyPending=true;
    if (HotkeyService::Poll(HotkeyService::Action::CopyLatest)) RequestRecentCopy({}, true);
    if (HotkeyService::Poll()) RequestCapture();
}

void Application::RefreshTheme(bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now < m_nextThemeCheck) return;
    m_nextThemeCheck = now + ThemeCheckInterval;
    // 宿主显式主题不查询系统主题，仅 Auto 时查询；不更新独立应用的托盘外观。
    std::optional<Theme> systemTheme;
    if (!m_hostCaptureOutput || m_themePreference == ThemePreference::Auto)
        systemTheme = QuerySystemTheme();
    if (!m_hostCaptureOutput) {
        const auto trayTheme = QuerySystemTrayTheme();
        if (force || trayTheme != m_trayTheme) {
            m_systemTray.UpdateIcon(trayTheme.value_or(Theme::Dark));
            m_trayTheme = trayTheme;
        }
    }
    const auto theme = ResolveTheme(m_themePreference, systemTheme);
    // SDL 在系统事件中可能重设原生标题栏，显式偏好也需重新应用。
    if (theme == m_theme && !force) return;
    m_theme = theme;
    if (m_overlayManager) m_overlayManager->SetTheme(theme);
    PinWindow::SetTheme(theme);
    OcrWindow::SetTheme(theme);
    if (m_settingsWindow) m_settingsWindow->SetTheme(theme);
    if (m_recentCaptures) m_recentCaptures->SetTheme(theme);
}
} // namespace shotlet
