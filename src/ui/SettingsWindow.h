#pragma once
#include "app/Preferences.h"
#include "utils/WindowChrome.h"
#include <SDL3/SDL.h>
#include <functional>
#include <chrono>
#include <span>

namespace shotlet {
class SettingsWindow {
public:
    struct StorageInfo {
        std::filesystem::path directory;
        uint64_t used = 0, count = 0, available = 0, capacity = 0;
        bool enabled = true, ready = false;
    };
    struct Callbacks {
        std::function<bool(const Preferences&, std::string&)> save;
        std::function<bool(bool, std::string&)> enableHistory;
        std::function<bool(std::string&)> clearHistory;
        std::function<std::filesystem::path()> chooseDirectory;
        std::function<std::filesystem::path()> chooseLogDirectory;
        std::function<bool(const std::filesystem::path&, std::string&)> saveLogDirectory;
        std::function<bool(std::string&)> viewLog;
    };
    SettingsWindow(SDL_GPUDevice* device, Preferences preferences, Language language, Theme theme, Callbacks callbacks);
    ~SettingsWindow();
    bool Show();
    void Hide();
    void Close();
    bool IsVisible() const;
    bool IsRecording() const { return IsVisible() && m_recording >= 0; }
    bool ProcessEvent(const SDL_Event& event);
    void RenderFrame();
    void SetTheme(Theme theme);
    void SetPreferences(const Preferences& preferences, Language language);
    void SetStorageInfo(StorageInfo storage) { m_storage = std::move(storage); }
    void SetLogPath(std::filesystem::path file) { m_logPath = std::move(file); }
    void SetError(std::string error) { m_error = std::move(error); }
private:
    friend class SettingsWindowTest;
    static constexpr int NoRecorder = -1;

    bool CreateWindow();
    bool InitializeImGui();
    void DestroyImGui();
    void UpdateDisplayScale();
    void ResizeForContentScale(float scale);
    void ApplyStyle();
    void HandleWindowAction(WindowChrome::WindowAction action);
    void HandleKeyDown(const SDL_Event& event);

    void RenderContent();
    void RenderSidebar(float width);
    void RenderSidebarItem(int page, float width);
    void RenderPage(float sidebarWidth);
    void RenderPageContent();
    void RenderSavedNotice(float sidebarWidth);
    void RenderError();
    void RenderGeneral();
    void RenderAppearance();
    void RenderLanguage();
    bool RenderChoiceRow(TextId title, TextId hint, const char* id, int& selected,
                         std::span<const char* const> choices);
    void RenderHotkeys();
    void RenderHotkeyRow(int index, TextId label, HotkeyBinding binding);
    void RenderRecorderButton(int index, HotkeyBinding binding);
    void RenderStorage();
    void RenderLogs();
    void RenderLogLocation();
    void ChangeLogDirectory();
    void RenderHistoryPreference();
    void RenderStorageLocation();
    void RenderStorageUsage();
    void RenderClearHistoryButton();
    void RenderClearConfirmation();
    void ConfirmClearHistory();
    void SubmitFrame();
    bool Save(Preferences preferences);
    SDL_GPUDevice* m_device;
    SDL_Window* m_window = nullptr;
    ImGuiContext* m_context = nullptr;
    Preferences m_preferences;
    Language m_language;
    Theme m_theme;
    Callbacks m_callbacks;
    StorageInfo m_storage;
    std::filesystem::path m_logPath;
    int m_page = 0, m_recording = NoRecorder;
    bool m_close = false;
    float m_scale = 1;
    float m_pixelDensity = 0;
    std::string m_error;
    std::chrono::steady_clock::time_point m_savedUntil{};
};
} // namespace shotlet
